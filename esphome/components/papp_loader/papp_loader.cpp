#include "papp_loader.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <limits>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#include "esp_cache.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_mmu_map.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#ifdef USE_ESP32
#include "esp_task_wdt.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "hal/mmu_types.h"
#include "pngAux.h"

// psram_app_handle_t is declared by psram_app.h as a pointer to the global
// C-compatible struct tag. Keep the concrete implementation type at global
// scope so the ABI handle and the C++ implementation refer to the same type.
struct psram_app {
  void *code_buf{nullptr};
  size_t code_alloc{0};
  void *exec_ptr{nullptr};
  bool mapped{false};
  papp_header_t header{};
};

namespace esphome {
namespace papp_loader {

static const char *const TAG = "papp_loader";
static constexpr int VIRTUAL_WIDTH = 800;
static constexpr int VIRTUAL_HEIGHT = 480;
static constexpr int EMU_WIDTH = 320;
static constexpr int EMU_HEIGHT = 240;
static constexpr int LCD_WIDTH = 1024;
static constexpr int LCD_HEIGHT = 600;
static constexpr int LCD_X_OFFSET = (LCD_WIDTH - VIRTUAL_WIDTH) / 2;
static constexpr int LCD_Y_OFFSET = (LCD_HEIGHT - VIRTUAL_HEIGHT) / 2;
static constexpr size_t MMU_PAGE_SIZE = 0x10000;
static constexpr size_t MAX_NETWORK_PAPP_SIZE = 16 * 1024 * 1024;
static constexpr size_t HTTP_READ_BUFFER_SIZE = 16 * 1024;
static constexpr uint32_t HTTP_TIMEOUT_MS = 15000;
static constexpr uint8_t HTTP_MAX_REDIRECTIONS = 5;
static constexpr size_t MAX_CATALOG_SIZE = 64 * 1024;
static constexpr uint16_t SCREEN_STREAM_PORT = 3232;
// This is a diagnostic transport, not the panel's render target.  Keeping it
// below 400 kB/s lets it coexist with Quake's render/audio tasks over the
// board's Ethernet link without holding stale frames in TCP buffers.
static constexpr uint32_t SCREEN_STREAM_INTERVAL_MS = 250;
static constexpr uint16_t SCREEN_STREAM_WIDTH = 100;
static constexpr uint16_t SCREEN_STREAM_HEIGHT = 60;
static constexpr size_t SCREEN_STREAM_FRAME_BYTES = SCREEN_STREAM_WIDTH * SCREEN_STREAM_HEIGHT * sizeof(uint16_t);
static constexpr size_t SCREEN_STREAM_AUDIO_BYTES = 192000;
static constexpr size_t SCREEN_STREAM_AUDIO_PACKET_BYTES = 12288;

struct __attribute__((packed)) ScreenStreamHeader {
  char magic[8];
  uint16_t width;
  uint16_t height;
  uint32_t frame_bytes;
  uint32_t sequence;
};

struct __attribute__((packed)) ScreenStreamAudioHeader {
  char magic[8];
  uint32_t sample_rate;
  uint16_t channels;
  uint16_t sample_bits;
  uint32_t audio_bytes;
  uint32_t sequence;
};

static bool send_screen_stream_bytes(int socket_fd, const void *data, size_t length) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  while (length != 0) {
    const ssize_t sent = send(socket_fd, bytes, length, MSG_NOSIGNAL);
    if (sent <= 0)
      return false;
    bytes += sent;
    length -= static_cast<size_t>(sent);
  }
  return true;
}

// The PAPP canvas occupies the centered 800x480 area. Put the shared close
// control in the unused right-hand margin so it cannot cover gameplay. The
// panel's raw draw path is hardware-flipped, so the raw destination is the
// 180-degree counterpart of the desired physical screen position.
static constexpr int CLOSE_BUTTON_SCREEN_X = LCD_X_OFFSET + VIRTUAL_WIDTH + 12;
static constexpr int CLOSE_BUTTON_SCREEN_Y = LCD_Y_OFFSET + 10;
static constexpr int CLOSE_BUTTON_SIZE = 58;
static constexpr int CLOSE_BUTTON_HIT_PADDING = 10;
static constexpr int CLOSE_BUTTON_RAW_X = LCD_WIDTH - CLOSE_BUTTON_SCREEN_X - CLOSE_BUTTON_SIZE;
static constexpr int CLOSE_BUTTON_RAW_Y = LCD_HEIGHT - CLOSE_BUTTON_SCREEN_Y - CLOSE_BUTTON_SIZE;
alignas(64) static uint16_t close_overlay_buffer[CLOSE_BUTTON_SIZE * CLOSE_BUTTON_SIZE] = {};

// A PAPP may request a large game stack.  Those stacks must live in PSRAM and
// must be deleted through ESP-IDF's matching WithCaps API.  Keep the handles
// private to the service layer so the PAPP ABI can continue to expose one
// task_create/task_delete pair for both ordinary and PSRAM-backed tasks.
static constexpr size_t MAX_PAPP_CAP_TASKS = 16;
static TaskHandle_t papp_cap_task_handles[MAX_PAPP_CAP_TASKS] = {};

static bool remember_papp_cap_task(TaskHandle_t handle) {
  if (handle == nullptr)
    return false;
  for (size_t i = 0; i < MAX_PAPP_CAP_TASKS; i++) {
    if (papp_cap_task_handles[i] == nullptr) {
      papp_cap_task_handles[i] = handle;
      return true;
    }
  }
  ESP_LOGE("PAPP", "PSRAM task registry is full; task cleanup may leak");
  return false;
}

static bool forget_papp_cap_task(TaskHandle_t handle) {
  if (handle == nullptr)
    return false;
  for (size_t i = 0; i < MAX_PAPP_CAP_TASKS; i++) {
    if (papp_cap_task_handles[i] == handle) {
      papp_cap_task_handles[i] = nullptr;
      return true;
    }
  }
  return false;
}

PappLoader *PappLoader::active_ = nullptr;

static std::string runtime_path(const char *path) {
  if (path == nullptr)
    return {};
  std::string result(path);
  if (result == "/sd")
    return "/sdcard";
  if (result.rfind("/sd/", 0) == 0)
    return "/sdcard/" + result.substr(4);
  return result;
}

static size_t align_up(size_t value, size_t alignment) {
  if (value > std::numeric_limits<size_t>::max() - (alignment - 1))
    return 0;
  return (value + alignment - 1) & ~(alignment - 1);
}

static bool is_network_url(const char *value) {
  if (value == nullptr)
    return false;
  return std::strncmp(value, "http://", 7) == 0 || std::strncmp(value, "https://", 8) == 0;
}

static esp_err_t fetch_http_text(const char *url, std::string *out);
static std::vector<std::pair<std::string, std::string>> parse_papp_catalog(const std::string &html,
                                                                             const std::string &base_url);

#ifdef PAPP_LOADER_USE_LVGL
struct PappCatalogButtonContext {
  PappLoader *loader;
  std::string url;
};

static void papp_catalog_button_event_cb(lv_event_t *event) {
  if (event == nullptr)
    return;
  auto *context = static_cast<PappCatalogButtonContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;

  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    ESP_LOGI("papp_loader", "Catalog selection: %s", context->url.c_str());
    context->loader->request_launch_url(context->url);
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}
#endif

#ifdef PAPP_LOADER_USE_USB_HIDX
static bool usb_keyboard_key_pressed(usb_hidx::USBHIDXComponent *component, uint8_t keycode) {
#ifdef USE_BINARY_SENSOR
  if (component == nullptr || keycode == 0)
    return false;
  auto &sensors = component->get_keyboard_key_sensors();
  auto it = sensors.find(keycode);
  return it != sensors.end() && it->second != nullptr && it->second->get_state();
#else
  (void) component;
  (void) keycode;
  return false;
#endif
}

static bool usb_mouse_button_pressed(binary_sensor::BinarySensor *sensor) {
  return sensor != nullptr && sensor->get_state();
}

#endif

void PappLoader::setup() {
  if (PappLoader::active_ != nullptr) {
    ESP_LOGE(TAG, "Only one PAPP loader can be active");
    this->mark_failed();
    return;
  }
  if (this->display_ == nullptr) {
    ESP_LOGE(TAG, "A display_id is required");
    this->mark_failed();
    return;
  }

  this->framebuffer_ = static_cast<uint16_t *>(heap_caps_aligned_calloc(
      64, 1, VIRTUAL_WIDTH * VIRTUAL_HEIGHT * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  this->rotated_framebuffer_ = static_cast<uint16_t *>(heap_caps_aligned_calloc(
      64, 1, VIRTUAL_WIDTH * VIRTUAL_HEIGHT * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  this->ppa_framebuffer_ = static_cast<uint16_t *>(heap_caps_aligned_calloc(
      64, 1, VIRTUAL_WIDTH * VIRTUAL_HEIGHT * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  this->emu_buffer_ = static_cast<uint16_t *>(heap_caps_aligned_calloc(
      64, 1, EMU_WIDTH * EMU_HEIGHT * sizeof(uint16_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  if (this->emu_buffer_ == nullptr) {
    this->emu_buffer_ = static_cast<uint16_t *>(heap_caps_aligned_calloc(
        64, 1, EMU_WIDTH * EMU_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  }
  this->stream_framebuffer_ = static_cast<uint16_t *>(heap_caps_aligned_calloc(
      64, 1, SCREEN_STREAM_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  this->stream_frame_packet_ = static_cast<uint16_t *>(heap_caps_aligned_calloc(
      64, 1, SCREEN_STREAM_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  this->stream_audio_buffer_ = static_cast<uint8_t *>(heap_caps_aligned_calloc(
      64, 1, SCREEN_STREAM_AUDIO_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  this->stream_audio_packet_ = static_cast<uint8_t *>(heap_caps_aligned_calloc(
      64, 1, SCREEN_STREAM_AUDIO_PACKET_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  this->stream_mutex_ = xSemaphoreCreateMutex();
  this->display_mutex_ = xSemaphoreCreateRecursiveMutex();
  this->keyboard_mutex_ = xSemaphoreCreateMutex();
  this->report_mutex_ = xSemaphoreCreateMutex();
  this->clear_mouse_delta_();

  if (this->framebuffer_ == nullptr || this->rotated_framebuffer_ == nullptr || this->ppa_framebuffer_ == nullptr || this->emu_buffer_ == nullptr ||
      this->display_mutex_ == nullptr ||
      this->keyboard_mutex_ == nullptr) {
    ESP_LOGE(TAG, "PAPP framebuffer allocation failed");
    this->mark_failed();
    return;
  }

  // Register one blocking SRM client for the shared PAPP flush path. The
  // native launcher uses the same operation to rotate the landscape canvas;
  // keeping it here avoids walking all 384,000 pixels on the CPU each frame.
  ppa_client_config_t ppa_config = {
      .oper_type = PPA_OPERATION_SRM,
      .max_pending_trans_num = 1,
  };
  ppa_client_handle_t ppa_client = nullptr;
  if (ppa_register_client(&ppa_config, &ppa_client) == ESP_OK) {
    this->ppa_srm_client_ = ppa_client;
    ESP_LOGI(TAG, "PAPP hardware SRM flush enabled");
  } else {
    ESP_LOGW(TAG, "PAPP hardware SRM unavailable; using software rotation");
  }

  PappLoader::active_ = this;
  if (this->stream_framebuffer_ != nullptr && this->stream_frame_packet_ != nullptr &&
      this->stream_audio_buffer_ != nullptr && this->stream_audio_packet_ != nullptr && this->stream_mutex_ != nullptr) {
    // PAPP games such as Quake run their main loop at priority 5 on core 0.
    // A lower-priority stream task can be starved indefinitely even though it
    // deliberately sleeps between packets. Match that priority so FreeRTOS
    // time-slices it, then yield for 200 ms after every packet batch.
    const BaseType_t result = xTaskCreatePinnedToCore(
        &PappLoader::screen_stream_task_entry_, "papp_stream", 8192, this, 5,
        &this->stream_task_handle_, 0);
    if (result != pdPASS)
      ESP_LOGW(TAG, "Could not start optional screen stream task");
    else
      ESP_LOGI(TAG, "Diagnostic screen stream listening on TCP port %u", SCREEN_STREAM_PORT);
  }
  this->clear_(0x0000);

  if (this->toggle_button_ != nullptr) {
    // The callback latches a short press so it cannot be lost between PAPP
    // frames. PAPP runs in a worker task while ESPHome keeps servicing USB,
    // GPIO/ADC, touchscreen, audio, and network components.
    // Keep the close request sticky until the PAPP has observed it; otherwise
    // a quick press/release between two PAPP frames can be lost.
    this->toggle_button_->add_on_state_callback([this](bool state) {
      this->toggle_button_state_ = state;
      if (state) {
        if (this->launched_) {
          this->toggle_close_requested_ = true;
          ESP_LOGI(TAG, "PAPP toggle close request latched");
        }
      } else if (!this->launched_) {
        this->toggle_wait_release_ = false;
        this->toggle_close_requested_ = false;
      }
    });
  }
  if (this->launch_button_ != nullptr) {
    // Opening has its own control so a close press cannot immediately start
    // the PAPP again. The request is latched by the binary-sensor edge and
    // the level is also checked in loop() as a fallback.
    this->launch_button_->add_on_state_callback([this](bool state) {
      this->launch_button_state_ = state;
      if (state) {
        if (!this->launched_ && !this->launch_wait_release_) {
          this->launch_pending_ = true;
          this->launch_wait_release_ = true;
          ESP_LOGI(TAG, "PAPP launch request latched from launch button");
        }
      } else {
        this->launch_wait_release_ = false;
      }
    });
  }
  ESP_LOGCONFIG(TAG, "PAPP runtime ready");
  ESP_LOGCONFIG(TAG, "  Path: %s", this->path_.c_str());
  const std::string mapped = runtime_path(this->path_.c_str());
  ESP_LOGCONFIG(TAG, "  SD path: %s", mapped.c_str());
  ESP_LOGCONFIG(TAG, "  Display: %dx%d virtual, centered on %dx%d LCD", VIRTUAL_WIDTH, VIRTUAL_HEIGHT,
                LCD_WIDTH, LCD_HEIGHT);
#ifdef PAPP_LOADER_USE_USB_HIDX
  const char *usb_status = this->usb_hidx_ ? "configured" : "disabled";
#else
  const char *usb_status = "disabled";
#endif
  ESP_LOGCONFIG(TAG, "  Touch: %s, audio: %s, USB input: %s", this->touchscreen_ ? "configured" : "disabled",
                this->speaker_ ? "configured" : "disabled", usb_status);

  if (!this->catalog_url_.empty()) {
    // Network may still be negotiating when setup() runs.  Fetch the HTML
    // index after the rest of ESPHome has had time to bring Ethernet/Wi-Fi up.
    this->set_timeout("papp_catalog_initial", 5000, [this]() { this->refresh_catalog(); });
  }

  if (this->autostart_) {
    // Let USB host enumeration complete before an optional autostart.
    // Official Switch Pro controllers need several report/command exchanges
    // after enumeration.  Give that handshake time to finish while the
    // normal ESPHome loop owns USB before the PAPP worker starts.
    this->set_timeout("papp_autostart", 5000, [this]() { this->launch_pending_ = true; });
  }
}

void PappLoader::loop() {
  if (this->catalog_loading_ && this->catalog_done_) {
    if (this->papp_catalog_task_handle_ != nullptr) {
      vTaskDelete(this->papp_catalog_task_handle_);
      this->papp_catalog_task_handle_ = nullptr;
    }
    this->catalog_loading_ = false;
    if (this->catalog_result_ == ESP_OK) {
      this->catalog_entries_ = parse_papp_catalog(this->catalog_html_, this->catalog_url_);
      ESP_LOGI(TAG, "PAPP catalog found %u application(s)",
               static_cast<unsigned>(this->catalog_entries_.size()));
      for (const auto &entry : this->catalog_entries_)
        ESP_LOGI(TAG, "  PAPP: %s -> %s", entry.first.c_str(), entry.second.c_str());
#ifdef PAPP_LOADER_USE_LVGL
      this->catalog_ui_pending_ = true;
#endif
    } else {
      ESP_LOGW(TAG, "PAPP catalog request failed: %s (0x%x)",
               esp_err_to_name(static_cast<esp_err_t>(this->catalog_result_)), this->catalog_result_);
    }
  }
#ifdef PAPP_LOADER_USE_LVGL
  if (this->catalog_ui_pending_ && this->catalog_container_ != nullptr && this->lvgl_ != nullptr &&
      this->lvgl_->is_loop_started()) {
    this->update_catalog_ui_();
    this->catalog_ui_pending_ = false;
  }
  if (!this->launched_ && this->catalog_container_ != nullptr && this->lvgl_ != nullptr &&
      this->lvgl_->is_loop_started()) {
    this->handle_launcher_controls_();
  }
#endif

  // After a PAPP exits, wait for a real release before accepting the next
  // press. The state callback above latches short presses, while this level
  // check clears the guard even when no new HID report has arrived yet.
  if (!this->launched_ && this->toggle_button_ != nullptr) {
    const bool pressed = this->toggle_button_->get_state();
    if (!pressed) {
      this->toggle_wait_release_ = false;
      this->toggle_close_requested_ = false;
    }
    this->toggle_button_state_ = pressed;
  }

  if (!this->launched_ && this->launch_button_ != nullptr) {
    const bool pressed = this->launch_button_->get_state();
    if (!pressed)
      this->launch_wait_release_ = false;
    if (pressed && !this->launch_button_state_ && !this->launch_wait_release_) {
      this->launch_pending_ = true;
      this->launch_wait_release_ = true;
    }
    this->launch_button_state_ = pressed;
  }

  // Poll this independently of the app. Some small PAPPs do not call the
  // optional touch service themselves, but the shared close control must
  // still work for them.
  if (this->launched_)
    this->poll_close_button_();

  if (this->launched_) {
    if (this->papp_loading_) {
      if (!this->papp_load_done_)
        return;

      if (this->papp_load_task_handle_ != nullptr) {
        vTaskDelete(this->papp_load_task_handle_);
        this->papp_load_task_handle_ = nullptr;
      }
      this->papp_loading_ = false;

      if (this->papp_load_result_ != ESP_OK || this->papp_load_handle_ == nullptr) {
        ESP_LOGE(TAG, "Network PAPP load failed: %s (0x%x)",
                 esp_err_to_name(static_cast<esp_err_t>(this->papp_load_result_)), this->papp_load_result_);
        this->papp_load_handle_ = nullptr;
        this->launched_ = false;
        this->begin_report_(this->papp_load_source_);
        this->send_report_("load_failed", this->papp_load_result_, this->papp_load_source_);
        return;
      }

      psram_app_handle_t app = this->papp_load_handle_;
      this->papp_load_handle_ = nullptr;
      const std::string source = this->papp_load_source_;
      if (!this->start_loaded_app_(app, source))
        this->launched_ = false;
      return;
    }
    if (this->papp_task_done_)
      this->finish_app_();
    return;
  }

  if (!this->launch_pending_ || this->path_.empty())
    return;
  ESP_LOGI(TAG, "Processing launch request: %s", this->path_.c_str());
  this->launch_pending_ = false;
  this->launched_ = true;
  this->toggle_close_requested_ = false;
  this->global_close_requested_ = false;
  this->clear_keyboard_queue_();
  this->clear_mouse_delta_();
  this->launch_();
}

void PappLoader::dump_config() {
  ESP_LOGCONFIG(TAG, "PAPP loader");
  ESP_LOGCONFIG(TAG, "  Path: %s", this->path_.c_str());
  ESP_LOGCONFIG(TAG, "  Autostart: %s", YESNO(this->autostart_));
  ESP_LOGCONFIG(TAG, "  Network PAPPs: enabled with papp_loader.launch_url");
  if (!this->catalog_url_.empty())
    ESP_LOGCONFIG(TAG, "  PAPP catalog: %s", this->catalog_url_.c_str());
}

void PappLoader::refresh_catalog() {
  if (this->catalog_url_.empty()) {
    ESP_LOGW(TAG, "Cannot refresh PAPP catalog without catalog_url");
    return;
  }
  if (this->catalog_loading_) {
    ESP_LOGD(TAG, "PAPP catalog request already in progress");
    return;
  }
  if (!network::is_connected()) {
    ESP_LOGW(TAG, "Cannot refresh PAPP catalog while the device is offline");
    return;
  }

  this->catalog_html_.clear();
  this->catalog_done_ = false;
  this->catalog_result_ = -1;
  this->catalog_loading_ = true;
  const BaseType_t task_result = xTaskCreatePinnedToCore(
      &PappLoader::papp_catalog_task_entry_, "papp_catalog", 8192, this, 4,
      &this->papp_catalog_task_handle_, 0);
  if (task_result != pdPASS) {
    this->catalog_loading_ = false;
    this->papp_catalog_task_handle_ = nullptr;
    ESP_LOGE(TAG, "Could not create PAPP catalog task");
    return;
  }
  ESP_LOGI(TAG, "PAPP catalog request started: %s", this->catalog_url_.c_str());
}

bool PappLoader::launch_() {
  const std::string source = this->path_;
  if (is_network_url(source.c_str())) {
    if (!network::is_connected()) {
      ESP_LOGE(TAG, "Cannot load network PAPP while the device is offline: %s", source.c_str());
      this->launched_ = false;
      return false;
    }

    // Download in its own task. HTTP/TLS setup and a several-megabyte PAPP
    // transfer must not block the ESPHome loop, USB host, LVGL, or watchdog.
    this->papp_load_source_ = source;
    this->papp_load_handle_ = nullptr;
    this->papp_load_done_ = false;
    this->papp_load_result_ = -1;
    this->papp_loading_ = true;
    const BaseType_t task_result = xTaskCreatePinnedToCore(
        &PappLoader::papp_load_task_entry_, "papp_http", 12288, this, 5,
        &this->papp_load_task_handle_, 0);
    if (task_result != pdPASS) {
      ESP_LOGE(TAG, "Could not create network PAPP loader task");
      this->papp_loading_ = false;
      this->launched_ = false;
      return false;
    }
    ESP_LOGI(TAG, "Network PAPP download started: %s", source.c_str());
    return true;
  }

  const std::string path = runtime_path(source.c_str());
  ESP_LOGI(TAG, "Opening selected PAPP: %s", path.c_str());
  struct stat st{};
  if (stat(path.c_str(), &st) != 0) {
    ESP_LOGE(TAG, "PAPP file not found: %s", path.c_str());
    this->launched_ = false;
    return false;
  }

  psram_app_handle_t app = nullptr;
  esp_err_t err = psram_app_load(path.c_str(), &app);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "PAPP load failed: %s (0x%x)", esp_err_to_name(err), err);
    this->launched_ = false;
    return false;
  }

  return this->start_loaded_app_(app, path);
}

bool PappLoader::start_loaded_app_(psram_app_handle_t app, const std::string &source) {
  if (app == nullptr)
    return false;

  ESP_LOGI(TAG, "Starting PAPP in worker task: %s", source.c_str());
  this->begin_report_(source);

#ifdef PAPP_LOADER_USE_LVGL
  // LVGL must not draw over the PAPP framebuffer. Pause it from the ESPHome
  // loop task; the PAPP worker never calls LVGL directly.
  if (this->lvgl_ != nullptr && this->lvgl_->is_loop_started()) {
    // Direct-rendered PAPPs share the launcher LVGL display but have no LVGL
    // object of their own. Put an invisible clickable object on top of the
    // launcher so a release cannot activate or invalidate a button underneath
    // the PAPP between its frame updates.
    if (this->touch_modal_shield_ == nullptr) {
      lv_obj_t *screen = this->lvgl_->get_screen_active();
      if (screen != nullptr) {
        this->touch_modal_shield_ = lv_obj_create(screen);
        lv_obj_remove_style_all(this->touch_modal_shield_);
        lv_obj_set_size(this->touch_modal_shield_, LV_PCT(100), LV_PCT(100));
        lv_obj_center(this->touch_modal_shield_);
        lv_obj_add_flag(this->touch_modal_shield_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            this->touch_modal_shield_,
            [](lv_event_t *event) { lv_event_stop_bubbling(event); }, LV_EVENT_ALL, nullptr);
      }
    }
    // Discard any launcher touch that was held during the transition. The
    // PAPP owns the touch surface while it is running; a stale LVGL press
    // must not be replayed against a launcher button after it closes.
    lv_indev_reset(nullptr, nullptr);
    // PAPPs may call lv_timer_handler() on the shared LVGL instance. Disable
    // the launcher input devices explicitly; pausing ESPHome's LVGL loop
    // alone does not stop those devices from being processed by the PAPP.
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
      lv_indev_enable(indev, false);
    }
    this->lvgl_->set_paused(true, false);
  }
#endif

  this->app_handle_ = app;
  this->papp_task_done_ = false;
  this->papp_task_result_ = -1;
  const BaseType_t task_result = xTaskCreatePinnedToCore(
      &PappLoader::papp_task_entry_, "papp_main", 16384, this, 5,
      &this->papp_task_handle_, 0);
  if (task_result != pdPASS) {
    ESP_LOGE(TAG, "Could not create PAPP worker task");
    this->app_handle_ = nullptr;
    this->papp_task_handle_ = nullptr;
    this->launched_ = false;
    this->send_report_("start_failed", -1, source);
#ifdef PAPP_LOADER_USE_LVGL
    if (this->lvgl_ != nullptr && this->lvgl_->is_loop_started())
      this->lvgl_->set_paused(false, false);
#endif
    psram_app_unload(app);
    return false;
  }
  ESP_LOGI(TAG, "PAPP worker task started on core 0; ESPHome loop remains active");
  return true;
}

void PappLoader::enqueue_keyboard_event_(int key, bool down) {
  if (!this->launched_ || key <= 0 || this->keyboard_mutex_ == nullptr)
    return;

  // USB HIDX and the PAPP worker can arrive here on different cores.  The
  // critical section is tiny; wait briefly instead of silently dropping a
  // character when the reader currently owns the mutex.
  if (xSemaphoreTake(this->keyboard_mutex_, pdMS_TO_TICKS(5)) != pdTRUE) {
    ESP_LOGW(TAG, "USB keyboard queue lock timeout; dropped key=%d", key);
    return;
  }
  const size_t next = (this->keyboard_head_ + 1) % KEYBOARD_QUEUE_SIZE;
  if (next == this->keyboard_tail_) {
    // Drop the oldest event rather than blocking the ESPHome loop task.
    this->keyboard_tail_ = (this->keyboard_tail_ + 1) % KEYBOARD_QUEUE_SIZE;
    ESP_LOGW(TAG, "USB keyboard event queue full; dropped oldest event");
  }
  this->keyboard_queue_[this->keyboard_head_] = {key, down ? 1 : 0};
  this->keyboard_head_ = next;
  xSemaphoreGive(this->keyboard_mutex_);
  ESP_LOGD(TAG, "Queued PAPP keyboard key=%d %s", key, down ? "down" : "up");
}

void PappLoader::enqueue_keyboard_tap_(int key) {
  // The USB text sensor reports characters, not a complete HID key lifecycle.
  // A tap is correct for console text and prevents printable keys from being
  // left held in Quake after a map/portal transition. Continuous controls
  // (WASD, arrows, etc.) are supplied independently by read_input_().
  this->enqueue_keyboard_event_(key, true);
  this->enqueue_keyboard_event_(key, false);
}

void PappLoader::enqueue_keyboard_text(const std::string &text) {
  if (text.empty())
    return;

  // Arrow keys already have a complete held-state path through the configured
  // USB binary sensors and read_input_(). Queueing the text sensor's named
  // arrow tap as well makes Quake menus advance twice for one key press.
  if (text == "Up" || text == "Down" || text == "Left" || text == "Right")
    return;

  int key = 0;
  if (text == "Backspace")
    key = 127;
  else if (text == "Tab")
    key = 9;
  else if (text == "Enter")
    key = 13;
  else if (text == "Escape")
    key = 27;
  else if (text == "Left Alt" || text == "Right Alt")
    key = 132;
  else if (text == "Left Ctrl" || text == "Right Ctrl")
    key = 133;
  else if (text == "Left Shift" || text == "Right Shift")
    key = 134;
  else if (text == "F1")
    key = 135;
  else if (text == "F2")
    key = 136;
  else if (text == "F3")
    key = 137;
  else if (text == "F4")
    key = 138;
  else if (text == "F5")
    key = 139;
  else if (text == "F6")
    key = 140;
  else if (text == "F7")
    key = 141;
  else if (text == "F8")
    key = 142;
  else if (text == "F9")
    key = 143;
  else if (text == "F10")
    key = 144;
  else if (text == "F11")
    key = 145;
  else if (text == "F12")
    key = 146;
  else if (text == "Insert")
    key = 147;
  else if (text == "Delete")
    key = 148;
  else if (text == "Page Down")
    key = 149;
  else if (text == "Page Up")
    key = 150;
  else if (text == "Home")
    key = 151;
  else if (text == "End")
    key = 152;
  else if (text.size() == 1)
    key = static_cast<unsigned char>(text[0]);

  if (key != 0)
    this->enqueue_keyboard_tap_(key);
}

void PappLoader::enqueue_mouse_delta(float dx, float dy) {
  if (!std::isfinite(dx) || !std::isfinite(dy))
    return;

  // HID mouse reports are int8 deltas, but keep the public callback float so
  // the same path also works for touchpads and scaled HIDX reports.
  const int32_t x = static_cast<int32_t>(std::lround(std::max(-32768.0f, std::min(32767.0f, dx))));
  const int32_t y = static_cast<int32_t>(std::lround(std::max(-32768.0f, std::min(32767.0f, dy))));
  if (x == 0 && y == 0)
    return;

  portENTER_CRITICAL(&this->mouse_input_lock_);
  const int64_t next_x = static_cast<int64_t>(this->mouse_dx_accum_) + x;
  const int64_t next_y = static_cast<int64_t>(this->mouse_dy_accum_) + y;
  this->mouse_dx_accum_ = static_cast<int32_t>(std::max<int64_t>(-32768, std::min<int64_t>(32767, next_x)));
  this->mouse_dy_accum_ = static_cast<int32_t>(std::max<int64_t>(-32768, std::min<int64_t>(32767, next_y)));
  portEXIT_CRITICAL(&this->mouse_input_lock_);
}

void PappLoader::clear_keyboard_queue_() {
  if (this->keyboard_mutex_ == nullptr)
    return;
  if (xSemaphoreTake(this->keyboard_mutex_, portMAX_DELAY) == pdTRUE) {
    this->keyboard_head_ = 0;
    this->keyboard_tail_ = 0;
    xSemaphoreGive(this->keyboard_mutex_);
  }
}

void PappLoader::clear_mouse_delta_() {
  portENTER_CRITICAL(&this->mouse_input_lock_);
  this->mouse_dx_accum_ = 0;
  this->mouse_dy_accum_ = 0;
  portEXIT_CRITICAL(&this->mouse_input_lock_);
}

int PappLoader::read_mouse_(int *dx, int *dy, int *buttons) {
  int32_t x = 0;
  int32_t y = 0;
  portENTER_CRITICAL(&this->mouse_input_lock_);
  x = this->mouse_dx_accum_;
  y = this->mouse_dy_accum_;
  this->mouse_dx_accum_ = 0;
  this->mouse_dy_accum_ = 0;
  portEXIT_CRITICAL(&this->mouse_input_lock_);

  if (dx != nullptr)
    *dx = x;
  if (dy != nullptr)
    *dy = y;

  int state = 0;
#ifdef PAPP_LOADER_USE_USB_HIDX
  if (this->usb_hidx_ != nullptr) {
    if (this->usb_hidx_->get_mouse_left_sensor() != nullptr &&
        this->usb_hidx_->get_mouse_left_sensor()->get_state())
      state |= 0x01;
    if (this->usb_hidx_->get_mouse_right_sensor() != nullptr &&
        this->usb_hidx_->get_mouse_right_sensor()->get_state())
      state |= 0x02;
    if (this->usb_hidx_->get_mouse_middle_sensor() != nullptr &&
        this->usb_hidx_->get_mouse_middle_sensor()->get_state())
      state |= 0x04;
  }
#endif
  if (buttons != nullptr)
    *buttons = state;

  // Return a sample every frame while USB HIDX is configured, even when the
  // delta is zero, so mouse-button releases are visible to Quake.
#ifdef PAPP_LOADER_USE_USB_HIDX
  return this->usb_hidx_ != nullptr ? 1 : 0;
#else
  return 0;
#endif
}

int PappLoader::read_keyboard_(papp_keyboard_event_t *event) {
  if (event == nullptr || this->keyboard_mutex_ == nullptr)
    return 0;
  if (xSemaphoreTake(this->keyboard_mutex_, 0) != pdTRUE)
    return 0;
  if (this->keyboard_head_ == this->keyboard_tail_) {
    xSemaphoreGive(this->keyboard_mutex_);
    return 0;
  }
  *event = this->keyboard_queue_[this->keyboard_tail_];
  this->keyboard_tail_ = (this->keyboard_tail_ + 1) % KEYBOARD_QUEUE_SIZE;
  xSemaphoreGive(this->keyboard_mutex_);
  return 1;
}

void PappLoader::papp_task_entry_(void *arg) {
  auto *self = static_cast<PappLoader *>(arg);
  self->papp_task_result_ = psram_app_run(self->app_handle_);
  self->papp_task_done_ = true;
  // Cleanup is owned by ESPHome's loop task. Do not return from a FreeRTOS
  // task, because the port's task-exit path treats that as an abort.
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void PappLoader::papp_load_task_entry_(void *arg) {
  auto *self = static_cast<PappLoader *>(arg);
  const std::string url = self->papp_load_source_;
  self->papp_load_result_ = psram_app_load_url(url.c_str(), &self->papp_load_handle_);
  self->papp_load_done_ = true;
  // Cleanup is owned by ESPHome's loop task. Keeping this task alive until
  // the loop deletes it avoids the ESP-IDF task-exit abort path used by the
  // PAPP worker as well.
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void PappLoader::papp_catalog_task_entry_(void *arg) {
  auto *self = static_cast<PappLoader *>(arg);
  const std::string url = self->catalog_url_;
  self->catalog_result_ = fetch_http_text(url.c_str(), &self->catalog_html_);
  self->catalog_done_ = true;
  // As with the PAPP loader task, the ESPHome loop deletes this task after it
  // has consumed the result. Returning from an IDF task is not safe on this
  // target and can trigger the abort path.
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

#ifdef PAPP_LOADER_USE_LVGL
void PappLoader::set_catalog_selection_(uint16_t index) {
  if (this->catalog_container_ == nullptr)
    return;

  const uint32_t count = lv_obj_get_child_count(this->catalog_container_);
  if (count == 0) {
    this->catalog_selection_ = 0;
    return;
  }
  if (index >= count)
    index = static_cast<uint16_t>(count - 1);
  this->catalog_selection_ = index;

  for (uint32_t i = 0; i < count; i++) {
    lv_obj_t *child = lv_obj_get_child(this->catalog_container_, static_cast<int32_t>(i));
    if (child != nullptr)
      lv_obj_remove_state(child, LV_STATE_FOCUSED);
  }
  lv_obj_t *selected = lv_obj_get_child(this->catalog_container_, static_cast<int32_t>(index));
  if (selected != nullptr) {
    lv_obj_add_state(selected, LV_STATE_FOCUSED);
    lv_obj_scroll_to_view(selected, LV_ANIM_OFF);
  }
}

void PappLoader::handle_launcher_controls_() {
  // The launcher uses the same controls as the PAPP runtime.  Detect edges
  // here instead of treating a held HID report or ADC value as repeated
  // presses every ESPHome loop iteration.
  uint8_t direction = 0;
  auto pressed = [this](uint8_t index) {
    return this->buttons_[index] != nullptr && this->buttons_[index]->get_state();
  };
  if (pressed(PAPP_INPUT_UP))
    direction |= 1U << 0;
  if (pressed(PAPP_INPUT_RIGHT))
    direction |= 1U << 1;
  if (pressed(PAPP_INPUT_DOWN))
    direction |= 1U << 2;
  if (pressed(PAPP_INPUT_LEFT))
    direction |= 1U << 3;

  // GPIO16 is the Elecrow resistor ladder. Keep the same thresholds used by
  // the original Elecrow ESPHome PAPP configuration, including diagonals.
  if (this->adc_button_sensor_ != nullptr) {
    const float voltage = this->adc_button_sensor_->get_state();
    if (voltage < 1.48f) {
      direction |= (1U << 2) | (1U << 3);  // down + left
    } else if (voltage < 1.60f) {
      direction |= (1U << 0) | (1U << 3);  // up + left
    } else if (voltage < 1.75f) {
      direction |= 1U << 3;  // left
    } else if (voltage < 1.95f) {
      direction |= (1U << 2) | (1U << 1);  // down + right
    } else if (voltage < 2.10f) {
      direction |= (1U << 0) | (1U << 1);  // up + right
    } else if (voltage < 2.40f) {
      direction |= 1U << 1;  // right
    } else if (voltage < 2.75f) {
      direction |= 1U << 2;  // down
    } else if (voltage < 3.10f) {
      direction |= 1U << 0;  // up
    }
  }

  const bool a = pressed(PAPP_INPUT_A);
  const bool touch = this->touch_button_ != nullptr && this->touch_button_->get_state();
  const bool active = direction != 0 || a || touch;

  // Do not let a controller that is held during cold boot select or scroll a
  // PAPP before the user has released it once. This is the same protection
  // used for the normal launch button and avoids the old phantom-input loop.
  if (!this->launcher_input_armed_) {
    if (!active) {
      this->launcher_input_armed_ = true;
      this->launcher_direction_state_ = 0;
      this->launcher_a_state_ = false;
      this->launcher_touch_state_ = false;
      ESP_LOGD(TAG, "Launcher controls armed after input release");
    } else {
      this->launcher_direction_state_ = direction;
      this->launcher_a_state_ = a;
      this->launcher_touch_state_ = touch;
    }
    return;
  }

  const uint8_t newly_pressed = direction & static_cast<uint8_t>(~this->launcher_direction_state_);
  if (!this->catalog_entries_.empty() && newly_pressed != 0) {
    const uint32_t count = this->catalog_entries_.size();
    uint16_t next = this->catalog_selection_;
    if ((newly_pressed & (1U << 0)) != 0) {
      next = next == 0 ? static_cast<uint16_t>(count - 1) : static_cast<uint16_t>(next - 1);
      this->set_catalog_selection_(next);
      ESP_LOGI(TAG, "PAPP catalog selection: %u/%u (up)", static_cast<unsigned>(next + 1),
               static_cast<unsigned>(count));
    } else if ((newly_pressed & (1U << 2)) != 0) {
      next = next + 1 >= count ? 0 : static_cast<uint16_t>(next + 1);
      this->set_catalog_selection_(next);
      ESP_LOGI(TAG, "PAPP catalog selection: %u/%u (down)", static_cast<unsigned>(next + 1),
               static_cast<unsigned>(count));
    }
  }

  const bool select_pressed = (a && !this->launcher_a_state_) || (touch && !this->launcher_touch_state_);
  if (select_pressed && !this->catalog_entries_.empty()) {
    lv_obj_t *selected = lv_obj_get_child(this->catalog_container_, static_cast<int32_t>(this->catalog_selection_));
    if (selected != nullptr) {
      ESP_LOGI(TAG, "PAPP catalog physical select: %u/%u", static_cast<unsigned>(this->catalog_selection_ + 1),
               static_cast<unsigned>(this->catalog_entries_.size()));
      lv_obj_send_event(selected, LV_EVENT_CLICKED, nullptr);
    }
  }

  this->launcher_direction_state_ = direction;
  this->launcher_a_state_ = a;
  this->launcher_touch_state_ = touch;
}

void PappLoader::update_catalog_ui_() {
  if (this->catalog_container_ == nullptr)
    return;

  lv_obj_clean(this->catalog_container_);
  this->catalog_selection_ = 0;
  if (this->catalog_entries_.empty()) {
    lv_list_add_text(this->catalog_container_, "No .papp files found");
    return;
  }

  for (const auto &entry : this->catalog_entries_) {
    auto *context = new PappCatalogButtonContext{this, entry.second};  // NOLINT
    lv_obj_t *button = lv_list_add_btn(this->catalog_container_, nullptr, entry.first.c_str());
    if (button == nullptr) {
      delete context;
      continue;
    }
    lv_obj_add_event_cb(button, papp_catalog_button_event_cb, LV_EVENT_ALL, context);
  }
  this->set_catalog_selection_(0);
}
#endif

void PappLoader::finish_app_() {
  const int result = this->papp_task_result_;
  ESP_LOGI(TAG, "PAPP worker returned %d", result);
  // Read before the close flags are reset below: was the app closed from the
  // loader, or did it return on its own?
  const bool closed_by_user = this->global_close_requested_ || this->toggle_close_requested_;
  // Stop the speaker before deleting the worker or unloading the PAPP. The
  // speaker owns asynchronous mixer/resampler tasks and must be quiescent
  // before any app-side teardown can release memory associated with a frame.
  if (this->speaker_ != nullptr) {
    ESP_LOGI(TAG, "PAPP close: stopping audio");
    this->speaker_->stop();
    ESP_LOGI(TAG, "PAPP close: audio stopped");
  }
  if (this->papp_task_handle_ != nullptr) {
    ESP_LOGI(TAG, "PAPP close: deleting worker task");
    vTaskDelete(this->papp_task_handle_);
    this->papp_task_handle_ = nullptr;
    ESP_LOGI(TAG, "PAPP close: worker task deleted");
  }
  if (this->app_handle_ != nullptr) {
    ESP_LOGI(TAG, "PAPP close: unloading app");
    psram_app_unload(this->app_handle_);
    this->app_handle_ = nullptr;
    ESP_LOGI(TAG, "PAPP close: app unloaded");
  }
  this->toggle_wait_release_ = this->toggle_button_ != nullptr;
  this->toggle_close_requested_ = false;
  this->global_close_requested_ = false;
  this->launched_ = false;
#ifdef PAPP_LOADER_USE_LVGL
  // Require a release before the next launcher input is accepted after an
  // app closes. This prevents the button used to exit an app from launching
  // the currently highlighted PAPP immediately afterward.
  this->launcher_input_armed_ = false;
  this->launcher_direction_state_ = 0;
  this->launcher_a_state_ = false;
  this->launcher_touch_state_ = false;
#endif
  // LVGL invalidation below redraws the launcher immediately. Avoid an extra
  // full 800x480 PAPP flush here; that transfer can leave the last game frame
  // visible while the close path waits for a second display transaction.
  this->restore_lvgl_();
  this->send_report_(closed_by_user ? "closed" : "exited", result, this->report_source_);
}

void PappLoader::clear_(uint16_t color) {
  if (this->framebuffer_ != nullptr)
    std::fill_n(this->framebuffer_, VIRTUAL_WIDTH * VIRTUAL_HEIGHT, color);
}

void PappLoader::draw_close_overlay_() {
  if (!this->launched_ || this->display_ == nullptr)
    return;

  const int left = 0;
  const int top = 0;
  const int right = left + CLOSE_BUTTON_SIZE;
  const int bottom = top + CLOSE_BUTTON_SIZE;

  // Opaque dark-red button keeps the X readable over bright game frames.
  for (int y = top; y < bottom; y++) {
    for (int x = left; x < right; x++)
      close_overlay_buffer[y * CLOSE_BUTTON_SIZE + x] = 0x7800;
  }

  // Red border.
  for (int i = 0; i < CLOSE_BUTTON_SIZE; i++) {
    close_overlay_buffer[top * CLOSE_BUTTON_SIZE + left + i] = 0xF800;
    close_overlay_buffer[(bottom - 1) * CLOSE_BUTTON_SIZE + left + i] = 0xF800;
    close_overlay_buffer[(top + i) * CLOSE_BUTTON_SIZE + left] = 0xF800;
    close_overlay_buffer[(top + i) * CLOSE_BUTTON_SIZE + right - 1] = 0xF800;
  }

  // White X.
  for (int i = 0; i < 26; i++) {
    for (int thickness = -1; thickness <= 1; thickness++) {
      const int x1 = left + 16 + i;
      const int y1 = top + 16 + i + thickness;
      const int x2 = left + 41 - i;
      const int y2 = top + 16 + i + thickness;
      if (x1 >= left && x1 < right && y1 >= top && y1 < bottom)
        close_overlay_buffer[y1 * CLOSE_BUTTON_SIZE + x1] = 0xFFFF;
      if (x2 >= left && x2 < right && y2 >= top && y2 < bottom)
        close_overlay_buffer[y2 * CLOSE_BUTTON_SIZE + x2] = 0xFFFF;
    }
  }

  this->display_->draw_pixels_at(
      CLOSE_BUTTON_RAW_X, CLOSE_BUTTON_RAW_Y, CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE,
      reinterpret_cast<const uint8_t *>(close_overlay_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
}

void PappLoader::clear_close_overlay_() {
  if (this->display_ == nullptr)
    return;

  std::fill_n(close_overlay_buffer, CLOSE_BUTTON_SIZE * CLOSE_BUTTON_SIZE, 0x0000);
  this->display_->draw_pixels_at(
      CLOSE_BUTTON_RAW_X, CLOSE_BUTTON_RAW_Y, CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE,
      reinterpret_cast<const uint8_t *>(close_overlay_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
}

void PappLoader::poll_close_button_() {
  if (this->touchscreen_ == nullptr || this->global_close_requested_)
    return;

  auto touch = this->touchscreen_->get_touch();
  if (!touch.has_value() || (touch->state & touchscreen::STATE_RELEASING) != 0)
    return;

  const int close_left = CLOSE_BUTTON_SCREEN_X - CLOSE_BUTTON_HIT_PADDING;
  const int close_top = CLOSE_BUTTON_SCREEN_Y - CLOSE_BUTTON_HIT_PADDING;
  const int close_right = CLOSE_BUTTON_SCREEN_X + CLOSE_BUTTON_SIZE + CLOSE_BUTTON_HIT_PADDING;
  const int close_bottom = CLOSE_BUTTON_SCREEN_Y + CLOSE_BUTTON_SIZE + CLOSE_BUTTON_HIT_PADDING;
  if (touch->x >= close_left && touch->x < close_right && touch->y >= close_top && touch->y < close_bottom) {
    this->global_close_requested_ = true;
    ESP_LOGI(TAG, "PAPP on-screen close requested");
    return;
  }
}

void PappLoader::flush_framebuffer_() {
  if (this->display_ == nullptr || this->framebuffer_ == nullptr)
    return;

  const int64_t flush_start_us = esp_timer_get_time();

  if (this->display_mutex_ != nullptr)
    xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY);

  const uint16_t *display_buffer = this->rotated_framebuffer_;
  bool ppa_ok = false;
  if (this->ppa_srm_client_ != nullptr && this->ppa_framebuffer_ != nullptr) {
    const size_t frame_bytes = VIRTUAL_WIDTH * VIRTUAL_HEIGHT * sizeof(uint16_t);
    esp_cache_msync(this->framebuffer_, frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = this->framebuffer_, .pic_w = VIRTUAL_WIDTH, .pic_h = VIRTUAL_HEIGHT,
            .block_w = VIRTUAL_WIDTH, .block_h = VIRTUAL_HEIGHT,
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = this->ppa_framebuffer_, .buffer_size = frame_bytes,
            .pic_w = VIRTUAL_WIDTH, .pic_h = VIRTUAL_HEIGHT,
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_180,
        .scale_x = 1.0f, .scale_y = 1.0f,
        .mirror_x = false, .mirror_y = false,
        .rgb_swap = false, .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(reinterpret_cast<ppa_client_handle_t>(this->ppa_srm_client_), &cfg) == ESP_OK) {
      esp_cache_msync(this->ppa_framebuffer_, frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
      display_buffer = this->ppa_framebuffer_;
      ppa_ok = true;
    }
  }
  if (!ppa_ok) {
    // Fallback for boards/IDF builds without a working PPA SRM client.
    for (int y = 0; y < VIRTUAL_HEIGHT; y++) {
      const uint16_t *src = this->framebuffer_ + y * VIRTUAL_WIDTH;
      uint16_t *dst = this->rotated_framebuffer_ + (VIRTUAL_HEIGHT - 1 - y) * VIRTUAL_WIDTH;
      for (int x = 0; x < VIRTUAL_WIDTH; x++)
        dst[VIRTUAL_WIDTH - 1 - x] = src[x];
    }
  }
  // One motion sample per second is enough for remote diagnosis and keeps the
  // raw video traffic small enough that it cannot starve the audio stream.
  static int64_t last_stream_frame_us = 0;
  if (this->stream_client_connected_ && this->stream_mutex_ != nullptr && display_buffer != nullptr &&
      flush_start_us - last_stream_frame_us >= 1000000) {
    if (xSemaphoreTake(this->stream_mutex_, 0) == pdTRUE) {
      constexpr uint16_t x_step = VIRTUAL_WIDTH / SCREEN_STREAM_WIDTH;
      constexpr uint16_t y_step = VIRTUAL_HEIGHT / SCREEN_STREAM_HEIGHT;
      for (uint16_t y = 0; y < SCREEN_STREAM_HEIGHT; y++) {
        const uint16_t *source = display_buffer + (y * y_step) * VIRTUAL_WIDTH;
        uint16_t *target = this->stream_framebuffer_ + y * SCREEN_STREAM_WIDTH;
        for (uint16_t x = 0; x < SCREEN_STREAM_WIDTH; x++)
          target[x] = source[x * x_step];
      }
      this->stream_frame_sequence_++;
      this->stream_frame_ready_ = true;
      last_stream_frame_us = flush_start_us;
      xSemaphoreGive(this->stream_mutex_);
    }
  }
  this->display_->draw_pixels_at(
      LCD_X_OFFSET, LCD_Y_OFFSET, VIRTUAL_WIDTH, VIRTUAL_HEIGHT,
      reinterpret_cast<const uint8_t *>(display_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
  if (this->launched_)
    this->draw_close_overlay_();
  else
    this->clear_close_overlay_();

  const int64_t flush_us = esp_timer_get_time() - flush_start_us;
  static uint32_t flush_frames = 0;
  static int64_t flush_total_us = 0;
  static int64_t flush_worst_us = 0;
  ++flush_frames;
  flush_total_us += flush_us;
  flush_worst_us = std::max(flush_worst_us, flush_us);
  if (flush_frames == 60) {
    ESP_LOGI(TAG, "PAPP video flush: avg=%lld us worst=%lld us last=%lld us",
             static_cast<long long>(flush_total_us / flush_frames),
             static_cast<long long>(flush_worst_us), static_cast<long long>(flush_us));
    flush_frames = 0;
    flush_total_us = 0;
    flush_worst_us = 0;
  }

  if (this->display_mutex_ != nullptr)
    xSemaphoreGiveRecursive(this->display_mutex_);
}

void PappLoader::screen_stream_task_entry_(void *arg) {
  auto *loader = static_cast<PappLoader *>(arg);
  if (loader != nullptr)
    loader->screen_stream_task_();
  vTaskDelete(nullptr);
}

void PappLoader::screen_stream_task_() {
  const int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (server_fd < 0)
    return;
  int reuse = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(SCREEN_STREAM_PORT);
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(server_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 || listen(server_fd, 1) < 0) {
    close(server_fd);
    return;
  }
  timeval accept_timeout{1, 0};
  setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &accept_timeout, sizeof(accept_timeout));
  while (true) {
    if (!this->stream_enabled_) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    sockaddr_in peer{};
    socklen_t peer_length = sizeof(peer);
    const int client_fd = accept(server_fd, reinterpret_cast<sockaddr *>(&peer), &peer_length);
    if (client_fd < 0)
      continue;
    // A full 800x480 RGB565 frame is 768 KB. Allow slower hosts and TCP
    // back-pressure to drain it without truncating the diagnostic frame.
    timeval send_timeout{5, 0};
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    this->stream_client_connected_ = true;
    this->stream_frame_ready_ = false;
    this->stream_audio_read_ = 0;
    this->stream_audio_write_ = 0;
    this->stream_audio_available_ = 0;
    ESP_LOGI(TAG, "Diagnostic screen recorder connected");
    bool connected = true;
    while (connected) {
      if (this->stream_mutex_ == nullptr || xSemaphoreTake(this->stream_mutex_, pdMS_TO_TICKS(250)) != pdTRUE)
        continue;
      const bool ready = this->stream_frame_ready_;
      const uint32_t sequence = this->stream_frame_sequence_;
      size_t audio_chunk = 0;
      if (ready) {
        std::memcpy(this->stream_frame_packet_, this->stream_framebuffer_, SCREEN_STREAM_FRAME_BYTES);
        this->stream_frame_ready_ = false;
      }
      if (this->stream_audio_available_ != 0) {
        audio_chunk = std::min<size_t>(this->stream_audio_available_, SCREEN_STREAM_AUDIO_PACKET_BYTES);
        const size_t first = std::min(audio_chunk, SCREEN_STREAM_AUDIO_BYTES - this->stream_audio_read_);
        std::memcpy(this->stream_audio_packet_, this->stream_audio_buffer_ + this->stream_audio_read_, first);
        if (first < audio_chunk)
          std::memcpy(this->stream_audio_packet_ + first, this->stream_audio_buffer_, audio_chunk - first);
        this->stream_audio_read_ = (this->stream_audio_read_ + audio_chunk) % SCREEN_STREAM_AUDIO_BYTES;
        this->stream_audio_available_ -= audio_chunk;
      }
      xSemaphoreGive(this->stream_mutex_);
      if (ready) {
        ScreenStreamHeader header{{'P','A','P','P','F','B','0','1'}, SCREEN_STREAM_WIDTH,
                                  SCREEN_STREAM_HEIGHT, static_cast<uint32_t>(SCREEN_STREAM_FRAME_BYTES), sequence};
        connected = send_screen_stream_bytes(client_fd, &header, sizeof(header)) &&
                    send_screen_stream_bytes(client_fd, this->stream_frame_packet_, SCREEN_STREAM_FRAME_BYTES);
      }
      if (connected && audio_chunk != 0) {
        ScreenStreamAudioHeader audio_header{{'P','A','P','P','A','U','0','1'},
                                             static_cast<uint32_t>(this->audio_sample_rate_ / 2), 1, 16,
                                             static_cast<uint32_t>(audio_chunk), sequence};
        connected = send_screen_stream_bytes(client_fd, &audio_header, sizeof(audio_header)) &&
                    send_screen_stream_bytes(client_fd, this->stream_audio_packet_, audio_chunk);
      }
      if (!connected)
        break;
      vTaskDelay(pdMS_TO_TICKS(SCREEN_STREAM_INTERVAL_MS));
    }
    this->stream_client_connected_ = false;
    this->stream_enabled_ = false;
    close(client_fd);
    ESP_LOGI(TAG, "Diagnostic screen recorder disconnected");
  }
}

void PappLoader::restore_lvgl_() {
#ifdef PAPP_LOADER_USE_LVGL
  if (this->lvgl_ == nullptr || !this->lvgl_->is_loop_started()) {
    ESP_LOGW(TAG, "PAPP closed before LVGL was ready; skipping UI restore");
    return;
  }

  lv_obj_t *screen = this->lvgl_->get_screen_active();
  if (screen == nullptr || this->lvgl_->get_disp() == nullptr) {
    ESP_LOGW(TAG, "PAPP closed but LVGL screen/display is unavailable");
    return;
  }

  // PAPP draws directly to the panel and bypasses LVGL's dirty-area tracking.
  // Invalidate the active screen and force one immediate refresh so the
  // interface underneath the PAPP replaces the black PAPP canvas at once.
  // Drop the PAPP's final touch/press state before exposing the launcher
  // again. This keeps a close tap from activating the button underneath it.
  lv_indev_reset(nullptr, nullptr);
  if (this->touch_modal_shield_ != nullptr) {
    lv_obj_delete(this->touch_modal_shield_);
    this->touch_modal_shield_ = nullptr;
  }
  this->lvgl_->set_paused(false, false);
  for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev != nullptr;
       indev = lv_indev_get_next(indev)) {
    lv_indev_enable(indev, true);
  }
  lv_indev_reset(nullptr, nullptr);
  lv_obj_invalidate(screen);
  lv_refr_now(this->lvgl_->get_disp());
  ESP_LOGI(TAG, "LVGL interface restored after PAPP exit");
#endif
}

void PappLoader::render_custom_(const uint16_t *buffer, uint16_t in_w, uint16_t in_h, float scale, bool byte_swap) {
  if (this->framebuffer_ == nullptr || buffer == nullptr || in_w == 0 || in_h == 0 || scale <= 0.0f)
    return;

  const int64_t render_start_us = esp_timer_get_time();

  int out_w = std::max(1, static_cast<int>(in_w * scale + 0.5f));
  int out_h = std::max(1, static_cast<int>(in_h * scale + 0.5f));
  out_w = std::min(out_w, VIRTUAL_WIDTH);
  out_h = std::min(out_h, VIRTUAL_HEIGHT);
  const int x0 = (VIRTUAL_WIDTH - out_w) / 2;
  const int y0 = (VIRTUAL_HEIGHT - out_h) / 2;

  bool ppa_scaled = false;
  // The common PAPP path is an exact 2x 400x240 -> 800x480 frame. Let the
  // P4 SRM unit do that copy/scale instead of touching 384,000 pixels on the
  // worker CPU. The old scaler remains below for arbitrary emulator sizes,
  // fractional scales, and byte-swapped input.
  if (!byte_swap && this->ppa_srm_client_ != nullptr && this->ppa_framebuffer_ != nullptr &&
      out_w == VIRTUAL_WIDTH && out_h == VIRTUAL_HEIGHT && x0 == 0 && y0 == 0) {
    const size_t input_bytes = static_cast<size_t>(in_w) * in_h * sizeof(uint16_t);
    const size_t output_bytes = static_cast<size_t>(VIRTUAL_WIDTH) * VIRTUAL_HEIGHT * sizeof(uint16_t);
    esp_cache_msync(const_cast<uint16_t *>(buffer), input_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = const_cast<uint16_t *>(buffer), .pic_w = in_w, .pic_h = in_h,
            .block_w = in_w, .block_h = in_h,
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = this->framebuffer_, .buffer_size = output_bytes,
            .pic_w = VIRTUAL_WIDTH, .pic_h = VIRTUAL_HEIGHT,
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = static_cast<float>(out_w) / in_w,
        .scale_y = static_cast<float>(out_h) / in_h,
        .mirror_x = false, .mirror_y = false,
        .rgb_swap = false, .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(reinterpret_cast<ppa_client_handle_t>(this->ppa_srm_client_), &cfg) == ESP_OK) {
      esp_cache_msync(this->framebuffer_, output_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
      ppa_scaled = true;
    }
  }

  // Also accelerate centered emulator-sized frames (for example NES
  // 320x240 -> 640x480). PPA writes the scaled image to its compact output
  // buffer, then only the visible rows are copied into the virtual canvas.
  // This avoids the much more expensive nearest-neighbour loop over the
  // entire scaled image while preserving the existing canvas contract.
  if (!ppa_scaled && !byte_swap && this->ppa_srm_client_ != nullptr && this->ppa_framebuffer_ != nullptr &&
      out_w <= VIRTUAL_WIDTH && out_h <= VIRTUAL_HEIGHT) {
    const size_t input_bytes = static_cast<size_t>(in_w) * in_h * sizeof(uint16_t);
    const size_t output_bytes = static_cast<size_t>(out_w) * out_h * sizeof(uint16_t);
    esp_cache_msync(const_cast<uint16_t *>(buffer), input_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = const_cast<uint16_t *>(buffer), .pic_w = in_w, .pic_h = in_h,
            .block_w = in_w, .block_h = in_h,
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = this->ppa_framebuffer_, .buffer_size = output_bytes,
            .pic_w = static_cast<uint32_t>(out_w), .pic_h = static_cast<uint32_t>(out_h),
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = static_cast<float>(out_w) / in_w,
        .scale_y = static_cast<float>(out_h) / in_h,
        .mirror_x = false, .mirror_y = false,
        .rgb_swap = false, .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(reinterpret_cast<ppa_client_handle_t>(this->ppa_srm_client_), &cfg) == ESP_OK) {
      esp_cache_msync(this->ppa_framebuffer_, output_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
      this->clear_(0x0000);
      for (int row = 0; row < out_h; row++) {
        std::memcpy(this->framebuffer_ + (y0 + row) * VIRTUAL_WIDTH + x0,
                    this->ppa_framebuffer_ + row * out_w,
                    static_cast<size_t>(out_w) * sizeof(uint16_t));
      }
      ppa_scaled = true;
    }
  }

  if (!ppa_scaled) {
    this->clear_(0x0000);
    for (int y = 0; y < out_h; y++) {
      const int src_y = (y * in_h) / out_h;
      for (int x = 0; x < out_w; x++) {
        const int src_x = (x * in_w) / out_w;
        uint16_t pixel = buffer[src_y * in_w + src_x];
        if (byte_swap)
          pixel = static_cast<uint16_t>((pixel >> 8) | (pixel << 8));
        this->framebuffer_[(y0 + y) * VIRTUAL_WIDTH + x0 + x] = pixel;
      }
    }
  }
  this->flush_framebuffer_();

  const int64_t render_us = esp_timer_get_time() - render_start_us;
  static uint32_t render_frames = 0;
  static int64_t render_total_us = 0;
  static int64_t render_worst_us = 0;
  ++render_frames;
  render_total_us += render_us;
  render_worst_us = std::max(render_worst_us, render_us);
  if (render_frames == 60) {
    ESP_LOGI(TAG, "PAPP video frame: avg=%lld us worst=%lld us last=%lld us input=%ux%u scale=%.2f",
             static_cast<long long>(render_total_us / render_frames),
             static_cast<long long>(render_worst_us), static_cast<long long>(render_us),
             static_cast<unsigned>(in_w), static_cast<unsigned>(in_h), scale);
    render_frames = 0;
    render_total_us = 0;
    render_worst_us = 0;
  }
}

void PappLoader::render_emu_() { this->render_custom_(this->emu_buffer_, EMU_WIDTH, EMU_HEIGHT, 2.0f, false); }

void PappLoader::read_input_(papp_gamepad_state_t *state) {
  if (state == nullptr)
    return;
  // Ignore small stale/noisy axis values around center. The Switch HID
  // reports are quantized and this also prevents a near-threshold value from
  // looking like a held direction after a map/portal transition.
  constexpr float stick_deadzone = 40.0f;
  std::memset(state, 0, sizeof(*state));
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    if (this->buttons_[i] != nullptr)
      state->values[i] = this->buttons_[i]->get_state() ? 1 : 0;
  }

  // GPIO2 is the board's TTP223 touch button. The right trigger remains a
  // normal A/fire input, and the touch button is an additional A/fire input.
  if (this->fire_button_ != nullptr && this->fire_button_->get_state())
    state->values[PAPP_INPUT_A] = 1;
  if (this->touch_button_ != nullptr && this->touch_button_->get_state())
    state->values[PAPP_INPUT_A] = 1;

  // Elecrow GPIO16 is a resistor ladder. Preserve the calibrated mapping used
  // by the ESPHome UI, while exposing its diagonal/axis directions to PAPPs.
  if (this->adc_button_sensor_ != nullptr) {
    const float voltage = this->adc_button_sensor_->get_state();
    if (voltage < 1.48f) {
      state->values[PAPP_INPUT_DOWN] = 1;
      state->values[PAPP_INPUT_LEFT] = 1;
    } else if (voltage < 1.60f) {
      state->values[PAPP_INPUT_UP] = 1;
      state->values[PAPP_INPUT_LEFT] = 1;
    } else if (voltage < 1.75f) {
      state->values[PAPP_INPUT_LEFT] = 1;
    } else if (voltage < 1.95f) {
      state->values[PAPP_INPUT_DOWN] = 1;
      state->values[PAPP_INPUT_RIGHT] = 1;
    } else if (voltage < 2.10f) {
      state->values[PAPP_INPUT_UP] = 1;
      state->values[PAPP_INPUT_RIGHT] = 1;
    } else if (voltage < 2.40f) {
      state->values[PAPP_INPUT_RIGHT] = 1;
    } else if (voltage < 2.75f) {
      state->values[PAPP_INPUT_DOWN] = 1;
    } else if (voltage < 3.10f) {
      state->values[PAPP_INPUT_UP] = 1;
    }
  }

  if (this->left_stick_x_sensor_ != nullptr) {
    const float x = this->left_stick_x_sensor_->get_state();
    if (x < -stick_deadzone) state->values[PAPP_INPUT_LEFT] = 1;
    if (x > stick_deadzone) state->values[PAPP_INPUT_RIGHT] = 1;
  }
  if (this->left_stick_y_sensor_ != nullptr) {
    const float y = this->left_stick_y_sensor_->get_state();
    // The Elecrow ADC axis reports positive when the stick is physically up.
    // Invert only Y; the X polarity is already correct.
    if (y < -stick_deadzone) state->values[PAPP_INPUT_DOWN] = 1;
    if (y > stick_deadzone) state->values[PAPP_INPUT_UP] = 1;
  }
  // The PAPP ABI has no separate strafe-axis fields. L/R are used as the
  // horizontal right-stick strafe pair, and Doom maps them to strafe keys.
  if (this->right_stick_x_sensor_ != nullptr) {
    const float x = this->right_stick_x_sensor_->get_state();
    if (x < -stick_deadzone) state->values[PAPP_INPUT_L] = 1;
    if (x > stick_deadzone) state->values[PAPP_INPUT_R] = 1;
  }
  if (this->right_stick_y_sensor_ != nullptr) {
    const float y = this->right_stick_y_sensor_->get_state();
    if (y < -stick_deadzone) state->values[PAPP_INPUT_DOWN] = 1;
    if (y > stick_deadzone) state->values[PAPP_INPUT_UP] = 1;
  }

#ifdef PAPP_LOADER_USE_USB_HIDX
  if (this->usb_hidx_ != nullptr) {
    auto key = [this](uint8_t code) { return usb_keyboard_key_pressed(this->usb_hidx_, code); };
    state->values[PAPP_INPUT_UP] |= key(0x52) || key(0x1A);       // Up / W
    state->values[PAPP_INPUT_RIGHT] |= key(0x4F) || key(0x07);    // Right / D
    state->values[PAPP_INPUT_DOWN] |= key(0x51) || key(0x16);     // Down / S
    state->values[PAPP_INPUT_LEFT] |= key(0x50) || key(0x04);     // Left / A
    state->values[PAPP_INPUT_A] |= key(0x2C) || key(0x28) || key(0x1D); // Space/Enter/Z
    state->values[PAPP_INPUT_B] |= key(0x1B);                    // X
    state->values[PAPP_INPUT_X] |= key(0x06);                    // C
    state->values[PAPP_INPUT_Y] |= key(0x19) || key(0x2B);        // V/Tab
    state->values[PAPP_INPUT_L] |= key(0x14);                     // Q
    state->values[PAPP_INPUT_R] |= key(0x08);                     // E
    state->values[PAPP_INPUT_MENU] |= key(0x29);                  // Escape
    state->values[PAPP_INPUT_START] |= key(0x28);                 // Enter
    state->values[PAPP_INPUT_SELECT] |= key(0x2A);                // Backspace

    state->values[PAPP_INPUT_A] |= usb_mouse_button_pressed(this->usb_hidx_->get_mouse_left_sensor());
    state->values[PAPP_INPUT_B] |= usb_mouse_button_pressed(this->usb_hidx_->get_mouse_right_sensor());
    state->values[PAPP_INPUT_X] |= usb_mouse_button_pressed(this->usb_hidx_->get_mouse_middle_sensor());
  }
#endif

  // The current psram_lvgl PAPP exits on PAPP_INPUT_X. Keep the toggle
  // control independent of the normal gamepad mapping and synthesize that
  // exit input while the configured button is held. This works with the
  // existing PAPP binary on the SD card; no ABI or SD-card replacement is
  // required.
  if (this->toggle_button_ != nullptr) {
    const bool pressed = this->toggle_button_->get_state();
    this->toggle_button_state_ = pressed;
    if (pressed)
      this->toggle_close_requested_ = true;
    if (pressed || this->toggle_close_requested_)
      state->values[PAPP_INPUT_X] = 1;
  }

  // The on-screen close control is intentionally exposed as both MENU and X:
  // older game PAPPs use the MENU watchdog, while utility PAPPs such as the
  // touch test use X.  The dedicated L3 service below also sees this request.
  if (this->global_close_requested_) {
    state->values[PAPP_INPUT_MENU] = 1;
    state->values[PAPP_INPUT_X] = 1;
  }
}

int PappLoader::read_touch_(int *x, int *y) {
  if (this->touchscreen_ == nullptr)
    return 0;

  // The ESPHome touchscreen loop remains active while the PAPP worker runs;
  // read the latest cached sample without starting a competing I2C poll.
  auto touch = this->touchscreen_->get_touch();
  if (!touch.has_value() || (touch->state & touchscreen::STATE_RELEASING) != 0) {
    if (this->touch_active_) {
      ESP_LOGI(TAG, "PAPP touch RELEASED");
      this->touch_active_ = false;
    }
    return 0;
  }

  const int physical_x = touch->x;
  const int physical_y = touch->y;
  const int close_left = CLOSE_BUTTON_SCREEN_X - CLOSE_BUTTON_HIT_PADDING;
  const int close_top = CLOSE_BUTTON_SCREEN_Y - CLOSE_BUTTON_HIT_PADDING;
  const int close_right = CLOSE_BUTTON_SCREEN_X + CLOSE_BUTTON_SIZE + CLOSE_BUTTON_HIT_PADDING;
  const int close_bottom = CLOSE_BUTTON_SCREEN_Y + CLOSE_BUTTON_SIZE + CLOSE_BUTTON_HIT_PADDING;
  if (physical_x >= close_left && physical_x < close_right && physical_y >= close_top && physical_y < close_bottom) {
    if (!this->global_close_requested_) {
      this->global_close_requested_ = true;
      ESP_LOGI(TAG, "PAPP on-screen close requested");
    }
    this->touch_active_ = true;
    return 0;
  }

  if (physical_x < LCD_X_OFFSET || physical_x >= LCD_X_OFFSET + VIRTUAL_WIDTH ||
      physical_y < LCD_Y_OFFSET || physical_y >= LCD_Y_OFFSET + VIRTUAL_HEIGHT) {
    if (!this->touch_active_)
      ESP_LOGD(TAG, "PAPP touch outside canvas: physical=(%d,%d)", physical_x, physical_y);
    return 0;
  }

  const int canvas_x = physical_x - LCD_X_OFFSET;
  const int canvas_y = physical_y - LCD_Y_OFFSET;

  // The touchscreen component already reports panel-oriented coordinates.
  // The PAPP framebuffer is rotated later during display flush, but applying
  // another 180-degree transform here would send a bottom-right touch to the
  // top-left of the app canvas.
  const int logical_x = canvas_x;
  const int logical_y = canvas_y;

  if (!this->touch_active_) {
    ESP_LOGI(TAG, "PAPP touch PRESSED: physical=(%d,%d) panel_canvas=(%d,%d) logical=(%d,%d)",
             physical_x, physical_y, canvas_x, canvas_y, logical_x, logical_y);
    this->touch_active_ = true;
  }

  if (x != nullptr)
    *x = logical_x;
  if (y != nullptr)
    *y = logical_y;
  return 1;
}

void PappLoader::audio_init_(int sample_rate) {
  if (this->speaker_ == nullptr || sample_rate <= 0)
    return;
  if (this->audio_sample_rate_ != sample_rate) {
    this->audio_sample_rate_ = sample_rate;
    this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 2, sample_rate));
    ESP_LOGI(TAG, "PAPP audio initialized: %d Hz stereo 16-bit", sample_rate);
  }
  this->speaker_->set_mute_state(false);
  this->speaker_->set_volume(1.0f);
  this->speaker_->start();
}

void PappLoader::audio_submit_(short *stereo_buf, int frame_count) {
  if (this->speaker_ == nullptr || stereo_buf == nullptr || frame_count <= 0)
    return;
  const size_t bytes = static_cast<size_t>(frame_count) * 2 * sizeof(int16_t);
  const size_t stream_bytes = static_cast<size_t>((frame_count + 1) / 2) * sizeof(int16_t);
  if (this->stream_client_connected_ && this->stream_mutex_ != nullptr && this->stream_audio_buffer_ != nullptr &&
      xSemaphoreTake(this->stream_mutex_, 0) == pdTRUE) {
    while (this->stream_audio_available_ + stream_bytes > SCREEN_STREAM_AUDIO_BYTES) {
      this->stream_audio_read_ = (this->stream_audio_read_ + sizeof(int16_t)) % SCREEN_STREAM_AUDIO_BYTES;
      this->stream_audio_available_ -= sizeof(int16_t);
    }
    // The diagnostic track is mono (the left channel) at half the game's
    // native sample rate. This keeps the combined video/audio transport below
    // the board's sustainable TCP rate.
    for (int frame = 0; frame < frame_count; frame += 2) {
      std::memcpy(this->stream_audio_buffer_ + this->stream_audio_write_, &stereo_buf[frame * 2], sizeof(int16_t));
      this->stream_audio_write_ = (this->stream_audio_write_ + sizeof(int16_t)) % SCREEN_STREAM_AUDIO_BYTES;
    }
    this->stream_audio_available_ += stream_bytes;
    xSemaphoreGive(this->stream_mutex_);
  }
  // Quake submits 512 stereo frames at 22,050 Hz, which is about 23.2 ms of
  // audio.  A 20 ms timeout could therefore drop the tail of a block whenever
  // the resampler/mixer task was briefly busy.  That produces an audible click
  // even though the average audio rate is correct.  Allow one full output
  // several output buffer intervals of back-pressure and report short writes
  // without flooding the serial log. This is intentionally longer than the
  // block duration: it lets the 22.05 -> 48 kHz resampler free enough room for
  // the complete block instead of accepting a partial block and clicking.
  const size_t written = this->speaker_->play(reinterpret_cast<const uint8_t *>(stereo_buf), bytes, pdMS_TO_TICKS(100));
  static int64_t last_audio_debug_log_us = 0;
  const int64_t audio_now_us = esp_timer_get_time();
  if (audio_now_us - last_audio_debug_log_us >= 1000000) {
    int peak = 0;
    for (int i = 0; i < frame_count * 2; i++) {
      const int value = std::abs(static_cast<int>(stereo_buf[i]));
      if (value > peak)
        peak = value;
    }
    ESP_LOGI(TAG, "PAPP audio: input=%d Hz frames=%d peak=%d wrote=%u/%u running=%d muted=%d volume=%.2f",
             this->audio_sample_rate_, frame_count, peak, static_cast<unsigned>(written),
             static_cast<unsigned>(bytes), this->speaker_->is_running(), this->speaker_->get_mute_state(),
             this->speaker_->get_volume());
    last_audio_debug_log_us = audio_now_us;
  }
  if (written != bytes) {
    static int64_t last_short_write_log_us = 0;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - last_short_write_log_us >= 1000000) {
      ESP_LOGW(TAG, "PAPP audio short write: %u/%u bytes", static_cast<unsigned>(written),
               static_cast<unsigned>(bytes));
      last_short_write_log_us = now_us;
    }
  }
}

uint16_t *PappLoader::svc_display_get_framebuffer() { return active_ != nullptr ? active_->framebuffer_ : nullptr; }
uint16_t *PappLoader::svc_display_get_emu_buffer() { return active_ != nullptr ? active_->emu_buffer_ : nullptr; }
void PappLoader::svc_display_flush() {
  if (active_ != nullptr)
    active_->flush_framebuffer_();
}
void PappLoader::svc_display_emu_flush() {
  if (active_ != nullptr)
    active_->render_emu_();
}
void PappLoader::svc_display_clear(uint16_t color) {
  if (active_ != nullptr)
    active_->clear_(color);
}
void PappLoader::svc_display_set_scale(float sx, float sy) {
  if (active_ != nullptr) {
    active_->scale_x_ = sx;
    active_->scale_y_ = sy;
  }
}
void PappLoader::svc_display_write_frame_rgb565(const uint16_t *buffer) {
  if (active_ != nullptr && active_->framebuffer_ != nullptr && buffer != nullptr)
    std::memcpy(active_->framebuffer_, buffer, VIRTUAL_WIDTH * VIRTUAL_HEIGHT * sizeof(uint16_t));
}
void PappLoader::svc_display_write_frame_custom(const uint16_t *buffer, uint16_t in_w, uint16_t in_h,
                                                float scale, bool byte_swap) {
  if (active_ != nullptr)
    active_->render_custom_(buffer, in_w, in_h, scale, byte_swap);
}
void PappLoader::svc_display_write_rect(int x, int y, int w, int h, const uint16_t *data) {
  if (active_ == nullptr || active_->framebuffer_ == nullptr || data == nullptr)
    return;
  for (int row = 0; row < h; row++) {
    const int dst_y = y + row;
    if (dst_y < 0 || dst_y >= VIRTUAL_HEIGHT)
      continue;
    for (int col = 0; col < w; col++) {
      const int dst_x = x + col;
      if (dst_x >= 0 && dst_x < VIRTUAL_WIDTH)
        active_->framebuffer_[dst_y * VIRTUAL_WIDTH + dst_x] = data[row * w + col];
    }
  }
}
int PappLoader::svc_sprite_blit(uint16_t *framebuf, uint32_t fb_w, uint32_t fb_h,
                                uint32_t x, uint32_t y, const uint16_t *sprite,
                                uint32_t sp_w, uint32_t sp_h, uint16_t colorkey) {
  if (framebuf == nullptr || sprite == nullptr || fb_w == 0 || fb_h == 0 || sp_w == 0 || sp_h == 0)
    return -1;
  for (uint32_t row = 0; row < sp_h; row++) {
    const uint32_t dst_y = y + row;
    if (dst_y >= fb_h)
      continue;
    for (uint32_t col = 0; col < sp_w; col++) {
      const uint32_t dst_x = x + col;
      if (dst_x >= fb_w)
        continue;
      const uint16_t pixel = sprite[row * sp_w + col];
      if (pixel != colorkey)
        framebuf[dst_y * fb_w + dst_x] = pixel;
    }
  }
  return 0;
}
int PappLoader::svc_fb_copy(const uint16_t *src, uint16_t *dst, uint32_t w, uint32_t h) {
  if (src == nullptr || dst == nullptr || w == 0 || h == 0)
    return -1;
  std::memcpy(dst, src, static_cast<size_t>(w) * h * sizeof(uint16_t));
  return 0;
}

uint16_t *PappLoader::svc_png_load_rgb565(const char *path,
                                          uint16_t *out_w, uint16_t *out_h) {
  if (path == nullptr)
    return nullptr;

  // PAPPs use the stable /sd namespace, while the ESPHome SD component mounts
  // the actual VFS at /sdcard. Keep PNG loading consistent with file_open().
  std::string fs_path(path);
  if (fs_path == "/sd")
    fs_path = "/sdcard";
  else if (fs_path.rfind("/sd/", 0) == 0)
    fs_path = "/sdcard/" + fs_path.substr(4);

  pngObject png{};
  if (!loadPngFromFileRaw(fs_path.c_str(), &png, true, true) || png.data == nullptr ||
      png.w == 0 || png.h == 0) {
    ESP_LOGW(TAG, "PNG load failed: %s", path);
    return nullptr;
  }

  // PNGdec's little-endian RGB565 output must be converted to the panel's
  // byte order and BGR wiring. This matches the native launcher path.
  auto *pixels = reinterpret_cast<uint16_t *>(png.data);
  const size_t pixel_count = static_cast<size_t>(png.w) * png.h;
  for (size_t i = 0; i < pixel_count; i++) {
    const uint16_t pixel = pixels[i];
    const uint16_t swapped = static_cast<uint16_t>((pixel >> 8) | (pixel << 8));
    pixels[i] = static_cast<uint16_t>((swapped & 0x07e0) |
                                      ((swapped & 0x001f) << 11) |
                                      ((swapped >> 11) & 0x001f));
  }

  if (out_w != nullptr)
    *out_w = png.w;
  if (out_h != nullptr)
    *out_h = png.h;
  ESP_LOGI(TAG, "PNG loaded: %s (%ux%u)", path, png.w, png.h);
  return reinterpret_cast<uint16_t *>(png.data);
}
void PappLoader::svc_display_lock() {
  if (active_ != nullptr && active_->display_mutex_ != nullptr)
    xSemaphoreTakeRecursive(active_->display_mutex_, portMAX_DELAY);
}
void PappLoader::svc_display_unlock() {
  if (active_ != nullptr && active_->display_mutex_ != nullptr)
    xSemaphoreGiveRecursive(active_->display_mutex_);
}
void PappLoader::svc_audio_init(int sample_rate) {
  if (active_ != nullptr)
    active_->audio_init_(sample_rate);
}
void PappLoader::svc_audio_submit(short *stereo_buf, int frame_count) {
  if (active_ != nullptr)
    active_->audio_submit_(stereo_buf, frame_count);
}
void PappLoader::svc_input_gamepad_read(papp_gamepad_state_t *state) {
  if (active_ != nullptr)
    active_->read_input_(state);
  else if (state != nullptr)
    std::memset(state, 0, sizeof(*state));
}
int PappLoader::svc_input_l3_read() {
  if (active_ == nullptr)
    return 0;
  if (active_->global_close_requested_)
    return 1;
  return active_->toggle_button_ != nullptr && active_->toggle_button_->get_state() ? 1 : 0;
}
int PappLoader::svc_input_mouse_read(int *dx, int *dy, int *buttons) {
#ifdef PAPP_LOADER_USE_USB_HIDX
  if (active_ != nullptr)
    return active_->read_mouse_(dx, dy, buttons);
#endif
  if (dx != nullptr) *dx = 0;
  if (dy != nullptr) *dy = 0;
  if (buttons != nullptr) *buttons = 0;
  return 0;
}
int PappLoader::svc_input_keyboard_read(papp_keyboard_event_t *event) {
  return active_ != nullptr ? active_->read_keyboard_(event) : 0;
}
int PappLoader::svc_touch_read(int *x, int *y) { return active_ != nullptr ? active_->read_touch_(x, y) : 0; }

void *PappLoader::svc_file_open(const char *path, const char *mode) {
  const std::string mapped = runtime_path(path);
  return std::fopen(mapped.c_str(), mode);
}
int PappLoader::svc_file_close(void *stream) { return stream != nullptr ? std::fclose(static_cast<FILE *>(stream)) : -1; }
size_t PappLoader::svc_file_read(void *ptr, size_t size, size_t nmemb, void *stream) {
  return stream != nullptr ? std::fread(ptr, size, nmemb, static_cast<FILE *>(stream)) : 0;
}
size_t PappLoader::svc_file_write(const void *ptr, size_t size, size_t nmemb, void *stream) {
  return stream != nullptr ? std::fwrite(ptr, size, nmemb, static_cast<FILE *>(stream)) : 0;
}
int PappLoader::svc_file_seek(void *stream, long offset, int whence) {
  return stream != nullptr ? std::fseek(static_cast<FILE *>(stream), offset, whence) : -1;
}
long PappLoader::svc_file_tell(void *stream) { return stream != nullptr ? std::ftell(static_cast<FILE *>(stream)) : -1; }

void *PappLoader::svc_mem_caps_alloc(size_t size, uint32_t caps) {
  uint32_t real_caps = MALLOC_CAP_8BIT;
  if (caps & PAPP_MEM_CAP_SPIRAM)
    real_caps |= MALLOC_CAP_SPIRAM;
  if (caps & PAPP_MEM_CAP_INTERNAL)
    real_caps |= MALLOC_CAP_INTERNAL;
  if (caps & PAPP_MEM_CAP_DMA)
    real_caps |= MALLOC_CAP_DMA;
  return heap_caps_malloc(size, real_caps);
}

void *PappLoader::svc_mem_alloc(size_t size) { return std::malloc(size); }
void *PappLoader::svc_mem_calloc(size_t n, size_t size) { return std::calloc(n, size); }
void *PappLoader::svc_mem_realloc(void *ptr, size_t size) { return std::realloc(ptr, size); }
void PappLoader::svc_mem_free(void *ptr) { std::free(ptr); }

int PappLoader::svc_log_vprintf(const char *fmt, va_list args) {
  char buffer[256];
  const int result = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  if (result > 0) {
    // stdout is buffered on the PAPP worker and can disappear from the
    // monitor until a later flush. Route app logs through ESPHome's logger so
    // every PAPP gets timestamped, immediately visible serial output.
    ESP_LOGI(TAG, "PAPP: %s", buffer);
    if (PappLoader::active_ != nullptr)
      PappLoader::active_->append_report_log_(buffer);
  }
  return result;
}
int PappLoader::svc_log_printf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int result = svc_log_vprintf(fmt, args);
  va_end(args);
  return result;
}
void PappLoader::svc_delay_ms(int ms) {
  // A PAPP runs synchronously on ESPHome's loopTask. That means the normal
  // Application::loop() watchdog feed is not reached while the app is
  // active. Background PAPP tasks call this hook too, but they are not
  // registered with ESPHome's task watchdog; calling App.feed_wdt() from
  // those tasks produces "task not found" and does not feed loopTask.
#ifdef USE_ESP32
  if (esp_task_wdt_status(nullptr) == ESP_OK)
    App.feed_wdt();
#else
  App.feed_wdt();
#endif
  vTaskDelay(pdMS_TO_TICKS(std::max(ms, 1)));
#ifdef USE_ESP32
  if (esp_task_wdt_status(nullptr) == ESP_OK)
    App.feed_wdt();
#else
  App.feed_wdt();
#endif
}
int64_t PappLoader::svc_get_time_us() { return esp_timer_get_time(); }

static char s_rom_path[256] = {};
static int32_t s_volume = 100;
static int32_t s_brightness = 100;
char *PappLoader::svc_settings_rom_path_get() { return s_rom_path; }
void PappLoader::svc_settings_rom_path_set(const char *path) {
  if (path == nullptr)
    return;
  std::strncpy(s_rom_path, path, sizeof(s_rom_path) - 1);
  s_rom_path[sizeof(s_rom_path) - 1] = '\0';
}
int32_t PappLoader::svc_settings_volume_get() { return s_volume; }
void PappLoader::svc_settings_volume_set(int32_t level) { s_volume = level; }
int32_t PappLoader::svc_settings_brightness_get() { return s_brightness; }
void PappLoader::svc_settings_brightness_set(int32_t level) { s_brightness = level; }

int PappLoader::svc_task_create(void (*fn)(void *), const char *name, uint32_t stack_depth, void *arg,
                                int priority, void *out_handle, int core) {
  TaskHandle_t handle = nullptr;
  const BaseType_t affinity = core < 0 ? tskNO_AFFINITY : static_cast<BaseType_t>(core);

  // ESP-IDF's normal task API allocates stacks from internal RAM.  Duke3D
  // requests a 256 KiB stack, so use the supported external-memory API for
  // large PAPP stacks instead of failing with errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY.
  if (stack_depth > 32768) {
    const BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        fn, name, stack_depth, arg, priority, &handle, affinity,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result != pdPASS || handle == nullptr) {
      ESP_LOGE("PAPP", "Failed to create %lu-byte PSRAM task '%s' (result=%ld, largest=%lu)",
               static_cast<unsigned long>(stack_depth), name != nullptr ? name : "?",
               static_cast<long>(result),
               static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
      if (out_handle != nullptr)
        *static_cast<TaskHandle_t *>(out_handle) = nullptr;
      return -1;
    }
    if (!remember_papp_cap_task(handle)) {
      vTaskDeleteWithCaps(handle);
      if (out_handle != nullptr)
        *static_cast<TaskHandle_t *>(out_handle) = nullptr;
      return -1;
    }
    if (out_handle != nullptr)
      *static_cast<TaskHandle_t *>(out_handle) = handle;
    ESP_LOGI("PAPP", "Created PSRAM task '%s' (%lu bytes) on core %d",
             name != nullptr ? name : "?", static_cast<unsigned long>(stack_depth), core);
    return 0;
  }

  const BaseType_t result = xTaskCreatePinnedToCore(
      fn, name, stack_depth, arg, priority, &handle,
      affinity);
  if (out_handle != nullptr)
    *static_cast<TaskHandle_t *>(out_handle) = handle;
  return result == pdPASS ? 0 : -1;
}
void PappLoader::svc_task_delete(void *handle) {
  TaskHandle_t task = static_cast<TaskHandle_t>(handle);
  if (forget_papp_cap_task(task)) {
    vTaskDeleteWithCaps(task);
  } else {
    vTaskDelete(task);
  }
}

void PappLoader::populate_services(app_services_t *services) {
  std::memset(services, 0, sizeof(*services));
  services->abi_version = PAPP_ABI_VERSION;
  services->display_get_framebuffer = &PappLoader::svc_display_get_framebuffer;
  services->display_get_emu_buffer = &PappLoader::svc_display_get_emu_buffer;
  services->display_flush = &PappLoader::svc_display_flush;
  services->display_emu_flush = &PappLoader::svc_display_emu_flush;
  services->display_clear = &PappLoader::svc_display_clear;
  services->display_set_scale = &PappLoader::svc_display_set_scale;
  services->display_write_frame_rgb565 = &PappLoader::svc_display_write_frame_rgb565;
  services->display_write_frame_custom = &PappLoader::svc_display_write_frame_custom;
  services->display_write_rect = &PappLoader::svc_display_write_rect;
  services->sprite_blit = &PappLoader::svc_sprite_blit;
  services->fb_copy = &PappLoader::svc_fb_copy;
  services->png_load_rgb565 = &PappLoader::svc_png_load_rgb565;
  services->display_lock = &PappLoader::svc_display_lock;
  services->display_unlock = &PappLoader::svc_display_unlock;
  services->audio_init = &PappLoader::svc_audio_init;
  services->audio_submit = &PappLoader::svc_audio_submit;
  services->input_gamepad_read = &PappLoader::svc_input_gamepad_read;
  services->input_l3_read = &PappLoader::svc_input_l3_read;
  services->file_open = &PappLoader::svc_file_open;
  services->file_close = &PappLoader::svc_file_close;
  services->file_read = &PappLoader::svc_file_read;
  services->file_write = &PappLoader::svc_file_write;
  services->file_seek = &PappLoader::svc_file_seek;
  services->file_tell = &PappLoader::svc_file_tell;
  services->mem_alloc = &PappLoader::svc_mem_alloc;
  services->mem_calloc = &PappLoader::svc_mem_calloc;
  services->mem_realloc = &PappLoader::svc_mem_realloc;
  services->mem_free = &PappLoader::svc_mem_free;
  services->mem_caps_alloc = &PappLoader::svc_mem_caps_alloc;
  services->log_printf = &PappLoader::svc_log_printf;
  services->log_vprintf = &PappLoader::svc_log_vprintf;
  services->delay_ms = &PappLoader::svc_delay_ms;
  services->get_time_us = &PappLoader::svc_get_time_us;
  services->settings_rom_path_get = &PappLoader::svc_settings_rom_path_get;
  services->settings_rom_path_set = &PappLoader::svc_settings_rom_path_set;
  services->settings_volume_get = &PappLoader::svc_settings_volume_get;
  services->settings_volume_set = &PappLoader::svc_settings_volume_set;
  services->settings_brightness_get = &PappLoader::svc_settings_brightness_get;
  services->settings_brightness_set = &PappLoader::svc_settings_brightness_set;
  services->task_create = &PappLoader::svc_task_create;
  services->task_delete = &PappLoader::svc_task_delete;
  services->touch_read = &PappLoader::svc_touch_read;
  services->input_mouse_read = &PappLoader::svc_input_mouse_read;
  services->input_keyboard_read = &PappLoader::svc_input_keyboard_read;
}

esp_err_t psram_app_load(const char *path, psram_app_handle_t *out_handle) {
  if (out_handle == nullptr || path == nullptr)
    return ESP_ERR_INVALID_ARG;
  *out_handle = nullptr;
  ESP_LOGI(TAG, "Loading PAPP: %s", path);

  FILE *file = std::fopen(path, "rb");
  if (file == nullptr)
    return ESP_ERR_NOT_FOUND;
  papp_header_t header{};
  if (std::fread(&header, 1, sizeof(header), file) != sizeof(header)) {
    std::fclose(file);
    return ESP_ERR_INVALID_SIZE;
  }
  if (header.magic != PAPP_MAGIC || header.version != PAPP_ABI_VERSION || header.text_size == 0 ||
      header.entry_off >= header.text_size) {
    ESP_LOGE(TAG, "Invalid PAPP header: magic=0x%08lx version=%lu text=%lu entry=%lu",
             static_cast<unsigned long>(header.magic), static_cast<unsigned long>(header.version),
             static_cast<unsigned long>(header.text_size), static_cast<unsigned long>(header.entry_off));
    std::fclose(file);
    return ESP_ERR_INVALID_ARG;
  }

  const size_t load_size = static_cast<size_t>(header.text_size) + header.data_size;
  const size_t total = load_size + header.bss_size;
  if (load_size < header.text_size || total < load_size) {
    std::fclose(file);
    return ESP_ERR_INVALID_SIZE;
  }
  const size_t allocated = align_up(total, MMU_PAGE_SIZE);
  auto app = static_cast<psram_app_handle_t>(std::calloc(1, sizeof(struct psram_app)));
  if (app == nullptr) {
    std::fclose(file);
    return ESP_ERR_NO_MEM;
  }
  app->header = header;
  app->code_alloc = allocated;
  app->code_buf = heap_caps_aligned_alloc(MMU_PAGE_SIZE, allocated, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (app->code_buf == nullptr) {
    std::free(app);
    std::fclose(file);
    return ESP_ERR_NO_MEM;
  }
  if (std::fread(app->code_buf, 1, load_size, file) != load_size) {
    heap_caps_free(app->code_buf);
    std::free(app);
    std::fclose(file);
    return ESP_ERR_INVALID_SIZE;
  }
  std::memset(static_cast<uint8_t *>(app->code_buf) + load_size, 0, allocated - load_size);
  std::fclose(file);

  esp_cache_msync(app->code_buf, app->code_alloc,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  *out_handle = app;
  ESP_LOGI(TAG, "PAPP loaded: text=%lu data=%lu bss=%lu entry=%lu",
           static_cast<unsigned long>(header.text_size), static_cast<unsigned long>(header.data_size),
           static_cast<unsigned long>(header.bss_size), static_cast<unsigned long>(header.entry_off));
  return ESP_OK;
}

static esp_err_t http_finish(esp_http_client_handle_t client, esp_err_t result) {
  if (client != nullptr) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
  }
  return result;
}

static esp_err_t http_read_exact(esp_http_client_handle_t client, uint8_t *buffer, size_t length) {
  if (client == nullptr || (buffer == nullptr && length != 0))
    return ESP_ERR_INVALID_ARG;

  size_t offset = 0;
  uint32_t idle_reads = 0;
  while (offset < length) {
    const int requested = static_cast<int>(std::min<size_t>(length - offset, INT_MAX));
    const int result = esp_http_client_read(client, reinterpret_cast<char *>(buffer + offset), requested);
    if (result > 0) {
      offset += static_cast<size_t>(result);
      idle_reads = 0;
      continue;
    }

    // A slow Wi-Fi server may briefly have no bytes available. The HTTP
    // client reports both zero and EAGAIN while a response is still open.
    if (result == 0 || result == -ESP_ERR_HTTP_EAGAIN) {
      if (++idle_reads > 750)  // 15 seconds at 20 ms per retry
        return ESP_ERR_TIMEOUT;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    return ESP_FAIL;
  }
  return ESP_OK;
}

// ── Test reports (report_url) ──────────────────────────────────────────────
// One JSON POST per app run, so a store test can be judged without someone
// copying the serial log by hand.

static void json_append_escaped(std::string *out, const std::string &value) {
  out->push_back('"');
  for (const char ch : value) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        if (c < 0x20 || c >= 0x80) {
          // Logs are expected to be ASCII; escape anything else so the body is
          // always valid JSON even if an app prints raw bytes.
          char escaped[7];
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
          out->append(escaped);
        } else {
          out->push_back(ch);
        }
    }
  }
  out->push_back('"');
}

struct PappReport {
  std::string url;
  std::string body;
};

void PappLoader::begin_report_(const std::string &source) {
  if (this->report_url_.empty() || this->report_mutex_ == nullptr)
    return;
  if (xSemaphoreTake(this->report_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    this->report_log_.clear();
    this->report_source_ = source;
    this->report_started_us_ = esp_timer_get_time();
    xSemaphoreGive(this->report_mutex_);
  }
}

void PappLoader::append_report_log_(const char *line) {
  if (this->report_url_.empty() || this->report_mutex_ == nullptr || line == nullptr)
    return;
  // Called from the PAPP worker; never block it for long.
  if (xSemaphoreTake(this->report_mutex_, pdMS_TO_TICKS(5)) != pdTRUE)
    return;
  this->report_log_.append(line);
  if (this->report_log_.empty() || this->report_log_.back() != '\n')
    this->report_log_.push_back('\n');
  if (this->report_log_.size() > this->report_log_bytes_)
    this->report_log_.erase(0, this->report_log_.size() - this->report_log_bytes_);
  xSemaphoreGive(this->report_mutex_);
}

void PappLoader::send_report_(const char *outcome, int result, const std::string &source) {
  if (this->report_url_.empty() || this->report_mutex_ == nullptr)
    return;
  if (!network::is_connected()) {
    ESP_LOGW(TAG, "Test report not sent: network is down");
    return;
  }

  std::string log;
  int64_t started_us = 0;
  if (xSemaphoreTake(this->report_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    log.swap(this->report_log_);
    started_us = this->report_started_us_;
    xSemaphoreGive(this->report_mutex_);
  }
  const int64_t runtime_ms = started_us > 0 ? (esp_timer_get_time() - started_us) / 1000 : 0;
  const size_t slash = source.find_last_of('/');
  const std::string file = slash == std::string::npos ? source : source.substr(slash + 1);
  const bool is_load_error = std::strcmp(outcome, "load_failed") == 0;

  auto *report = new PappReport{this->report_url_, {}};  // NOLINT(cppcoreguidelines-owning-memory)
  std::string &body = report->body;
  body.reserve(log.size() + 512);
  body.append("{\"device\":");
  json_append_escaped(&body, App.get_name());
  body.append(",\"app\":");
  json_append_escaped(&body, file);
  body.append(",\"source\":");
  json_append_escaped(&body, source);
  body.append(",\"outcome\":");
  json_append_escaped(&body, outcome);
  body.append(",\"result\":");
  body.append(std::to_string(result));
  if (is_load_error) {
    body.append(",\"error\":");
    json_append_escaped(&body, esp_err_to_name(static_cast<esp_err_t>(result)));
  }
  body.append(",\"runtime_ms\":");
  body.append(std::to_string(runtime_ms));
  body.append(",\"abi\":");
  body.append(std::to_string(PAPP_ABI_VERSION));
  body.append(",\"log\":");
  json_append_escaped(&body, log);
  body.push_back('}');

  // The HTTP(S) request can take seconds; keep it off the ESPHome loop.
  if (xTaskCreatePinnedToCore(&PappLoader::report_task_entry_, "papp_report", 8192, report, 3, nullptr, 0) !=
      pdPASS) {
    ESP_LOGW(TAG, "Test report not sent: could not create task");
    delete report;  // NOLINT(cppcoreguidelines-owning-memory)
    return;
  }
  ESP_LOGI(TAG, "Sending test report: %s %s (result %d, %lld ms, %u log bytes)", file.c_str(), outcome, result,
           static_cast<long long>(runtime_ms), static_cast<unsigned>(log.size()));
}

void PappLoader::report_task_entry_(void *arg) {
  auto *report = static_cast<PappReport *>(arg);
  esp_http_client_config_t config{};
  config.url = report->url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.disable_auto_redirect = false;
  config.max_redirection_count = HTTP_MAX_REDIRECTIONS;
  config.keep_alive_enable = false;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (std::strncmp(report->url.c_str(), "https://", 8) == 0)
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Test report not sent: out of memory");
  } else {
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, report->body.data(), static_cast<int>(report->body.size()));
    const esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "Test report sent (HTTP %d)", esp_http_client_get_status_code(client));
    } else {
      ESP_LOGW(TAG, "Test report failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
  }
  delete report;  // NOLINT(cppcoreguidelines-owning-memory)
  vTaskDelete(nullptr);
}

static esp_err_t fetch_http_text(const char *url, std::string *out) {
  if (url == nullptr || out == nullptr || !is_network_url(url))
    return ESP_ERR_INVALID_ARG;
  out->clear();
  if (!network::is_connected())
    return ESP_ERR_INVALID_STATE;

  esp_http_client_config_t config{};
  config.url = url;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.buffer_size = HTTP_READ_BUFFER_SIZE;
  config.buffer_size_tx = 2048;
  config.disable_auto_redirect = false;
  config.max_redirection_count = HTTP_MAX_REDIRECTIONS;
  config.keep_alive_enable = false;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (std::strncmp(url, "https://", 8) == 0)
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr)
    return ESP_ERR_NO_MEM;
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK)
    return http_finish(client, err);

  int64_t content_length = -1;
  for (uint8_t attempt = 0; attempt < 6; attempt++) {
    content_length = esp_http_client_fetch_headers(client);
    if (content_length != -ESP_ERR_HTTP_EAGAIN)
      break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (content_length == -ESP_ERR_HTTP_EAGAIN)
    return http_finish(client, ESP_ERR_TIMEOUT);
  if (content_length > static_cast<int64_t>(MAX_CATALOG_SIZE)) {
    ESP_LOGE(TAG, "PAPP catalog is too large: %lld bytes", static_cast<long long>(content_length));
    return http_finish(client, ESP_ERR_INVALID_SIZE);
  }

  const int status_code = esp_http_client_get_status_code(client);
  if (status_code != 200) {
    ESP_LOGE(TAG, "PAPP catalog HTTP status: %d", status_code);
    return http_finish(client, ESP_ERR_INVALID_RESPONSE);
  }

  char buffer[2048];
  uint32_t idle_reads = 0;
  while (true) {
    const int result = esp_http_client_read(client, buffer, sizeof(buffer));
    if (result > 0) {
      if (out->size() + static_cast<size_t>(result) > MAX_CATALOG_SIZE)
        return http_finish(client, ESP_ERR_INVALID_SIZE);
      out->append(buffer, static_cast<size_t>(result));
      idle_reads = 0;
      continue;
    }
    if (result == 0)
      break;
    if (result == -ESP_ERR_HTTP_EAGAIN) {
      if (++idle_reads > 750)
        return http_finish(client, ESP_ERR_TIMEOUT);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    return http_finish(client, ESP_FAIL);
  }
  http_finish(client, ESP_OK);
  return ESP_OK;
}

static std::string lower_copy(const std::string &value) {
  std::string result = value;
  for (char &character : result)
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return result;
}

static std::string catalog_url_for_href(const std::string &base_url, const std::string &href) {
  if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0)
    return href;
  if (!href.empty() && href[0] == '/') {
    const size_t scheme_end = base_url.find("://");
    const size_t host_end = scheme_end == std::string::npos ? std::string::npos : base_url.find('/', scheme_end + 3);
    return base_url.substr(0, host_end) + href;
  }
  return base_url + (base_url.empty() || base_url.back() == '/' ? "" : "/") + href;
}

static std::vector<std::pair<std::string, std::string>> parse_papp_catalog(const std::string &html,
                                                                             const std::string &base_url) {
  std::vector<std::pair<std::string, std::string>> entries;
  const std::string lowered = lower_copy(html);
  size_t search_from = 0;
  while (true) {
    const size_t href_pos = lowered.find("href", search_from);
    if (href_pos == std::string::npos)
      break;
    size_t cursor = href_pos + 4;
    while (cursor < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[cursor])))
      cursor++;
    if (cursor >= lowered.size() || lowered[cursor] != '=') {
      search_from = cursor;
      continue;
    }
    cursor++;
    while (cursor < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[cursor])))
      cursor++;
    if (cursor >= lowered.size() || (lowered[cursor] != '\'' && lowered[cursor] != '"')) {
      search_from = cursor;
      continue;
    }
    const char quote = lowered[cursor++];
    const size_t end = lowered.find(quote, cursor);
    if (end == std::string::npos)
      break;
    std::string href = html.substr(cursor, end - cursor);
    const size_t entity = href.find("&amp;");
    if (entity != std::string::npos)
      href.replace(entity, 5, "&");
    const size_t fragment = href.find_first_of("?#");
    if (fragment != std::string::npos)
      href.resize(fragment);
    const std::string href_lower = lower_copy(href);
    if (href_lower.size() >= 5 && href_lower.compare(href_lower.size() - 5, 5, ".papp") == 0) {
      const std::string url = catalog_url_for_href(base_url, href);
      size_t slash = href.find_last_of('/');
      const std::string name = slash == std::string::npos ? href : href.substr(slash + 1);
      bool duplicate = false;
      for (const auto &existing : entries) {
        if (existing.second == url) {
          duplicate = true;
          break;
        }
      }
      if (!name.empty() && !duplicate)
        entries.emplace_back(name, url);
    }
    search_from = end + 1;
  }
  return entries;
}

esp_err_t psram_app_load_url(const char *url, psram_app_handle_t *out_handle) {
  if (url == nullptr || out_handle == nullptr || !is_network_url(url))
    return ESP_ERR_INVALID_ARG;
  *out_handle = nullptr;

  ESP_LOGI(TAG, "Streaming PAPP from URL: %s", url);
  if (!network::is_connected())
    return ESP_ERR_INVALID_STATE;

  esp_http_client_config_t config{};
  config.url = url;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.buffer_size = HTTP_READ_BUFFER_SIZE;
  config.buffer_size_tx = 4096;
  config.disable_auto_redirect = false;
  config.max_redirection_count = HTTP_MAX_REDIRECTIONS;
  config.keep_alive_enable = false;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (std::strncmp(url, "https://", 8) == 0)
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr)
    return ESP_ERR_NO_MEM;

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK)
    return http_finish(client, err);

  int64_t content_length = -1;
  for (uint8_t attempt = 0; attempt < 6; attempt++) {
    content_length = esp_http_client_fetch_headers(client);
    if (content_length != -ESP_ERR_HTTP_EAGAIN)
      break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (content_length == -ESP_ERR_HTTP_EAGAIN)
    return http_finish(client, ESP_ERR_TIMEOUT);

  const int status_code = esp_http_client_get_status_code(client);
  if (status_code != 200) {
    ESP_LOGE(TAG, "Network PAPP HTTP status: %d", status_code);
    return http_finish(client, ESP_ERR_INVALID_RESPONSE);
  }

  papp_header_t header{};
  err = http_read_exact(client, reinterpret_cast<uint8_t *>(&header), sizeof(header));
  if (err != ESP_OK)
    return http_finish(client, err);

  if (header.magic != PAPP_MAGIC || header.version != PAPP_ABI_VERSION || header.flags != 0 ||
      header.text_size == 0 || header.entry_off >= header.text_size) {
    ESP_LOGE(TAG, "Invalid network PAPP header: magic=0x%08lx version=%lu text=%lu entry=%lu flags=0x%08lx",
             static_cast<unsigned long>(header.magic), static_cast<unsigned long>(header.version),
             static_cast<unsigned long>(header.text_size), static_cast<unsigned long>(header.entry_off),
             static_cast<unsigned long>(header.flags));
    return http_finish(client, ESP_ERR_INVALID_ARG);
  }

  const uint64_t load_size_64 = static_cast<uint64_t>(header.text_size) + header.data_size;
  const uint64_t total_size_64 = load_size_64 + header.bss_size;
  const uint64_t wire_size_64 = static_cast<uint64_t>(PAPP_HEADER_SIZE) + load_size_64;
  if (load_size_64 < header.text_size || total_size_64 < load_size_64 ||
      total_size_64 > MAX_NETWORK_PAPP_SIZE || load_size_64 > SIZE_MAX || total_size_64 > SIZE_MAX ||
      (content_length > 0 && static_cast<uint64_t>(content_length) != wire_size_64)) {
    ESP_LOGE(TAG, "Network PAPP size rejected: wire=%lld expected=%llu total=%llu",
             static_cast<long long>(content_length), static_cast<unsigned long long>(wire_size_64),
             static_cast<unsigned long long>(total_size_64));
    return http_finish(client, ESP_ERR_INVALID_SIZE);
  }

  const size_t load_size = static_cast<size_t>(load_size_64);
  const size_t total_size = static_cast<size_t>(total_size_64);
  const size_t allocated = align_up(total_size, MMU_PAGE_SIZE);
  if (allocated == 0 || allocated > MAX_NETWORK_PAPP_SIZE)
    return http_finish(client, ESP_ERR_INVALID_SIZE);

  auto app = static_cast<psram_app_handle_t>(std::calloc(1, sizeof(struct psram_app)));
  if (app == nullptr)
    return http_finish(client, ESP_ERR_NO_MEM);
  app->header = header;
  app->code_alloc = allocated;
  app->code_buf = heap_caps_aligned_alloc(MMU_PAGE_SIZE, allocated, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (app->code_buf == nullptr) {
    std::free(app);
    return http_finish(client, ESP_ERR_NO_MEM);
  }

  auto *transfer_buffer = static_cast<uint8_t *>(heap_caps_malloc(HTTP_READ_BUFFER_SIZE, MALLOC_CAP_8BIT));
  if (transfer_buffer == nullptr) {
    heap_caps_free(app->code_buf);
    std::free(app);
    return http_finish(client, ESP_ERR_NO_MEM);
  }

  size_t received = 0;
  while (received < load_size) {
    const size_t chunk = std::min(HTTP_READ_BUFFER_SIZE, load_size - received);
    err = http_read_exact(client, transfer_buffer, chunk);
    if (err != ESP_OK)
      break;
    std::memcpy(static_cast<uint8_t *>(app->code_buf) + received, transfer_buffer, chunk);
    received += chunk;
    if ((received % (256 * 1024)) < chunk || received == load_size)
      ESP_LOGI(TAG, "Network PAPP download: %u/%u bytes", static_cast<unsigned>(received),
               static_cast<unsigned>(load_size));
  }
  heap_caps_free(transfer_buffer);
  if (err != ESP_OK) {
    heap_caps_free(app->code_buf);
    std::free(app);
    return http_finish(client, err);
  }

  std::memset(static_cast<uint8_t *>(app->code_buf) + load_size, 0, allocated - load_size);
  esp_cache_msync(app->code_buf, app->code_alloc,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  http_finish(client, ESP_OK);
  *out_handle = app;
  ESP_LOGI(TAG, "Network PAPP loaded: text=%lu data=%lu bss=%lu entry=%lu",
           static_cast<unsigned long>(header.text_size), static_cast<unsigned long>(header.data_size),
           static_cast<unsigned long>(header.bss_size), static_cast<unsigned long>(header.entry_off));
  return ESP_OK;
}

int psram_app_run(psram_app_handle_t handle) {
  if (handle == nullptr || handle->code_buf == nullptr || PappLoader::active() == nullptr)
    return -1;

  esp_paddr_t physical = 0;
  mmu_target_t target = MMU_TARGET_PSRAM0;
  esp_err_t err = esp_mmu_vaddr_to_paddr(handle->code_buf, &physical, &target);
  if (err != ESP_OK)
    return -1;

  const mmu_mem_caps_t caps = static_cast<mmu_mem_caps_t>(
      MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ | MMU_MEM_CAP_32BIT);
  err = esp_mmu_map(physical, handle->code_alloc, MMU_TARGET_PSRAM0, caps,
                    ESP_MMU_MMAP_FLAG_PADDR_SHARED, &handle->exec_ptr);
  if (err != ESP_OK)
    return -1;
  handle->mapped = true;
  esp_cache_msync(handle->exec_ptr, handle->code_alloc,
                  ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);

  app_services_t services{};
  PappLoader::populate_services(&services);
  auto entry = reinterpret_cast<papp_entry_fn_t>(static_cast<uint8_t *>(handle->exec_ptr) + handle->header.entry_off);
  ESP_LOGI(TAG, "Calling PAPP entry point at %p", reinterpret_cast<void *>(entry));
  const int result = entry(&services);

  esp_mmu_unmap(handle->exec_ptr);
  handle->exec_ptr = nullptr;
  handle->mapped = false;
  return result;
}

void psram_app_unload(psram_app_handle_t handle) {
  if (handle == nullptr)
    return;
  if (handle->mapped && handle->exec_ptr != nullptr)
    esp_mmu_unmap(handle->exec_ptr);
  if (handle->code_buf != nullptr)
    heap_caps_free(handle->code_buf);
  std::free(handle);
}

esp_err_t psram_app_selftest(void) {
  ESP_LOGW(TAG, "PSRAM self-test is not exposed by the ESPHome component");
  return ESP_ERR_NOT_SUPPORTED;
}

}  // namespace papp_loader
}  // namespace esphome

// psram_app.h exposes the loader API with C linkage because the same header
// is included by the separately linked PAPP applications.  The implementation
// above lives in the ESPHome namespace, so provide the global C ABI entry
// points expected by the launcher's calls and by the linker.
extern "C" esp_err_t psram_app_load(const char *path, psram_app_handle_t *out_handle) {
  return esphome::papp_loader::psram_app_load(path, out_handle);
}

extern "C" esp_err_t psram_app_load_url(const char *url, psram_app_handle_t *out_handle) {
  return esphome::papp_loader::psram_app_load_url(url, out_handle);
}

extern "C" int psram_app_run(psram_app_handle_t handle) {
  return esphome::papp_loader::psram_app_run(handle);
}

extern "C" void psram_app_unload(psram_app_handle_t handle) {
  esphome::papp_loader::psram_app_unload(handle);
}

extern "C" esp_err_t psram_app_selftest(void) {
  return esphome::papp_loader::psram_app_selftest();
}
