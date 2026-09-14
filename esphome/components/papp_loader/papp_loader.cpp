#include "papp_loader.h"
#include "papp_internal.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <limits>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#include "esp_cache.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_tls.h"
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
#include "freertos/semphr.h"
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
// The canvas (the app's framebuffer, centred on the panel) is runtime state:
// see papp_canvas.h and geometry_. Apps start at canvas::LEGACY_WIDTH x
// LEGACY_HEIGHT (800x480) and may switch with display_set_canvas.
static constexpr int EMU_WIDTH = 320;
static constexpr int EMU_HEIGHT = 240;
static constexpr size_t MMU_PAGE_SIZE = 0x10000;
static constexpr size_t MAX_NETWORK_PAPP_SIZE = 16 * 1024 * 1024;
static constexpr size_t HTTP_READ_BUFFER_SIZE = 16 * 1024;
static constexpr uint32_t HTTP_TIMEOUT_MS = 15000;
static constexpr uint8_t HTTP_MAX_REDIRECTIONS = 5;
// Not 3232: that is ESPHome's OTA port on ESP32, taken as soon as the YAML has `ota:`.
static constexpr uint16_t SCREEN_STREAM_PORT = 3233;
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

// PAPPFL01: a status (StreamFileStatus), then `size` bytes of the file or of a
// directory listing ("name<TAB>size" lines, directories end in '/').
struct __attribute__((packed)) StreamFileHeader {
  char magic[8];
  uint32_t status;
  uint32_t size;
  uint32_t sequence;
};
enum StreamFileStatus : uint32_t { FILE_OK = 0, FILE_NOT_FOUND = 1, FILE_TOO_LARGE = 2, FILE_BAD_PATH = 3, FILE_READ_ERROR = 4 };
static constexpr size_t STREAM_FILE_MAX_BYTES = 1024 * 1024;
static constexpr size_t STREAM_LISTING_MAX_BYTES = 64 * 1024;

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

// The shared close control. With the 800x480 canvas it sits in the unused
// right-hand margin so it cannot cover gameplay; a canvas too wide for that
// gets it over its top-right corner (canvas::Geometry). The panel's raw draw
// path is hardware-flipped, so the raw destination is the 180-degree
// counterpart of the desired physical screen position.
static constexpr int CLOSE_BUTTON_SIZE = canvas::CLOSE_SIZE;
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

std::string runtime_path(const char *path) {
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

bool is_network_url(const char *value) {
  if (value == nullptr)
    return false;
  return std::strncmp(value, "http://", 7) == 0 || std::strncmp(value, "https://", 8) == 0;
}

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

// The "◀ name ▶" row at the top of the list: a tap shows the next catalog.
static void papp_catalog_switch_event_cb(lv_event_t *event) {
  if (event != nullptr && lv_event_get_code(event) == LV_EVENT_CLICKED)
    static_cast<PappLoader *>(lv_event_get_user_data(event))->next_catalog(1);
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

  // The panel: the YAML size, else the display's own. A display that reports
  // less than the 800x480 canvas (or no size at all) is taken to be the
  // 1024x600 panel this loader was written for.
  const bool panel_configured = this->panel_config_w_ > 0 && this->panel_config_h_ > 0;
  int panel_w = panel_configured ? this->panel_config_w_ : this->display_->get_native_width();
  int panel_h = panel_configured ? this->panel_config_h_ : this->display_->get_native_height();
  const bool panel_known = panel_w >= canvas::LEGACY_WIDTH && panel_h >= canvas::LEGACY_HEIGHT;
  if (!panel_known) {
    ESP_LOGW(TAG, "Display reports %dx%d, less than the %dx%d canvas; assuming a %dx%d panel", panel_w, panel_h,
             canvas::LEGACY_WIDTH, canvas::LEGACY_HEIGHT, canvas::FALLBACK_PANEL_WIDTH, canvas::FALLBACK_PANEL_HEIGHT);
    panel_w = canvas::FALLBACK_PANEL_WIDTH;
    panel_h = canvas::FALLBACK_PANEL_HEIGHT;
  }
  panel_w = std::min(panel_w, canvas::MAX_SIDE);
  panel_h = std::min(panel_h, canvas::MAX_SIDE);
  this->geometry_.panel_w = panel_w;
  this->geometry_.panel_h = panel_h;
  this->geometry_.canvas_w = canvas::LEGACY_WIDTH;
  this->geometry_.canvas_h = canvas::LEGACY_HEIGHT;

  // Frame buffers for the largest canvas, the whole panel (canvas sizes are
  // even), allocated once. Short of PSRAM, every app stays at 800x480.
  if (!this->allocate_frame_buffers_(panel_w - panel_w % 2, panel_h - panel_h % 2) &&
      (panel_w - panel_w % 2 != canvas::LEGACY_WIDTH || panel_h - panel_h % 2 != canvas::LEGACY_HEIGHT)) {
    ESP_LOGW(TAG, "No PSRAM for %dx%d frame buffers; apps are limited to %dx%d", panel_w, panel_h,
             canvas::LEGACY_WIDTH, canvas::LEGACY_HEIGHT);
    this->allocate_frame_buffers_(canvas::LEGACY_WIDTH, canvas::LEGACY_HEIGHT);
  }
  // The canvas offered to apps that ask (display_get_size): the YAML one, else
  // the whole panel, or 800x480 when the panel's size is not known.
  int offer_w = this->default_canvas_w_, offer_h = this->default_canvas_h_;
  if (offer_w <= 0 || offer_h <= 0) {
    offer_w = panel_known || panel_configured ? this->max_canvas_w_ : canvas::LEGACY_WIDTH;
    offer_h = panel_known || panel_configured ? this->max_canvas_h_ : canvas::LEGACY_HEIGHT;
  }
  if (!canvas::size_ok(offer_w, offer_h, this->max_canvas_w_, this->max_canvas_h_)) {
    ESP_LOGW(TAG, "Default canvas %dx%d does not fit (even sizes from %dx%d to %dx%d); using %dx%d", offer_w, offer_h,
             canvas::MIN_WIDTH, canvas::MIN_HEIGHT, this->max_canvas_w_, this->max_canvas_h_, this->max_canvas_w_,
             this->max_canvas_h_);
    offer_w = this->max_canvas_w_;
    offer_h = this->max_canvas_h_;
  }
  this->default_canvas_w_ = offer_w;
  this->default_canvas_h_ = offer_h;
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
  this->handoff_mutex_ = xSemaphoreCreateMutex();
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
    // deliberately sleeps between packets, and at the same priority a game
    // that never blocks (OpenLara at ~35 FPS once the present got fast) still
    // kept screenshots from arriving. One above lets it preempt: it only runs
    // when the network needs it, then yields for 200 ms after every batch.
    // 16 KiB: `readfile` runs FATFS (long-file-name buffers) and stat() here.
    const BaseType_t result = xTaskCreatePinnedToCore(
        &PappLoader::screen_stream_task_entry_, "papp_stream", 16384, this, 6,
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
#ifdef PAPP_LOADER_USE_USB_HIDX
  // USB keyboard keys and mouse/touchpad motion reach the PAPP through
  // usb_hidx's keyboard text sensor and mouse X/Y sensors. Subscribe here so
  // the device YAML only has to declare those sensors, no lambdas.
  if (this->usb_hidx_ != nullptr) {
#ifdef USE_TEXT_SENSOR
    if (auto *keys = this->usb_hidx_->get_keyboard_sensor(); keys != nullptr)
      keys->add_on_state_callback([this](const std::string &key) { this->enqueue_keyboard_text(key); });
#endif
#ifdef USE_SENSOR
    if (auto *mouse_x = this->usb_hidx_->get_mouse_x_sensor(); mouse_x != nullptr)
      mouse_x->add_on_state_callback([this](float dx) { this->enqueue_mouse_delta(dx, 0.0f); });
    if (auto *mouse_y = this->usb_hidx_->get_mouse_y_sensor(); mouse_y != nullptr)
      mouse_y->add_on_state_callback([this](float dy) { this->enqueue_mouse_delta(0.0f, dy); });
#endif
  }
#endif
  ESP_LOGCONFIG(TAG, "PAPP runtime ready");
  ESP_LOGCONFIG(TAG, "  Path: %s", this->path_.c_str());
  const std::string mapped = runtime_path(this->path_.c_str());
  ESP_LOGCONFIG(TAG, "  SD path: %s", mapped.c_str());
  ESP_LOGCONFIG(TAG, "  Display: %dx%d panel; apps draw %dx%d unless they pick a canvas (up to %dx%d, offered %dx%d)",
                this->geometry_.panel_w, this->geometry_.panel_h, canvas::LEGACY_WIDTH, canvas::LEGACY_HEIGHT,
                this->max_canvas_w_, this->max_canvas_h_, this->default_canvas_w_, this->default_canvas_h_);
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

// ── Canvas size ──────────────────────────────────────────────────────────────

void PappLoader::free_frame_buffers_() {
  heap_caps_free(this->framebuffer_);
  heap_caps_free(this->rotated_framebuffer_);
  heap_caps_free(this->ppa_framebuffer_);
  this->framebuffer_ = nullptr;
  this->rotated_framebuffer_ = nullptr;
  this->ppa_framebuffer_ = nullptr;
  this->frame_alloc_bytes_ = 0;
}

// The three canvas buffers, each big enough for a width x height canvas and
// whole cache lines (PPA output and cache syncs work in those).
bool PappLoader::allocate_frame_buffers_(int width, int height) {
  this->free_frame_buffers_();
  const size_t bytes = canvas::align_bytes(canvas::frame_bytes(width, height));
  auto allocate = [bytes]() {
    return static_cast<uint16_t *>(
        heap_caps_aligned_calloc(canvas::CACHE_LINE, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA));
  };
  this->framebuffer_ = allocate();
  this->rotated_framebuffer_ = allocate();
  this->ppa_framebuffer_ = allocate();
  if (this->framebuffer_ == nullptr || this->rotated_framebuffer_ == nullptr || this->ppa_framebuffer_ == nullptr) {
    this->free_frame_buffers_();
    return false;
  }
  this->frame_alloc_bytes_ = bytes;
  this->max_canvas_w_ = width;
  this->max_canvas_h_ = height;
  return true;
}

void PappLoader::apply_canvas_(int width, int height, bool blank_panel) {
  this->geometry_.canvas_w = width;
  this->geometry_.canvas_h = height;
  // Nothing of the old canvas may show through: its rows have another stride.
  std::memset(this->framebuffer_, 0, this->frame_alloc_bytes_);
  std::memset(this->rotated_framebuffer_, 0, this->frame_alloc_bytes_);
  // Written back now so no dirty line can later land on PPA output, and
  // dropped from the cache so reads see what the PPA writes.
  esp_cache_msync(this->rotated_framebuffer_, this->frame_alloc_bytes_,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
  this->direct_frame_[2] = 0;  // the direct path clears its border again
  this->last_frame_direct_ = false;
  if (blank_panel && this->display_ != nullptr) {
    // The old canvas, its border and the close control may be anywhere: clear
    // the whole panel, in bands of the (now black) rotated buffer.
    const int panel_w = this->geometry_.panel_w, panel_h = this->geometry_.panel_h;
    const int rows = std::max(1, static_cast<int>(this->frame_alloc_bytes_ / sizeof(uint16_t) / panel_w));
    for (int y = 0; y < panel_h; y += rows) {
      this->display_->draw_pixels_at(0, y, panel_w, std::min(rows, panel_h - y),
                                     reinterpret_cast<const uint8_t *>(this->rotated_framebuffer_),
                                     display::COLOR_ORDER_RGB, display::COLOR_BITNESS_565, false);
    }
  }
}

// Called by the app (display_set_canvas), normally from its drawing task.
int PappLoader::set_canvas_(int width, int height) {
  if (this->framebuffer_ == nullptr || !canvas::size_ok(width, height, this->max_canvas_w_, this->max_canvas_h_)) {
    ESP_LOGW(TAG, "PAPP canvas %dx%d refused: even sizes from %dx%d to %dx%d", width, height, canvas::MIN_WIDTH,
             canvas::MIN_HEIGHT, this->max_canvas_w_, this->max_canvas_h_);
    return -1;
  }
  if (this->display_mutex_ != nullptr)
    xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY);
  const bool change = width != this->geometry_.canvas_w || height != this->geometry_.canvas_h;
  if (change)
    this->apply_canvas_(width, height, true);
  this->canvas_chosen_ = true;
  if (this->display_mutex_ != nullptr)
    xSemaphoreGiveRecursive(this->display_mutex_);
  if (change) {
    const canvas::Geometry &g = this->geometry_;
    ESP_LOGI(TAG, "PAPP canvas: %dx%d at (%d,%d) on the %dx%d panel, close control %s", width, height, g.x(), g.y(),
             g.panel_w, g.panel_h, g.close_beside() ? "beside it" : "over its top-right corner");
  }
  return 0;
}

// Before an app starts (and after one ends): the 800x480 canvas again, with
// no app loop running. Waits only briefly for the display lock: an app task
// that is gone may have died holding it.
void PappLoader::restore_legacy_canvas_() {
  this->canvas_chosen_ = false;
  if (this->framebuffer_ == nullptr ||
      (this->geometry_.canvas_w == canvas::LEGACY_WIDTH && this->geometry_.canvas_h == canvas::LEGACY_HEIGHT))
    return;
  const bool locked = this->display_mutex_ != nullptr &&
                      xSemaphoreTakeRecursive(this->display_mutex_, pdMS_TO_TICKS(200)) == pdTRUE;
  this->apply_canvas_(canvas::LEGACY_WIDTH, canvas::LEGACY_HEIGHT, true);
  if (locked)
    xSemaphoreGiveRecursive(this->display_mutex_);
}

void PappLoader::prepare_canvas_for_app_(const std::string &source) {
  this->restore_legacy_canvas_();
  const std::string app = canvas::app_key(source);
  const std::string setting = this->get_app_canvas(app);
  int w = 0, h = 0;
  const bool from_setting = !setting.empty() && canvas::parse_size(setting, &w, &h) &&
                            canvas::size_ok(w, h, this->max_canvas_w_, this->max_canvas_h_);
  // No setting: the size the app's listing recommends, if it fits.
  const std::string recommended = from_setting ? std::string() : this->listing_recommended_canvas_(source);
  const bool from_listing = !recommended.empty() && canvas::parse_size(recommended, &w, &h) &&
                            canvas::size_ok(w, h, this->max_canvas_w_, this->max_canvas_h_);
  this->offered_canvas_w_ = from_setting || from_listing ? w : this->default_canvas_w_;
  this->offered_canvas_h_ = from_setting || from_listing ? h : this->default_canvas_h_;
  ESP_LOGI(TAG, "PAPP canvas for %s: %dx%d until the app picks one; it is offered %dx%d (%s)", app.c_str(),
           canvas::LEGACY_WIDTH, canvas::LEGACY_HEIGHT, this->offered_canvas_w_, this->offered_canvas_h_,
           from_setting ? "its Screen setting" : from_listing ? "recommended by its listing" : "device default");
}

// Runs on the LVGL thread (the main loop). Copies the snapshot into a plain
// PSRAM buffer, dropping any row padding, and frees LVGL's buffer here so the
// stream task never touches LVGL memory.
void PappLoader::take_menu_shot_() {
#if defined(PAPP_LOADER_USE_LVGL) && defined(LV_USE_SNAPSHOT) && LV_USE_SNAPSHOT
  if (this->menu_shot_ready_ || this->lvgl_ == nullptr || !this->lvgl_->is_loop_started())
    return;
  lv_draw_buf_t *shot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
  if (shot == nullptr) {
    ESP_LOGW(TAG, "Menu screenshot: snapshot failed (out of memory?)");
    this->menu_shot_ready_ = true;  // send an empty frame rather than hang
    return;
  }
  const uint32_t w = shot->header.w, h = shot->header.h, stride = shot->header.stride;
  auto *pixels = static_cast<uint16_t *>(heap_caps_malloc(w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (pixels != nullptr) {
    for (uint32_t y = 0; y < h; y++)
      std::memcpy(pixels + y * w, shot->data + y * stride, w * sizeof(uint16_t));
  }
  lv_draw_buf_destroy(shot);
  // What the top layer shows (a store detail page, the progress panel) is not
  // part of the screen: blend each visible child on top.
  lv_obj_t *top = lv_layer_top();
  for (uint32_t i = 0; pixels != nullptr && top != nullptr && i < lv_obj_get_child_count(top); i++) {
    lv_obj_t *child = lv_obj_get_child(top, static_cast<int32_t>(i));
    if (child == nullptr || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN))
      continue;
    lv_draw_buf_t *layer = lv_snapshot_take(child, LV_COLOR_FORMAT_ARGB8888);
    if (layer == nullptr)
      continue;
    lv_area_t area;
    lv_obj_get_coords(child, &area);
    // The snapshot is the object plus its extra draw area on every side.
    const int32_t x0 = area.x1 - (static_cast<int32_t>(layer->header.w) - lv_area_get_width(&area)) / 2;
    const int32_t y0 = area.y1 - (static_cast<int32_t>(layer->header.h) - lv_area_get_height(&area)) / 2;
    for (int32_t y = 0; y < static_cast<int32_t>(layer->header.h); y++) {
      const int32_t sy = y0 + y;
      if (sy < 0 || sy >= static_cast<int32_t>(h))
        continue;
      const uint8_t *src = layer->data + y * layer->header.stride;
      for (int32_t x = 0; x < static_cast<int32_t>(layer->header.w); x++) {
        const int32_t sx = x0 + x;
        const uint8_t *p = src + x * 4;  // B, G, R, A
        const uint32_t a = p[3];
        if (sx < 0 || sx >= static_cast<int32_t>(w) || a == 0)
          continue;
        uint16_t &d = pixels[sy * w + sx];
        const uint32_t dr = (d >> 8) & 0xF8, dg = (d >> 3) & 0xFC, db = (d << 3) & 0xF8;
        const uint32_t r = (p[2] * a + dr * (255 - a)) / 255, g = (p[1] * a + dg * (255 - a)) / 255,
                       b = (p[0] * a + db * (255 - a)) / 255;
        d = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
      }
    }
    lv_draw_buf_destroy(layer);
  }
  this->menu_shot_ = pixels;
  this->menu_shot_w_ = pixels != nullptr ? static_cast<uint16_t>(w) : 0;
  this->menu_shot_h_ = pixels != nullptr ? static_cast<uint16_t>(h) : 0;
  this->menu_shot_ready_ = true;
#endif
}

void PappLoader::loop() {
  if (this->menu_shot_wanted_) {
    this->menu_shot_wanted_ = false;
    if (!this->launched_)
      this->take_menu_shot_();
  }
  this->update_progress_ui_();
  this->poll_info_fetch_();
  this->poll_install_();
  if (this->catalog_loading_ && this->catalog_done_) {
    if (this->papp_catalog_task_handle_ != nullptr) {
      vTaskDelete(this->papp_catalog_task_handle_);
      this->papp_catalog_task_handle_ = nullptr;
    }
    this->catalog_loading_ = false;
    if (this->catalog_fetch_url_ != this->catalog_url_) {
      // Another catalog was chosen while this one loaded: show that one instead.
      this->refresh_catalog();
    } else if (this->catalog_result_ == ESP_OK) {
      this->catalog_entries_ = parse_papp_catalog(this->catalog_html_, this->catalog_url_);
      ESP_LOGI(TAG, "PAPP catalog found %u application(s)",
               static_cast<unsigned>(this->catalog_entries_.size()));
      for (const auto &entry : this->catalog_entries_)
        ESP_LOGI(TAG, "  PAPP: %s -> %s", entry.first.c_str(), entry.second.c_str());
      this->start_info_fetch_();
#ifdef PAPP_LOADER_USE_LVGL
      this->catalog_ui_pending_ = true;
#endif
    } else {
      ESP_LOGW(TAG, "PAPP catalog request failed: %s (0x%x)",
               esp_err_to_name(static_cast<esp_err_t>(this->catalog_result_)), this->catalog_result_);
#ifdef PAPP_LOADER_USE_LVGL
      this->catalog_ui_pending_ = true;  // replace "Loading..." with the empty list
#endif
    }
  }
#ifdef PAPP_LOADER_USE_LVGL
  // While a PAPP runs, the rebuild waits: LVGL is paused under the app.
  if (this->catalog_ui_pending_ && !this->launched_ && this->catalog_container_ != nullptr &&
      this->lvgl_ != nullptr && this->lvgl_->is_loop_started()) {
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
  if (this->launched_) {
    this->poll_close_button_();
    this->snapshot_touches_();
#ifdef PAPP_LOADER_USE_LVGL
    // ESPHome's LVGL resumes itself on any touch or key while paused when
    // `resume_on_input` is on (its default), and then repaints the launcher
    // (the black shield) over the running app. Apps that redraw every frame
    // hide it; NetSurf, which only pushes changed frames, went black on the
    // first tap. Pause it again at once; `lvgl: resume_on_input: false` in
    // the YAML avoids even the one repainted frame.
    if (this->papp_task_handle_ != nullptr && !this->papp_loading_ && this->lvgl_ != nullptr &&
        this->lvgl_->is_loop_started() && !this->lvgl_->is_paused()) {
      this->lvgl_->set_paused(true, false);
      if (!this->lvgl_resume_warned_) {
        this->lvgl_resume_warned_ = true;
        ESP_LOGW(TAG, "LVGL resumed by touch/key input during a PAPP and was paused again; "
                      "set 'resume_on_input: false' under lvgl: to stop it repainting over apps");
      }
    }
#endif
  }

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
        const bool data_failed = this->papp_load_data_failed_;
        ESP_LOGE(TAG, "Network PAPP %s failed: %s (0x%x)", data_failed ? "data download" : "load",
                 esp_err_to_name(static_cast<esp_err_t>(this->papp_load_result_)), this->papp_load_result_);
        // A data failure already set a specific status line; keep it on screen.
        if (!data_failed) {
          this->set_progress_(false, 0, 0, "Could not load the app: %s",
                              esp_err_to_name(static_cast<esp_err_t>(this->papp_load_result_)));
        }
        this->papp_load_handle_ = nullptr;
        this->launched_ = false;
        this->begin_report_(this->papp_load_source_);
        this->send_report_(data_failed ? "data_failed" : "load_failed", this->papp_load_result_,
                           this->papp_load_source_);
        this->continue_chain_();  // an app handed over to that could not load: back to its caller
        return;
      }

      this->set_progress_(false, 0, 0, "%s", "");
      psram_app_handle_t app = this->papp_load_handle_;
      this->papp_load_handle_ = nullptr;
      const std::string source = this->papp_load_source_;
      if (!this->start_loaded_app_(app, source)) {
        this->launched_ = false;
        this->continue_chain_();
      }
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
  this->set_progress_(false, 0, 0, "%s", "");  // drop the previous launch's status
  // app_get_arg: what an app hand-off passed along; nothing for a launch from
  // the menu, a button or remotely, which also forgets the hand-off chain.
  if (this->handoff_mutex_ != nullptr)
    xSemaphoreTake(this->handoff_mutex_, portMAX_DELAY);
  if (!this->chain_launch_) {
    this->chain_.reset();
    this->next_arg_.clear();
  }
  this->launch_arg_ = this->next_arg_;
  this->resume_arg_ = this->next_arg_;
  this->running_source_ = this->path_;
  if (this->handoff_mutex_ != nullptr)
    xSemaphoreGive(this->handoff_mutex_);
  this->next_arg_.clear();
  this->chain_launch_ = false;
  this->launched_ = true;
  this->toggle_close_requested_ = false;
  this->global_close_requested_ = false;
  this->clear_keyboard_queue_();
  this->clear_mouse_delta_();
  if (!this->launch_()) {
    this->launched_ = false;
    this->continue_chain_();
  }
}

void PappLoader::dump_config() {
  ESP_LOGCONFIG(TAG, "PAPP loader");
  ESP_LOGCONFIG(TAG, "  Path: %s", this->path_.c_str());
  ESP_LOGCONFIG(TAG, "  Autostart: %s", YESNO(this->autostart_));
  ESP_LOGCONFIG(TAG, "  Network PAPPs: enabled with papp_loader.launch_url");
  ESP_LOGCONFIG(TAG, "  App data: %s into %s", this->download_data_ ? "download missing files" : "off",
                this->data_root_.c_str());
  for (const auto &root : this->data_search_)
    ESP_LOGCONFIG(TAG, "  App data also looked for in: %s", root.c_str());
  if (!this->catalog_url_.empty())
    ESP_LOGCONFIG(TAG, "  PAPP catalog: %s", this->catalog_url_.c_str());
  for (const auto &catalog : this->catalogs_)
    ESP_LOGCONFIG(TAG, "  PAPP library source: %s -> %s", catalog.name.c_str(), catalog.url.c_str());
}

void PappLoader::select_catalog_index(size_t index) {
  if (index >= this->catalogs_.size())
    return;
  this->catalog_index_ = index;
  this->catalog_url_ = this->catalogs_[index].url;
  ESP_LOGI(TAG, "PAPP library: %s (%s)", this->catalogs_[index].name.c_str(), this->catalog_url_.c_str());
  this->catalog_entries_.clear();
  this->start_info_fetch_();
#ifdef PAPP_LOADER_USE_LVGL
  this->catalog_ui_pending_ = true;
#endif
  this->refresh_catalog();
}

void PappLoader::select_catalog(const std::string &name) {
  auto lower = [](std::string text) {
    for (char &c : text)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
  };
  for (size_t i = 0; i < this->catalogs_.size(); i++) {
    if (lower(this->catalogs_[i].name) == lower(name)) {
      this->select_catalog_index(i);
      return;
    }
  }
  ESP_LOGW(TAG, "No PAPP catalog named %s", name.c_str());
}

void PappLoader::next_catalog(int step) {
  const int count = static_cast<int>(this->catalogs_.size());
  if (count < 2)
    return;
  const int next = ((static_cast<int>(this->catalog_index_) + step) % count + count) % count;
  this->select_catalog_index(static_cast<size_t>(next));
}

// A folder catalog (a URL starting with '/', such as /sd/roms/papp/): the
// .papp files in that folder under every data search root, so the same folder
// on the SD card and a USB stick are listed together.
std::vector<std::pair<std::string, std::string>> PappLoader::catalog_folders_(const std::string &path) const {
  std::string folder = path;
  if (folder.empty() || folder.back() != '/')
    folder += '/';
  std::vector<std::pair<std::string, std::string>> folders;  // root label, folder
  for (const auto &root : this->data_search_) {
    if (folder.rfind(root + "/", 0) == 0) {
      const std::string rest = folder.substr(root.size());
      for (const auto &other : this->data_search_)
        folders.emplace_back(other.substr(1), other + rest);
      break;
    }
  }
  if (folders.empty())
    folders.emplace_back("", folder);
  return folders;
}

void PappLoader::list_catalog_folder_() {
  std::string folder = this->catalog_url_;
  if (folder.back() != '/')
    folder += '/';
  const std::vector<std::pair<std::string, std::string>> folders = this->catalog_folders_(folder);

  std::vector<std::pair<std::string, std::string>> entries;
  for (size_t i = 0; i < folders.size(); i++) {
    DIR *dir = opendir(runtime_path(folders[i].second.c_str()).c_str());
    if (dir == nullptr)
      continue;
    while (dirent *entry = readdir(dir)) {
      std::string file = entry->d_name;
      std::string lower = file;
      for (char &c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (lower.size() <= 5 || lower.compare(lower.size() - 5, 5, ".papp") != 0)
        continue;
      std::string label = file.substr(0, file.size() - 5);
      if (i > 0)
        label += " (" + folders[i].first + ")";  // not on the first root, e.g. "(usb0)"
      entries.emplace_back(label, folders[i].second + file);
    }
    closedir(dir);
  }
  std::sort(entries.begin(), entries.end());
  this->catalog_entries_ = std::move(entries);
  ESP_LOGI(TAG, "PAPP folder catalog %s: %u application(s)", folder.c_str(),
           static_cast<unsigned>(this->catalog_entries_.size()));
  this->start_info_fetch_();
#ifdef PAPP_LOADER_USE_LVGL
  this->catalog_ui_pending_ = true;
#endif
}

void PappLoader::refresh_catalog() {
  if (this->catalog_url_.empty()) {
    ESP_LOGW(TAG, "Cannot refresh PAPP catalog without catalog_url");
    return;
  }
  if (this->catalog_url_[0] == '/') {
    this->list_catalog_folder_();
    return;
  }
  if (this->catalog_loading_) {
    // The fetch that finishes checks whether it is still for the catalog being
    // shown, and fetches the current one if not.
    ESP_LOGD(TAG, "PAPP catalog request already in progress");
    return;
  }
  if (!network::is_connected()) {
    ESP_LOGW(TAG, "Cannot refresh PAPP catalog while the device is offline");
    return;
  }

  this->catalog_html_.clear();
  this->catalog_fetch_url_ = this->catalog_url_;
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
  this->prepare_canvas_for_app_(source);
#ifdef PAPP_LOADER_USE_USB_MIDI
  // The app gets the notes played from now on, not what piled up before.
  if (this->usb_midi_ != nullptr) {
    uint8_t stale[64];
    while (this->usb_midi_->read(stale, sizeof(stale)) > 0) {
    }
  }
#endif

#ifdef PAPP_LOADER_USE_LVGL
  // LVGL must not draw over the PAPP framebuffer. Pause it from the ESPHome
  // loop task; the PAPP worker never calls LVGL directly.
  if (this->lvgl_ != nullptr && this->lvgl_->is_loop_started()) {
    // Direct-rendered PAPPs share the launcher LVGL display but have no LVGL
    // object of their own. Put a black clickable object over the whole
    // launcher, top layer included (store detail page, side menu): a release
    // cannot reach a button underneath, and the panel around the canvas is
    // black instead of a frozen store or launcher page.
    if (this->touch_modal_shield_ == nullptr) {
      lv_obj_t *top = lv_layer_top();
      if (top != nullptr) {
        this->touch_modal_shield_ = lv_obj_create(top);
        lv_obj_remove_style_all(this->touch_modal_shield_);
        lv_obj_set_size(this->touch_modal_shield_, LV_PCT(100), LV_PCT(100));
        lv_obj_center(this->touch_modal_shield_);
        lv_obj_set_style_bg_color(this->touch_modal_shield_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(this->touch_modal_shield_, LV_OPA_COVER, 0);
        lv_obj_move_foreground(this->touch_modal_shield_);
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
    // The PAPP canvas does not cover the whole panel: whatever LVGL last drew
    // around it stays visible while the app runs. Hide the finished download
    // bar and redraw once, so the panel shows only the black shield.
    this->set_progress_(false, 0, 0, "%s", "");
    this->update_progress_ui_();
    lv_refr_now(nullptr);
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

  // Arrow keys normally have a held-state path through the configured USB
  // binary sensors and read_input_(); queueing the text sensor's named arrow
  // tap as well made Quake menus advance twice. Some usb_hidx builds only
  // update key sensors for printable keys, so when the arrow's sensor is not
  // on, press the matching D-pad direction briefly instead (see read_input_).
  static const char *const arrows[4] = {"Up", "Right", "Down", "Left"};  // PAPP_INPUT_UP..LEFT order
  static const uint8_t arrow_codes[4] = {0x52, 0x4F, 0x51, 0x50};
  for (int i = 0; i < 4; i++) {
    if (text != arrows[i])
      continue;
#ifdef PAPP_LOADER_USE_USB_HIDX
    if (this->usb_hidx_ != nullptr && usb_keyboard_key_pressed(this->usb_hidx_, arrow_codes[i]))
      return;  // the held sensor has it
#endif
    this->arrow_tap_until_us_[i] = esp_timer_get_time() + ARROW_TAP_US;
    return;
  }

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
  // Fetch the app's data files first, so it never starts without them.
  const esp_err_t data_result = self->sync_app_data_(url);
  self->papp_load_data_failed_ = data_result != ESP_OK;
  if (data_result != ESP_OK) {
    self->papp_load_result_ = data_result;
  } else {
    self->papp_load_result_ = psram_app_load_url(url.c_str(), &self->papp_load_handle_);
  }
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
  const std::string url = self->catalog_fetch_url_;
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
  if (this->store_ui_) {
    // Store view: the tiles sit in one grid object under the switcher row.
    if (this->catalog_tiles_.empty()) {
      this->catalog_selection_ = 0;
      return;
    }
    index = std::min<uint16_t>(index, static_cast<uint16_t>(this->catalog_tiles_.size() - 1));
    this->catalog_selection_ = index;
    for (lv_obj_t *tile : this->catalog_tiles_)
      lv_obj_remove_state(tile, LV_STATE_FOCUSED);
    lv_obj_add_state(this->catalog_tiles_[index], LV_STATE_FOCUSED);
    lv_obj_scroll_to_view_recursive(this->catalog_tiles_[index], LV_ANIM_ON);
    return;
  }

  // `index` counts apps; the switcher row, when present, comes first.
  const uint32_t children = lv_obj_get_child_count(this->catalog_container_);
  const uint32_t count = children > this->catalog_header_rows_ ? children - this->catalog_header_rows_ : 0;
  if (count == 0) {
    this->catalog_selection_ = 0;
    return;
  }
  if (index >= count)
    index = static_cast<uint16_t>(count - 1);
  this->catalog_selection_ = index;

  for (uint32_t i = 0; i < children; i++) {
    lv_obj_t *child = lv_obj_get_child(this->catalog_container_, static_cast<int32_t>(i));
    if (child != nullptr)
      lv_obj_remove_state(child, LV_STATE_FOCUSED);
  }
  lv_obj_t *selected =
      lv_obj_get_child(this->catalog_container_, static_cast<int32_t>(index + this->catalog_header_rows_));
  if (selected != nullptr) {
    lv_obj_add_state(selected, LV_STATE_FOCUSED);
    lv_obj_scroll_to_view(selected, LV_ANIM_OFF);
  }
}

void PappLoader::handle_launcher_controls_() {
  // The launcher sees exactly what an app would: gamepad buttons, the ADC
  // ladder, sticks, the fire/touch buttons (as A), held keyboard keys
  // (arrows/WASD, Space/Enter/Z, X, Backspace...) and arrow taps. Detect edges
  // here instead of treating a held HID report or ADC value as repeated
  // presses every ESPHome loop iteration.
  papp_gamepad_state_t input;
  this->read_input_(&input);
  uint8_t direction = 0;
  for (uint8_t i = 0; i < 4; i++) {  // PAPP_INPUT_UP, RIGHT, DOWN, LEFT
    if (input.values[PAPP_INPUT_UP + i])
      direction |= 1U << i;
  }
  const bool a = input.values[PAPP_INPUT_A] != 0;
  // Menu (the keyboard's Escape) also goes back.
  const bool b = input.values[PAPP_INPUT_B] != 0 || input.values[PAPP_INPUT_MENU] != 0;
  const bool l = input.values[PAPP_INPUT_L] != 0;
  const bool r = input.values[PAPP_INPUT_R] != 0;
  const bool select = input.values[PAPP_INPUT_SELECT] != 0;
  const bool touch = false;  // read_input_ already counts the touch button as A
  const bool active = direction != 0 || a || b || l || r || select || touch;
  auto remember = [&]() {
    this->launcher_direction_state_ = direction;
    this->launcher_a_state_ = a;
    this->launcher_b_state_ = b;
    this->launcher_l_state_ = l;
    this->launcher_r_state_ = r;
    this->launcher_select_state_ = select;
    this->launcher_touch_state_ = touch;
  };

  // Do not let a controller that is held during cold boot select or scroll a
  // PAPP before the user has released it once. This is the same protection
  // used for the normal launch button and avoids the old phantom-input loop.
  if (!this->launcher_input_armed_) {
    if (!active)
      ESP_LOGD(TAG, "Launcher controls armed after input release");
    this->launcher_input_armed_ = !active;
    remember();
    return;
  }

  const uint8_t newly_pressed = direction & static_cast<uint8_t>(~this->launcher_direction_state_);
  if (this->store_ui_) {
    // Grid and detail page: d-pad moves, A opens / presses, B goes back, L/R switch
    // sources, Select opens the side menu.
    this->handle_store_controls_(newly_pressed, (a && !this->launcher_a_state_) || (touch && !this->launcher_touch_state_),
                                 b && !this->launcher_b_state_, l && !this->launcher_l_state_,
                                 r && !this->launcher_r_state_, select && !this->launcher_select_state_);
    remember();
    return;
  }
  // Left/right switch catalogs (up/down still move through the list).
  if (this->catalogs_.size() > 1 && (newly_pressed & ((1U << 1) | (1U << 3))) != 0 &&
      (newly_pressed & ((1U << 0) | (1U << 2))) == 0) {
    this->next_catalog((newly_pressed & (1U << 1)) != 0 ? 1 : -1);
  } else if (!this->catalog_entries_.empty() && newly_pressed != 0) {
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
    lv_obj_t *selected = lv_obj_get_child(this->catalog_container_,
                                          static_cast<int32_t>(this->catalog_selection_ + this->catalog_header_rows_));
    if (selected != nullptr) {
      ESP_LOGI(TAG, "PAPP catalog physical select: %u/%u", static_cast<unsigned>(this->catalog_selection_ + 1),
               static_cast<unsigned>(this->catalog_entries_.size()));
      lv_obj_send_event(selected, LV_EVENT_CLICKED, nullptr);
    }
  }

  remember();
}

void PappLoader::update_catalog_ui_() {
  if (this->catalog_container_ == nullptr)
    return;

  // The store view keeps the selection and an open detail page across rebuilds
  // (listings arriving, an install finishing).
  const uint16_t keep_selection = this->catalog_selection_;
  const std::string reopen_url = this->detail_url_;
  if (this->store_ui_)
    this->close_detail_();
  lv_obj_clean(this->catalog_container_);
  this->catalog_tiles_.clear();
  this->free_icons_();
  this->catalog_selection_ = 0;
  this->catalog_header_rows_ = 0;
  if (this->catalogs_.size() > 1) {
    const std::string title =
        std::string(LV_SYMBOL_LEFT "  ") + this->catalogs_[this->catalog_index_].name + "  " LV_SYMBOL_RIGHT;
    lv_obj_t *header = lv_list_add_btn(this->catalog_container_, nullptr, title.c_str());
    if (header != nullptr) {
      lv_obj_add_event_cb(header, papp_catalog_switch_event_cb, LV_EVENT_CLICKED, this);
      this->catalog_header_rows_ = 1;
    }
  }
  if (this->store_ui_) {
    // The list's light theme does not suit the store: dark, switcher row included.
    lv_obj_set_style_bg_color(this->catalog_container_, lv_color_hex(0x08111F), 0);
    lv_obj_set_style_text_color(this->catalog_container_, lv_color_hex(0xE2E8F0), 0);
    if (this->catalog_header_rows_ > 0) {
      lv_obj_t *header = lv_obj_get_child(this->catalog_container_, 0);
      lv_obj_set_style_bg_color(header, lv_color_hex(0x111C33), 0);
      lv_obj_set_style_text_color(header, lv_color_hex(0xE2E8F0), 0);
      lv_obj_set_style_border_color(header, lv_color_hex(0x1E2A44), 0);
    }
    this->build_drawer_();  // this source's side menu, even when it lists nothing
  }
  if (this->catalog_entries_.empty()) {
    lv_list_add_text(this->catalog_container_, this->catalog_loading_ ? "Loading..." : "No .papp files found");
    return;
  }
  if (this->store_ui_) {
    this->build_store_grid_();
    this->set_catalog_selection_(keep_selection);
    for (size_t i = 0; i < this->catalog_entries_.size() && !reopen_url.empty(); i++) {
      if (this->catalog_entries_[i].second == reopen_url) {
        this->set_catalog_selection_(static_cast<uint16_t>(i));
        this->open_detail_(static_cast<int>(i));
        break;
      }
    }
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
  close_app_sockets_();
  close_app_tls_();
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
  // The next app starts at 800x480 again. An app that had another canvas is
  // blanked from the whole panel here, so none of it shows around the next
  // one (or under a launcher without LVGL).
  this->restore_legacy_canvas_();
  // LVGL invalidation below redraws the launcher immediately. Avoid an extra
  // full PAPP flush here; that transfer can leave the last game frame
  // visible while the close path waits for a second display transaction.
  this->restore_lvgl_();
  this->send_report_(closed_by_user ? "closed" : "exited", result, this->report_source_);
  // An app it handed over to (app_open), or the app to go back to.
  this->continue_chain_();
}

void PappLoader::continue_chain_() {
  handoff::Frame next;
  bool have = false;
  if (this->handoff_mutex_ != nullptr && xSemaphoreTake(this->handoff_mutex_, portMAX_DELAY) == pdTRUE) {
    have = this->chain_.next(&next);
    xSemaphoreGive(this->handoff_mutex_);
  }
  if (!have || next.source.empty())
    return;
  ESP_LOGI(TAG, "App hand-off: starting %s%s", next.source.c_str(), next.arg.empty() ? "" : " with an argument");
  this->path_ = next.source;
  this->next_arg_ = next.arg;
  this->chain_launch_ = true;
  this->launch_pending_ = true;
}

void PappLoader::clear_(uint16_t color) {
  if (this->framebuffer_ != nullptr)
    std::fill_n(this->framebuffer_, static_cast<size_t>(this->geometry_.canvas_w) * this->geometry_.canvas_h, color);
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
      this->geometry_.close_raw_x(), this->geometry_.close_raw_y(), CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE,
      reinterpret_cast<const uint8_t *>(close_overlay_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
}

void PappLoader::clear_close_overlay_() {
  if (this->display_ == nullptr)
    return;

  std::fill_n(close_overlay_buffer, CLOSE_BUTTON_SIZE * CLOSE_BUTTON_SIZE, 0x0000);
  this->display_->draw_pixels_at(
      this->geometry_.close_raw_x(), this->geometry_.close_raw_y(), CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE,
      reinterpret_cast<const uint8_t *>(close_overlay_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
}

void PappLoader::poll_close_button_() {
  if (this->touchscreen_ == nullptr || this->global_close_requested_)
    return;

  auto touch = this->touchscreen_->get_touch();
  if (!touch.has_value() || (touch->state & touchscreen::STATE_RELEASING) != 0) {
    this->close_hold_since_us_ = 0;
    return;
  }
  this->close_touch_(touch->x, touch->y);
}

// A touch on the close control; true when it asked the app to close. Beside
// the canvas the control is drawn and a touch closes at once. A canvas too
// wide for that margin (a full-panel one) has app controls in that corner
// (Tulip's power and shuffle buttons), so the control is not drawn there:
// taps reach the app and only a touch held CLOSE_HOLD_US closes.
bool PappLoader::close_touch_(int x, int y) {
  static constexpr int64_t CLOSE_HOLD_US = 2000000;
  const canvas::Geometry geometry = this->geometry_;
  if (!geometry.in_close(x, y)) {
    this->close_hold_since_us_ = 0;
    return false;
  }
  if (!geometry.close_beside()) {
    const int64_t now = esp_timer_get_time();
    if (this->close_hold_since_us_ == 0)
      this->close_hold_since_us_ = now;
    if (now - this->close_hold_since_us_ < CLOSE_HOLD_US)
      return false;
  }
  if (!this->global_close_requested_) {
    this->begin_close_();
    ESP_LOGI(TAG, "PAPP on-screen close requested");
  }
  return true;
}

// Copies the fingers on the panel for touch_read_points: the touchscreen
// component updates its list on this (the main loop's) task.
void PappLoader::snapshot_touches_() {
  if (this->touchscreen_ == nullptr)
    return;
  const touchscreen::TouchPoints_t touches = this->touchscreen_->get_touches();
  portENTER_CRITICAL(&this->touch_points_lock_);
  int count = 0;
  for (const auto &touch : touches) {
    if (count == MAX_TOUCH_POINTS)
      break;
    if ((touch.state & touchscreen::STATE_RELEASING) == 0)
      this->touch_points_[count++] = touch;
  }
  this->touch_point_count_ = count;
  portEXIT_CRITICAL(&this->touch_points_lock_);
}

void PappLoader::flush_framebuffer_() {
  if (this->display_ == nullptr || this->framebuffer_ == nullptr)
    return;

  const int64_t flush_start_us = esp_timer_get_time();

  if (this->display_mutex_ != nullptr)
    xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY);

  const uint16_t *display_buffer = this->rotated_framebuffer_;
  this->last_frame_direct_ = false;
  bool ppa_ok = false;
  const int canvas_w = this->geometry_.canvas_w;
  const int canvas_h = this->geometry_.canvas_h;
  if (this->ppa_srm_client_ != nullptr && this->ppa_framebuffer_ != nullptr) {
    // Whole cache lines: 768,000 bytes for 800x480, as before.
    const size_t frame_bytes =
        canvas::sync_bytes(canvas::frame_bytes(canvas_w, canvas_h), this->frame_alloc_bytes_);
    esp_cache_msync(this->framebuffer_, frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = this->framebuffer_,
            .pic_w = static_cast<uint32_t>(canvas_w), .pic_h = static_cast<uint32_t>(canvas_h),
            .block_w = static_cast<uint32_t>(canvas_w), .block_h = static_cast<uint32_t>(canvas_h),
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = this->ppa_framebuffer_, .buffer_size = frame_bytes,
            .pic_w = static_cast<uint32_t>(canvas_w), .pic_h = static_cast<uint32_t>(canvas_h),
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
    for (int y = 0; y < canvas_h; y++) {
      const uint16_t *src = this->framebuffer_ + static_cast<size_t>(y) * canvas_w;
      uint16_t *dst = this->rotated_framebuffer_ + static_cast<size_t>(canvas_h - 1 - y) * canvas_w;
      for (int x = 0; x < canvas_w; x++)
        dst[canvas_w - 1 - x] = src[x];
    }
    this->direct_frame_[2] = 0;  // the direct path must clear its border again
  }
  this->send_display_buffer_(display_buffer, flush_start_us);

  if (this->display_mutex_ != nullptr)
    xSemaphoreGiveRecursive(this->display_mutex_);
}

void PappLoader::send_display_buffer_(const uint16_t *display_buffer, int64_t flush_start_us) {
  // One motion sample per second is enough for remote diagnosis and keeps the
  // raw video traffic small enough that it cannot starve the audio stream.
  static int64_t last_stream_frame_us = 0;
  const canvas::Geometry geometry = this->geometry_;
  if (this->stream_client_connected_ && this->stream_mutex_ != nullptr && display_buffer != nullptr &&
      flush_start_us - last_stream_frame_us >= 1000000) {
    if (xSemaphoreTake(this->stream_mutex_, 0) == pdTRUE) {
      // A 100x60 sample of the canvas (every 8th pixel for 800x480).
      const int x_step = geometry.canvas_w / SCREEN_STREAM_WIDTH;
      const int y_step = geometry.canvas_h / SCREEN_STREAM_HEIGHT;
      for (int y = 0; y < SCREEN_STREAM_HEIGHT; y++) {
        const uint16_t *source = display_buffer + static_cast<size_t>(y * y_step) * geometry.canvas_w;
        uint16_t *target = this->stream_framebuffer_ + y * SCREEN_STREAM_WIDTH;
        for (int x = 0; x < SCREEN_STREAM_WIDTH; x++)
          target[x] = source[x * x_step];
      }
      this->stream_frame_sequence_++;
      this->stream_frame_ready_ = true;
      last_stream_frame_us = flush_start_us;
      xSemaphoreGive(this->stream_mutex_);
    }
  }
  // The frame is turned 180 degrees, so it goes to the canvas's mirrored
  // corner: (112, 60) for 800x480 on 1024x600, (0, 0) for a full-panel canvas.
  this->display_->draw_pixels_at(
      geometry.raw_x(), geometry.raw_y(), geometry.canvas_w, geometry.canvas_h,
      reinterpret_cast<const uint8_t *>(display_buffer), display::COLOR_ORDER_RGB,
      display::COLOR_BITNESS_565, false);
  // Over the canvas (close_touch_) the control is neither drawn nor cleared:
  // either would cover the app's own corner.
  if (geometry.close_beside()) {
    if (this->launched_)
      this->draw_close_overlay_();
    else
      this->clear_close_overlay_();
  }

  const int64_t flush_us = esp_timer_get_time() - flush_start_us;
  static uint32_t flush_frames = 0;
  static int64_t flush_total_us = 0;
  static int64_t flush_worst_us = 0;
  ++flush_frames;
  flush_total_us += flush_us;
  flush_worst_us = std::max(flush_worst_us, flush_us);
  if (flush_frames == 1800) {
    ESP_LOGI(TAG, "PAPP video flush: avg=%lld us worst=%lld us last=%lld us",
             static_cast<long long>(flush_total_us / flush_frames),
             static_cast<long long>(flush_worst_us), static_cast<long long>(flush_us));
    flush_frames = 0;
    flush_total_us = 0;
    flush_worst_us = 0;
  }
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
    // A full 800x480 RGB565 frame is 768 KB (a 1024x600 canvas 1.2 MB). Allow
    // slower hosts and TCP back-pressure to drain it without truncating the
    // diagnostic frame.
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
      // A recorder that has gone away is only noticed when a send fails, and
      // with no app running nothing is sent: the next screenshot or file would
      // go to the dead socket while the new client waits. Check for its close.
      char probe;
      const int peeked = recv(client_fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
      if (peeked == 0 || (peeked < 0 && errno != EWOULDBLOCK && errno != EAGAIN))
        break;
      if (this->screenshot_requested_) {
        this->screenshot_requested_ = false;
        if (!this->send_screenshot_(client_fd))
          break;
      }
      if (this->file_requested_) {
        this->file_requested_ = false;
        if (!this->send_file_(client_fd))
          break;
      }
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
    // A request that arrived for a client that had already gone waits for the next one.
    this->stream_enabled_ = this->screenshot_requested_ || this->file_requested_;
    close(client_fd);
    ESP_LOGI(TAG, "Diagnostic screen recorder disconnected");
  }
}

// Sends one PAPPSS01 packet: the app's own canvas (800x480 unless it chose
// another size; logical orientation, the same RGB565 layout as the PAPPFB01
// thumbnails), the menu, or an empty 0x0 packet. The copy lives in PSRAM only
// while it is sent.
bool PappLoader::send_screenshot_(int client_fd) {
  static uint32_t screenshot_sequence = 0;
  uint16_t *copy = nullptr;
  uint16_t width = 0;
  uint16_t height = 0;
  size_t bytes = 0;
  if (this->launched_ && !this->papp_loading_ && this->framebuffer_ != nullptr) {
    // The app draws without this lock; holding it keeps a flush (and its
    // rotation into the panel buffer) or a canvas switch from running during
    // the copy.
    const bool locked = this->display_mutex_ != nullptr &&
                        xSemaphoreTakeRecursive(this->display_mutex_, pdMS_TO_TICKS(200)) == pdTRUE;
    const int canvas_w = this->geometry_.canvas_w;
    const int canvas_h = this->geometry_.canvas_h;
    const size_t frame_bytes = canvas::frame_bytes(canvas_w, canvas_h);
    copy = static_cast<uint16_t *>(heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy != nullptr) {
      if (this->last_frame_direct_) {
        // The frame went straight to the panel buffer, turned 180 degrees:
        // reading it backwards turns it upright again.
        const uint16_t *source = this->rotated_framebuffer_;
        const size_t pixels = static_cast<size_t>(canvas_w) * canvas_h;
        for (size_t i = 0; i < pixels; i++)
          copy[i] = source[pixels - 1 - i];
      } else {
        std::memcpy(copy, this->framebuffer_, frame_bytes);
      }
      width = static_cast<uint16_t>(canvas_w);
      height = static_cast<uint16_t>(canvas_h);
      bytes = frame_bytes;
    }
    if (locked)
      xSemaphoreGiveRecursive(this->display_mutex_);
  }
#if defined(PAPP_LOADER_USE_LVGL) && defined(LV_USE_SNAPSHOT) && LV_USE_SNAPSHOT
  if (copy == nullptr && !this->launched_) {
    // No app: the main loop snapshots the menu; wait for it briefly. A shot
    // that arrived after an earlier request gave up is stale: drop it first.
    if (this->menu_shot_ready_) {
      heap_caps_free(this->menu_shot_);
      this->menu_shot_ = nullptr;
      this->menu_shot_ready_ = false;
    }
    this->menu_shot_wanted_ = true;
    for (int waited = 0; !this->menu_shot_ready_ && waited < 1500; waited += 20)
      vTaskDelay(pdMS_TO_TICKS(20));
    if (this->menu_shot_ready_) {
      copy = this->menu_shot_;
      width = this->menu_shot_w_;
      height = this->menu_shot_h_;
      bytes = static_cast<size_t>(width) * height * sizeof(uint16_t);
      this->menu_shot_ = nullptr;
      this->menu_shot_ready_ = false;
    }
  }
#endif
  ScreenStreamHeader header{{'P','A','P','P','S','S','0','1'}, width, height, static_cast<uint32_t>(bytes),
                            ++screenshot_sequence};
  bool ok = send_screen_stream_bytes(client_fd, &header, sizeof(header));
  if (ok && copy != nullptr)
    ok = send_screen_stream_bytes(client_fd, copy, bytes);
  if (copy != nullptr) {
    ESP_LOGI(TAG, "Screenshot sent: %ux%u", static_cast<unsigned>(width), static_cast<unsigned>(height));
    heap_caps_free(copy);
  } else {
    ESP_LOGI(TAG, "Screenshot requested, but no PAPP is running (menu screenshots need LV_USE_SNAPSHOT)");
  }
  return ok;
}

void PappLoader::request_file(const std::string &path) {
  if (path.size() >= sizeof(this->file_request_path_)) {
    ESP_LOGW(TAG, "File request path is too long");
    return;
  }
  std::memcpy(this->file_request_path_, path.c_str(), path.size() + 1);
  this->file_requested_ = true;
  this->stream_enabled_ = true;
  ESP_LOGI(TAG, "File requested remotely: %s", this->file_request_path_);
}

static constexpr size_t WRITE_FILE_MAX_BYTES = 16 * 1024;

// Text files only: a written .papp or firmware image would be code. Deleting
// also takes leftovers (.bak, .log), but never game data or apps.
static bool writable_extension(const std::string &path, bool deleting = false) {
  static const char *const allowed[] = {".ini", ".cfg", ".conf", ".txt", ".json", ".yaml", ".yml", ".csv"};
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos || path.find('/', dot) != std::string::npos)
    return false;
  std::string ext = path.substr(dot);
  for (char &c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char *candidate : allowed) {
    if (ext == candidate)
      return true;
  }
  return deleting && (ext == ".bak" || ext == ".log");
}

void PappLoader::delete_file(const std::string &path) {
  if (path.rfind("/sd/", 0) != 0 || path.find("..") != std::string::npos || path.back() == '/' ||
      path.size() >= sizeof(this->file_request_path_) || !writable_extension(path, true)) {
    ESP_LOGW(TAG, "deletefile refused: %s is not a text or leftover file under /sd/", path.c_str());
    return;
  }
  if (this->launched_) {
    ESP_LOGW(TAG, "deletefile refused while an app is running (it may still use %s)", path.c_str());
    return;
  }
  const std::string local = runtime_path(path.c_str());
  struct stat info {};
  if (stat(local.c_str(), &info) != 0) {
    ESP_LOGW(TAG, "deletefile: %s does not exist", path.c_str());
    return;
  }
  if (!S_ISREG(info.st_mode)) {
    ESP_LOGW(TAG, "deletefile refused: %s is not a regular file", path.c_str());
    return;
  }
  if (unlink(local.c_str()) != 0) {
    ESP_LOGW(TAG, "deletefile: removing %s failed (errno %d)", path.c_str(), errno);
    return;
  }
  ESP_LOGI(TAG, "deletefile: removed %s (%u bytes)", path.c_str(), static_cast<unsigned>(info.st_size));
}

void PappLoader::write_file(const std::string &path, const std::string &data) {
  if (path.rfind("/sd/", 0) != 0 || path.find("..") != std::string::npos || path.back() == '/' ||
      path.size() >= sizeof(this->file_request_path_) || !writable_extension(path)) {
    ESP_LOGW(TAG, "writefile refused: %s is not a text file under /sd/", path.c_str());
    return;
  }
  if (data.size() > WRITE_FILE_MAX_BYTES || data.find('\0') != std::string::npos) {
    ESP_LOGW(TAG, "writefile refused: %u bytes (up to %u bytes of text)", static_cast<unsigned>(data.size()),
             static_cast<unsigned>(WRITE_FILE_MAX_BYTES));
    return;
  }
  if (this->launched_) {
    ESP_LOGW(TAG, "writefile refused while an app is running (it may rewrite %s on exit)", path.c_str());
    return;
  }
  const std::string local = runtime_path(path.c_str());
  const std::string backup = local + ".bak";
  struct stat info {};
  const bool existed = stat(local.c_str(), &info) == 0;
  if (existed) {
    if (!S_ISREG(info.st_mode)) {
      ESP_LOGW(TAG, "writefile refused: %s is not a regular file", path.c_str());
      return;
    }
    unlink(backup.c_str());
    if (rename(local.c_str(), backup.c_str()) != 0) {
      ESP_LOGW(TAG, "writefile: could not keep the old %s (errno %d)", path.c_str(), errno);
      return;
    }
  }
  FILE *file = std::fopen(local.c_str(), "wb");
  bool ok = file != nullptr;
  if (ok) {
    ok = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    ok = std::fclose(file) == 0 && ok;
  }
  if (!ok) {
    ESP_LOGW(TAG, "writefile: writing %s failed (errno %d)", path.c_str(), errno);
    unlink(local.c_str());
    if (existed)
      rename(backup.c_str(), local.c_str());
    return;
  }
  ESP_LOGI(TAG, "writefile: wrote %u bytes to %s%s", static_cast<unsigned>(data.size()), path.c_str(),
           existed ? " (old copy kept as .bak)" : "");
}

// Sends one PAPPFL01 packet for the requested /sd/ path: the file's bytes, or
// a listing when the path ends in '/'. Anything else gets a status and no data.
bool PappLoader::send_file_(int client_fd) {
  static uint32_t file_sequence = 0;
  char requested[sizeof(this->file_request_path_)];
  std::memcpy(requested, this->file_request_path_, sizeof(requested));
  requested[sizeof(requested) - 1] = '\0';
  const std::string path(requested);
  const std::string local = runtime_path(requested);

  uint32_t status = FILE_OK;
  uint8_t *content = nullptr;
  size_t size = 0;
  std::string listing;
  if (path.rfind("/sd/", 0) != 0 || path.find("..") != std::string::npos) {
    status = FILE_BAD_PATH;
  } else if (path.back() == '/') {
    DIR *dir = opendir(local.c_str());
    if (dir == nullptr) {
      status = FILE_NOT_FOUND;
    } else {
      while (dirent *entry = readdir(dir)) {
        struct stat info {};
        const bool known = stat((local + entry->d_name).c_str(), &info) == 0;
        listing += entry->d_name;
        if (known && S_ISDIR(info.st_mode))
          listing += '/';
        listing += '\t';
        listing += std::to_string(known ? static_cast<long long>(info.st_size) : 0LL);
        listing += '\n';
        if (listing.size() > STREAM_LISTING_MAX_BYTES) {
          status = FILE_TOO_LARGE;
          break;
        }
      }
      closedir(dir);
      if (status == FILE_OK) {
        content = reinterpret_cast<uint8_t *>(listing.data());
        size = listing.size();
      }
    }
  } else {
    FILE *file = std::fopen(local.c_str(), "rb");
    if (file == nullptr) {
      status = FILE_NOT_FOUND;
    } else {
      struct stat info {};
      if (fstat(fileno(file), &info) != 0 || info.st_size < 0) {
        status = FILE_READ_ERROR;
      } else if (static_cast<size_t>(info.st_size) > STREAM_FILE_MAX_BYTES) {
        status = FILE_TOO_LARGE;
      } else {
        size = static_cast<size_t>(info.st_size);
        content = static_cast<uint8_t *>(heap_caps_malloc(std::max<size_t>(size, 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (content == nullptr || std::fread(content, 1, size, file) != size) {
          status = FILE_READ_ERROR;
          heap_caps_free(content);
          content = nullptr;
        }
      }
      std::fclose(file);
    }
    if (status != FILE_OK)
      size = 0;
  }

  StreamFileHeader header{{'P','A','P','P','F','L','0','1'}, status, static_cast<uint32_t>(size), ++file_sequence};
  bool ok = send_screen_stream_bytes(client_fd, &header, sizeof(header));
  if (ok && size != 0)
    ok = send_screen_stream_bytes(client_fd, content, size);
  ESP_LOGI(TAG, "File %s: status %u, %u bytes", path.c_str(), static_cast<unsigned>(status),
           static_cast<unsigned>(size));
  if (content != nullptr && content != reinterpret_cast<uint8_t *>(listing.data()))
    heap_caps_free(content);
  return ok;
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

  // The frame scaled, at most the canvas, centred in it. Recomputed under the
  // display lock by the direct path, so a canvas switch never mixes sizes.
  int canvas_w = 0, canvas_h = 0, out_w = 0, out_h = 0, x0 = 0, y0 = 0;
  auto fit = [&]() {
    canvas_w = this->geometry_.canvas_w;
    canvas_h = this->geometry_.canvas_h;
    out_w = std::min(std::max(1, static_cast<int>(in_w * scale + 0.5f)), canvas_w);
    out_h = std::min(std::max(1, static_cast<int>(in_h * scale + 0.5f)), canvas_h);
    x0 = (canvas_w - out_w) / 2;
    y0 = (canvas_h - out_h) / 2;
  };

  // Fast path: one PPA pass scales the frame, turns it 180 degrees for the
  // panel and writes it centred into rotated_framebuffer_ (canvas-sized),
  // which goes straight to the display. Its black border is only cleared when
  // the frame's place changes; a frame that fills the canvas has none. This
  // replaces scaling into a scratch buffer, clearing and copying into the
  // canvas, then a second PPA pass to rotate the whole canvas (about 3 MB of
  // memory traffic per frame at 320x240 x2).
  if (!byte_swap && this->ppa_srm_client_ != nullptr && this->rotated_framebuffer_ != nullptr) {
    if (this->display_mutex_ != nullptr)
      xSemaphoreTakeRecursive(this->display_mutex_, portMAX_DELAY);
    fit();
    // Whole cache lines (768,000 bytes for 800x480, as before).
    const size_t frame_bytes =
        canvas::sync_bytes(canvas::frame_bytes(canvas_w, canvas_h), this->frame_alloc_bytes_);
    const uint16_t place[4] = {static_cast<uint16_t>(x0), static_cast<uint16_t>(y0), static_cast<uint16_t>(out_w),
                               static_cast<uint16_t>(out_h)};
    if (std::memcmp(place, this->direct_frame_, sizeof(place)) != 0) {
      std::memset(this->rotated_framebuffer_, 0, frame_bytes);
      // Written back now so no dirty border line can later land on PPA output,
      // and dropped from the cache so reads see what the PPA writes.
      esp_cache_msync(this->rotated_framebuffer_, frame_bytes,
                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
      std::memcpy(this->direct_frame_, place, sizeof(place));
    }
    esp_cache_msync(const_cast<uint16_t *>(buffer), static_cast<size_t>(in_w) * in_h * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = const_cast<uint16_t *>(buffer), .pic_w = in_w, .pic_h = in_h,
            .block_w = in_w, .block_h = in_h,
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = this->rotated_framebuffer_, .buffer_size = frame_bytes,
            .pic_w = static_cast<uint32_t>(canvas_w), .pic_h = static_cast<uint32_t>(canvas_h),
            // Turned 180 degrees, the block lands mirrored in the frame.
            .block_offset_x = static_cast<uint32_t>(canvas_w - x0 - out_w),
            .block_offset_y = static_cast<uint32_t>(canvas_h - y0 - out_h),
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_180,
        .scale_x = static_cast<float>(out_w) / in_w,
        .scale_y = static_cast<float>(out_h) / in_h,
        .mirror_x = false, .mirror_y = false,
        .rgb_swap = false, .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    const bool sent =
        ppa_do_scale_rotate_mirror(reinterpret_cast<ppa_client_handle_t>(this->ppa_srm_client_), &cfg) == ESP_OK;
    if (sent) {
      // The PPA wrote memory behind the CPU cache: invalidate so the remote
      // view sample (read by the CPU) sees this frame, not a stale one.
      esp_cache_msync(this->rotated_framebuffer_, frame_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
      this->last_frame_direct_ = true;
      this->send_display_buffer_(this->rotated_framebuffer_, render_start_us);
    } else {
      this->direct_frame_[2] = 0;
    }
    if (this->display_mutex_ != nullptr)
      xSemaphoreGiveRecursive(this->display_mutex_);
    if (sent) {
      this->log_render_time_(render_start_us, in_w, in_h, scale);
      return;
    }
  }

  fit();
  bool ppa_scaled = false;
  // The common PAPP path is an exact 2x 400x240 -> 800x480 frame. Let the
  // P4 SRM unit do that copy/scale instead of touching 384,000 pixels on the
  // worker CPU. The old scaler remains below for arbitrary emulator sizes,
  // fractional scales, and byte-swapped input.
  if (!byte_swap && this->ppa_srm_client_ != nullptr && this->ppa_framebuffer_ != nullptr &&
      out_w == canvas_w && out_h == canvas_h && x0 == 0 && y0 == 0) {
    const size_t input_bytes = static_cast<size_t>(in_w) * in_h * sizeof(uint16_t);
    const size_t output_bytes =
        canvas::sync_bytes(canvas::frame_bytes(canvas_w, canvas_h), this->frame_alloc_bytes_);
    esp_cache_msync(const_cast<uint16_t *>(buffer), input_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = const_cast<uint16_t *>(buffer), .pic_w = in_w, .pic_h = in_h,
            .block_w = in_w, .block_h = in_h,
            .block_offset_x = 0, .block_offset_y = 0, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = this->framebuffer_, .buffer_size = output_bytes,
            .pic_w = static_cast<uint32_t>(canvas_w), .pic_h = static_cast<uint32_t>(canvas_h),
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
      out_w <= canvas_w && out_h <= canvas_h) {
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
        std::memcpy(this->framebuffer_ + static_cast<size_t>(y0 + row) * canvas_w + x0,
                    this->ppa_framebuffer_ + static_cast<size_t>(row) * out_w,
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
        this->framebuffer_[static_cast<size_t>(y0 + y) * canvas_w + x0 + x] = pixel;
      }
    }
  }
  this->flush_framebuffer_();
  this->log_render_time_(render_start_us, in_w, in_h, scale);
}

void PappLoader::log_render_time_(int64_t render_start_us, uint16_t in_w, uint16_t in_h, float scale) {
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
  if (this->touch_button_ != nullptr) {
    if (!this->touch_button_->get_state())
      this->touch_button_released_();
    else if (!this->touch_button_stuck_())
      state->values[PAPP_INPUT_A] = 1;
  }

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

  // Arrow presses that only came as text (enqueue_keyboard_text): a short
  // D-pad press, enough for a menu step or a small move.
  const int64_t now_us = esp_timer_get_time();
  for (int i = 0; i < 4; i++) {
    if (now_us < this->arrow_tap_until_us_[i])
      state->values[PAPP_INPUT_UP + i] = 1;
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

  // A close (on-screen button or API) shows up as MENU, X and the dedicated L3
  // service, in the order close_buttons_() gives.
  const uint8_t close = this->close_buttons_();
  if (close & 1)
    state->values[PAPP_INPUT_MENU] = 1;
  if (close & 2)
    state->values[PAPP_INPUT_X] = 1;
}

void PappLoader::begin_close_() {
  if (this->global_close_requested_)
    return;
  this->close_requested_us_ = esp_timer_get_time();
  this->global_close_requested_ = true;
}

// Apps quit on different things: Menu pressed (most emulators; SNES only while
// X and Y are up), Menu released (Stella and Atari800 open their exit menu),
// Menu or L3 held for up to 3 s (Doom, Duke3D, Quake, OpenTyrian), or X
// (utilities such as the touch test). Holding everything at once hid the first
// two, so a close taps Menu alone, lets go, then holds Menu, X and L3 until
// the app returns.
uint8_t PappLoader::close_buttons_() const {
  if (!this->global_close_requested_)
    return 0;
  const int64_t since_ms = (esp_timer_get_time() - this->close_requested_us_) / 1000;
  if (since_ms < 250)
    return 1;  // Menu alone
  if (since_ms < 500)
    return 0;  // released
  return 1 | 2 | 4;
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
    this->close_hold_since_us_ = 0;
    return 0;
  }

  const int physical_x = touch->x;
  const int physical_y = touch->y;
  const canvas::Geometry geometry = this->geometry_;
  if (this->close_touch_(physical_x, physical_y)) {
    this->touch_active_ = true;
    return 0;
  }

  int canvas_x = 0;
  int canvas_y = 0;
  if (!geometry.to_canvas(physical_x, physical_y, &canvas_x, &canvas_y)) {
    if (!this->touch_active_)
      ESP_LOGD(TAG, "PAPP touch outside canvas: physical=(%d,%d)", physical_x, physical_y);
    return 0;
  }

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
  if (audio_now_us - last_audio_debug_log_us >= 30000000) {
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
  // A mixer/resampler speaker chain can end up "running" while nothing
  // drains it (its downstream speaker stopped on its own): every write then
  // waits out the timeout and takes nothing, and the app's audio (and a
  // movie waiting on it) stalls for good. start() does not help a speaker
  // that thinks it is running, so restart the chain after ~1 s of that.
  static int empty_writes = 0;
  empty_writes = written == 0 ? empty_writes + 1 : 0;
  if (empty_writes >= 10) {
    ESP_LOGW(TAG, "PAPP audio: speaker is not draining; restarting it");
    this->speaker_->stop();
    this->speaker_->start();
    empty_writes = 0;
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
    std::memcpy(active_->framebuffer_, buffer,
                canvas::frame_bytes(active_->geometry_.canvas_w, active_->geometry_.canvas_h));
}
void PappLoader::svc_display_get_size(int *width, int *height) {
  int w = canvas::LEGACY_WIDTH;
  int h = canvas::LEGACY_HEIGHT;
  if (active_ != nullptr) {
    // The app's own choice once it made one, else what it is offered.
    w = active_->canvas_chosen_ ? active_->geometry_.canvas_w : active_->offered_canvas_w_;
    h = active_->canvas_chosen_ ? active_->geometry_.canvas_h : active_->offered_canvas_h_;
  }
  if (width != nullptr)
    *width = w;
  if (height != nullptr)
    *height = h;
}
int PappLoader::svc_display_set_canvas(int width, int height) {
  return active_ != nullptr ? active_->set_canvas_(width, height) : -1;
}
int PappLoader::svc_touch_read_points(papp_touch_point_t *points, int max) {
  if (active_ == nullptr || points == nullptr || max <= 0)
    return 0;
  touchscreen::TouchPoint touches[MAX_TOUCH_POINTS];
  portENTER_CRITICAL(&active_->touch_points_lock_);
  const int count = active_->touch_point_count_;
  for (int i = 0; i < count; i++)
    touches[i] = active_->touch_points_[i];
  portEXIT_CRITICAL(&active_->touch_points_lock_);
  const canvas::Geometry geometry = active_->geometry_;
  int out = 0;
  for (int i = 0; i < count && out < max; i++) {
    int x = 0, y = 0;
    if (!geometry.to_canvas(touches[i].x, touches[i].y, &x, &y))
      continue;
    points[out].x = static_cast<int16_t>(x);
    points[out].y = static_cast<int16_t>(y);
    points[out].id = touches[i].id;
    out++;
  }
  return out;
}
int PappLoader::svc_midi_read(uint8_t *buf, int len) {
#ifdef PAPP_LOADER_USE_USB_MIDI
  if (active_ != nullptr && active_->usb_midi_ != nullptr && buf != nullptr && len > 0)
    return static_cast<int>(active_->usb_midi_->read(buf, static_cast<size_t>(len)));
#endif
  return 0;
}
int PappLoader::svc_midi_write(const uint8_t *data, int len) {
#ifdef PAPP_LOADER_USE_USB_MIDI
  if (active_ != nullptr && active_->usb_midi_ != nullptr && data != nullptr && len > 0)
    return active_->usb_midi_->write(data, static_cast<size_t>(len)) ? 0 : -1;
#endif
  return -1;
}
void PappLoader::svc_display_write_frame_custom(const uint16_t *buffer, uint16_t in_w, uint16_t in_h,
                                                float scale, bool byte_swap) {
  if (active_ != nullptr)
    active_->render_custom_(buffer, in_w, in_h, scale, byte_swap);
}
void PappLoader::svc_display_write_rect(int x, int y, int w, int h, const uint16_t *data) {
  if (active_ == nullptr || active_->framebuffer_ == nullptr || data == nullptr)
    return;
  const int canvas_w = active_->geometry_.canvas_w;
  const int canvas_h = active_->geometry_.canvas_h;
  for (int row = 0; row < h; row++) {
    const int dst_y = y + row;
    if (dst_y < 0 || dst_y >= canvas_h)
      continue;
    for (int col = 0; col < w; col++) {
      const int dst_x = x + col;
      if (dst_x >= 0 && dst_x < canvas_w)
        active_->framebuffer_[dst_y * canvas_w + dst_x] = data[row * w + col];
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
  if (active_->close_buttons_() & 4)
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

// ── UDP and TCP for apps (multiplayer) ──────────────────────────────────────
// Sockets are plain lwIP descriptors. The loader remembers them so a crashing
// or careless app cannot leak them past its exit. 16: an 8-player TCP game
// needs a listener, up to 7 peers and a UDP discovery socket.
static constexpr int APP_SOCKET_MAX = 16;
static int s_app_sockets[APP_SOCKET_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

static bool app_socket_slot_free() {
  for (int fd : s_app_sockets) {
    if (fd < 0)
      return true;
  }
  return false;
}

static void remember_app_socket(int fd) {
  for (int &slot : s_app_sockets) {
    if (slot < 0) {
      slot = fd;
      return;
    }
  }
}

static bool is_app_socket(int handle) {
  if (handle < 0)
    return false;
  for (int fd : s_app_sockets) {
    if (fd == handle)
      return true;
  }
  return false;
}

static bool set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void set_tcp_nodelay(int fd) {
  const int yes = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

int PappLoader::svc_net_udp_open(uint16_t port, int broadcast) {
  if (!app_socket_slot_free())
    return -1;
  const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0)
    return -1;
  const int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  if (broadcast)
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
      ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    ESP_LOGW(TAG, "PAPP UDP: cannot bind port %u (errno %d)", port, errno);
    ::close(fd);
    return -1;
  }
  remember_app_socket(fd);
  ESP_LOGI(TAG, "PAPP UDP: port %u open%s", port, broadcast ? " (broadcast)" : "");
  return fd;
}

int PappLoader::svc_net_tcp_connect(uint32_t ip, uint16_t port) {
  if (!app_socket_slot_free())
    return -1;
  const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return -1;
  set_tcp_nodelay(fd);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = htonl(ip);
  if (!set_nonblocking(fd) ||
      (::connect(fd, reinterpret_cast<sockaddr *>(&to), sizeof(to)) != 0 && errno != EINPROGRESS)) {
    ESP_LOGW(TAG, "PAPP TCP: connect to %s:%u failed (errno %d)", inet_ntoa(to.sin_addr), port, errno);
    ::close(fd);
    return -1;
  }
  remember_app_socket(fd);
  ESP_LOGI(TAG, "PAPP TCP: connecting to %s:%u", inet_ntoa(to.sin_addr), port);
  return fd;
}

int PappLoader::svc_net_tcp_listen(uint16_t port) {
  if (!app_socket_slot_free())
    return -1;
  const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return -1;
  const int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(fd, 8) != 0 ||
      !set_nonblocking(fd)) {
    ESP_LOGW(TAG, "PAPP TCP: cannot listen on port %u (errno %d)", port, errno);
    ::close(fd);
    return -1;
  }
  remember_app_socket(fd);
  ESP_LOGI(TAG, "PAPP TCP: listening on port %u", port);
  return fd;
}

int PappLoader::svc_net_tcp_accept(int handle, uint32_t *ip, uint16_t *port) {
  if (!is_app_socket(handle))
    return -1;
  sockaddr_in from{};
  socklen_t from_len = sizeof(from);
  const int fd = ::accept(handle, reinterpret_cast<sockaddr *>(&from), &from_len);
  if (fd < 0)
    return (errno == EWOULDBLOCK || errno == EAGAIN) ? -2 : -1;
  if (!app_socket_slot_free() || !set_nonblocking(fd)) {
    ::close(fd);
    return -1;
  }
  set_tcp_nodelay(fd);
  remember_app_socket(fd);
  if (ip != nullptr)
    *ip = ntohl(from.sin_addr.s_addr);
  if (port != nullptr)
    *port = ntohs(from.sin_port);
  ESP_LOGI(TAG, "PAPP TCP: accepted %s:%u", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
  return fd;
}

int PappLoader::svc_net_tcp_send(int handle, const void *buf, int len) {
  if (!is_app_socket(handle) || buf == nullptr || len < 0)
    return -1;
  const int sent = ::send(handle, buf, len, 0);
  if (sent < 0)
    return (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOMEM) ? 0 : -1;
  return sent;
}

int PappLoader::svc_net_tcp_recv(int handle, void *buf, int len) {
  if (!is_app_socket(handle) || buf == nullptr || len <= 0)
    return -1;
  const int got = ::recv(handle, buf, len, 0);
  if (got < 0)
    return (errno == EWOULDBLOCK || errno == EAGAIN) ? -2 : -1;
  return got;
}

static constexpr int APP_TLS_HANDLE_BASE = 0x10000;  // above every lwIP descriptor
static int poll_app_tls(int handle);

int PappLoader::svc_net_poll(int handle) {
  if (handle >= APP_TLS_HANDLE_BASE)
    return poll_app_tls(handle);
  if (!is_app_socket(handle))
    return -1;
  fd_set readable, writable, failed;
  FD_ZERO(&readable);
  FD_ZERO(&writable);
  FD_ZERO(&failed);
  FD_SET(handle, &readable);
  FD_SET(handle, &writable);
  FD_SET(handle, &failed);
  timeval now{0, 0};
  if (::select(handle + 1, &readable, &writable, &failed, &now) < 0)
    return -1;
  int error = 0;
  socklen_t error_len = sizeof(error);
  ::getsockopt(handle, SOL_SOCKET, SO_ERROR, &error, &error_len);
  return (FD_ISSET(handle, &readable) ? 1 : 0) | (FD_ISSET(handle, &writable) ? 2 : 0) |
         (FD_ISSET(handle, &failed) || error != 0 ? 4 : 0);
}

int PappLoader::svc_net_resolve(const char *host, uint32_t *ip) {
  if (host == nullptr || *host == '\0' || ip == nullptr)
    return 0;
  in_addr parsed{};
  if (inet_aton(host, &parsed) != 0) {
    *ip = ntohl(parsed.s_addr);
    return 1;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *found = nullptr;
  if (getaddrinfo(host, nullptr, &hints, &found) != 0 || found == nullptr)
    return 0;
  *ip = ntohl(reinterpret_cast<sockaddr_in *>(found->ai_addr)->sin_addr.s_addr);
  freeaddrinfo(found);
  return 1;
}

int PappLoader::svc_net_udp_send(int handle, const void *buf, int len, uint32_t ip, uint16_t port) {
  if (handle < 0 || buf == nullptr || len < 0)
    return -1;
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = htonl(ip);
  const int sent = ::sendto(handle, buf, len, 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
  if (sent < 0)
    return (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOMEM) ? 0 : -1;
  return sent;
}

int PappLoader::svc_net_udp_recv(int handle, void *buf, int len, uint32_t *ip, uint16_t *port) {
  if (handle < 0 || buf == nullptr || len <= 0)
    return -1;
  sockaddr_in from{};
  socklen_t from_len = sizeof(from);
  const int got = ::recvfrom(handle, buf, len, 0, reinterpret_cast<sockaddr *>(&from), &from_len);
  if (got < 0)
    return (errno == EWOULDBLOCK || errno == EAGAIN) ? 0 : -1;
  if (ip != nullptr)
    *ip = ntohl(from.sin_addr.s_addr);
  if (port != nullptr)
    *port = ntohs(from.sin_port);
  return got;
}

void PappLoader::svc_net_udp_close(int handle) {
  for (int &fd : s_app_sockets) {
    if (fd == handle && fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
}

int PappLoader::svc_net_ipv4(uint32_t *ip, uint32_t *netmask) {
  esp_netif_t *netif = esp_netif_get_default_netif();
  esp_netif_ip_info_t info{};
  if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0)
    return 0;
  if (ip != nullptr)
    *ip = ntohl(info.ip.addr);
  if (netmask != nullptr)
    *netmask = ntohl(info.netmask.addr);
  return 1;
}

void PappLoader::close_app_sockets_() {
  for (int &fd : s_app_sockets) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
}

// ── TLS (HTTPS) connections for apps ────────────────────────────────────────
// esp-tls does the work. Name lookup, the TCP connect and the handshake (the
// server's certificate checked against the firmware's CA bundle, SNI and the
// expected name from `host`) run on a short-lived task per connection, so the
// app's task never waits on DNS, the network or the handshake's crypto. Once
// ready, the socket is switched to non-blocking and the app's calls read and
// write through esp_tls_conn_read/write, which return at once.
//
// Each session costs mbedTLS's record buffers (CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN
// and _OUT_CONTENT_LEN, 16 KB and 4 KB by default) plus its context, about
// 25 KB of internal RAM, more during the handshake, and an lwIP socket (the
// component reserves APP_TLS_MAX in __init__.py).
static constexpr int APP_TLS_MAX = 4;
static constexpr int APP_TLS_TIMEOUT_MS = 15000;
static constexpr uint32_t APP_TLS_TASK_STACK = 8192;
static constexpr size_t APP_TLS_HOST_MAX = 253;  // the longest DNS name

enum AppTlsState : int8_t { TLS_CONNECTING = 0, TLS_READY = 1, TLS_FAILED = -1, TLS_CLOSED = 2 };

struct AppTlsSession {
  int handle{-1};         // the app's handle; -1 when the app holds none
  bool worker{false};     // a connect task still owns this slot
  AppTlsState state{TLS_FAILED};
  esp_tls_t *tls{nullptr};
};

struct AppTlsJob {
  int slot;
  int handle;
  uint16_t port;
  char host[APP_TLS_HOST_MAX + 1];
};

static AppTlsSession s_app_tls[APP_TLS_MAX];
static SemaphoreHandle_t s_app_tls_lock = nullptr;  // created by populate_services
static uint32_t s_app_tls_generation = 0;

// Every session change and every esp_tls call on an app's session happens
// under s_app_tls_lock, so a close from one task cannot free a session another
// is reading. The calls under it do not block (the sockets are non-blocking).
class AppTlsLock {
 public:
  explicit AppTlsLock(TickType_t wait = pdMS_TO_TICKS(1000)) : lock_(s_app_tls_lock) {
    if (this->lock_ != nullptr && xSemaphoreTake(this->lock_, wait) != pdTRUE)
      this->lock_ = nullptr;
  }
  ~AppTlsLock() {
    if (this->lock_ != nullptr)
      xSemaphoreGive(this->lock_);
  }
  AppTlsLock(const AppTlsLock &) = delete;
  AppTlsLock &operator=(const AppTlsLock &) = delete;
  bool held() const { return this->lock_ != nullptr; }

 private:
  SemaphoreHandle_t lock_;  // the lock taken (close_app_tls_ may replace s_app_tls_lock)
};

static AppTlsSession *find_app_tls(int handle) {
  if (handle < APP_TLS_HANDLE_BASE)
    return nullptr;
  for (auto &session : s_app_tls) {
    if (session.handle == handle)
      return &session;
  }
  return nullptr;
}

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
static void app_tls_connect_task(void *arg) {
  auto *job = static_cast<AppTlsJob *>(arg);
  const int64_t started_us = esp_timer_get_time();
  esp_tls_t *tls = nullptr;
  bool ready = false;
  uint32_t ip = 0;
  if (!PappLoader::svc_net_resolve(job->host, &ip)) {
    ESP_LOGW(TAG, "PAPP TLS: %s not found", job->host);
  } else if ((tls = esp_tls_init()) == nullptr) {
    ESP_LOGW(TAG, "PAPP TLS: out of memory for %s", job->host);
  } else {
    char address[16];
    std::snprintf(address, sizeof(address), "%u.%u.%u.%u", static_cast<unsigned>(ip >> 24),
                  static_cast<unsigned>((ip >> 16) & 0xFF), static_cast<unsigned>((ip >> 8) & 0xFF),
                  static_cast<unsigned>(ip & 0xFF));
    esp_tls_cfg_t cfg{};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.common_name = job->host;  // SNI and the name the certificate must carry
    cfg.timeout_ms = APP_TLS_TIMEOUT_MS;
    int fd = -1;
    if (esp_tls_conn_new_sync(address, static_cast<int>(std::strlen(address)), job->port, &cfg, tls) == 1 &&
        esp_tls_get_conn_sockfd(tls, &fd) == ESP_OK && fd >= 0 && set_nonblocking(fd)) {
      ready = true;
      ESP_LOGI(TAG, "PAPP TLS: %s:%u ready (%lld ms)", job->host, job->port,
               static_cast<long long>((esp_timer_get_time() - started_us) / 1000));
    } else {
      esp_tls_error_handle_t error = nullptr;
      if (esp_tls_get_error_handle(tls, &error) == ESP_OK && error != nullptr) {
        ESP_LOGW(TAG, "PAPP TLS: %s:%u failed: %s (TLS error -0x%04x, certificate flags 0x%x)", job->host, job->port,
                 esp_err_to_name(error->last_error), static_cast<unsigned>(-error->esp_tls_error_code),
                 static_cast<unsigned>(error->esp_tls_flags));
      } else {
        ESP_LOGW(TAG, "PAPP TLS: %s:%u failed", job->host, job->port);
      }
    }
  }

  bool kept = false;
  for (;;) {
    AppTlsLock lock;  // retried, so a lock replaced by close_app_tls_ is picked up
    if (!lock.held())
      continue;
    AppTlsSession &session = s_app_tls[job->slot];
    session.worker = false;
    if (session.handle == job->handle) {  // the app has not closed it meanwhile
      session.state = ready ? TLS_READY : TLS_FAILED;
      if (ready) {
        session.tls = tls;
        kept = true;
      }
    }
    break;
  }
  if (!kept && tls != nullptr)
    esp_tls_conn_destroy(tls);
  std::free(job);
  vTaskDelete(nullptr);
}
#endif

int PappLoader::svc_net_tls_connect(const char *host, uint16_t port) {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (host == nullptr || *host == '\0' || port == 0 || std::strlen(host) > APP_TLS_HOST_MAX)
    return -1;
  auto *job = static_cast<AppTlsJob *>(std::calloc(1, sizeof(AppTlsJob)));
  if (job == nullptr)
    return -1;
  AppTlsLock lock;
  int slot = -1;
  for (int i = 0; lock.held() && i < APP_TLS_MAX; i++) {
    if (s_app_tls[i].handle < 0 && !s_app_tls[i].worker && s_app_tls[i].tls == nullptr) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    ESP_LOGW(TAG, "PAPP TLS: no free session for %s (at most %d)", host, APP_TLS_MAX);
    std::free(job);
    return -1;
  }
  // A new number each time, so a stale handle never reaches a later session.
  s_app_tls_generation = (s_app_tls_generation + 1) & 0xFFFF;
  const int handle = APP_TLS_HANDLE_BASE + static_cast<int>(s_app_tls_generation) * APP_TLS_MAX + slot;
  job->slot = slot;
  job->handle = handle;
  job->port = port;
  std::memcpy(job->host, host, std::strlen(host) + 1);
  AppTlsSession &session = s_app_tls[slot];
  session.handle = handle;
  session.worker = true;
  session.state = TLS_CONNECTING;
  session.tls = nullptr;
  if (xTaskCreatePinnedToCore(&app_tls_connect_task, "papp_tls", APP_TLS_TASK_STACK, job, 5, nullptr,
                              tskNO_AFFINITY) != pdPASS) {
    ESP_LOGW(TAG, "PAPP TLS: cannot start a connect task for %s", host);
    session = AppTlsSession{};
    std::free(job);
    return -1;
  }
  ESP_LOGI(TAG, "PAPP TLS: connecting to %s:%u", host, port);
  return handle;
#else
  ESP_LOGW(TAG, "PAPP TLS: unavailable, this firmware has no CA bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)");
  return -1;
#endif
}

int PappLoader::svc_net_tls_status(int handle) {
  AppTlsLock lock;
  const AppTlsSession *session = lock.held() ? find_app_tls(handle) : nullptr;
  if (session == nullptr)
    return -1;
  switch (session->state) {
    case TLS_CONNECTING:
      return 0;
    case TLS_READY:
    case TLS_CLOSED:
      return 1;
    default:
      return -1;
  }
}

int PappLoader::svc_net_tls_send(int handle, const void *buf, int len) {
  if (buf == nullptr || len < 0)
    return -1;
  AppTlsLock lock;
  AppTlsSession *session = lock.held() ? find_app_tls(handle) : nullptr;
  if (session == nullptr)
    return -1;
  if (session->state == TLS_CONNECTING)
    return 0;
  if (session->state != TLS_READY || session->tls == nullptr)
    return -1;
  if (len == 0)
    return 0;
  const ssize_t sent = esp_tls_conn_write(session->tls, buf, static_cast<size_t>(len));
  if (sent > 0)
    return static_cast<int>(sent);
  if (sent == 0 || sent == ESP_TLS_ERR_SSL_WANT_WRITE || sent == ESP_TLS_ERR_SSL_WANT_READ)
    return 0;
  ESP_LOGW(TAG, "PAPP TLS: send failed (-0x%04x)", static_cast<unsigned>(-sent));
  session->state = TLS_FAILED;
  return -1;
}

int PappLoader::svc_net_tls_recv(int handle, void *buf, int len) {
  if (buf == nullptr || len <= 0)
    return -1;
  AppTlsLock lock;
  AppTlsSession *session = lock.held() ? find_app_tls(handle) : nullptr;
  if (session == nullptr)
    return -1;
  if (session->state == TLS_CONNECTING)
    return 0;
  if (session->state != TLS_READY || session->tls == nullptr)
    return -1;
  const ssize_t got = esp_tls_conn_read(session->tls, buf, static_cast<size_t>(len));
  if (got > 0)
    return static_cast<int>(got);
  if (got == ESP_TLS_ERR_SSL_WANT_READ || got == ESP_TLS_ERR_SSL_WANT_WRITE)
    return 0;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
  if (got == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)  // TLS 1.3 housekeeping, no data
    return 0;
#endif
  // 0 is the server's close_notify; a plain TCP close without one ends the
  // stream too (HTTP servers often do that after the response).
  bool closed = got == 0;
#ifdef MBEDTLS_ERR_SSL_CONN_EOF
  closed = closed || got == MBEDTLS_ERR_SSL_CONN_EOF;
#endif
#ifdef MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY
  closed = closed || got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
#endif
  if (!closed)
    ESP_LOGW(TAG, "PAPP TLS: receive failed (-0x%04x)", static_cast<unsigned>(-got));
  session->state = closed ? TLS_CLOSED : TLS_FAILED;
  return -1;
}

void PappLoader::svc_net_tls_close(int handle) {
  esp_tls_t *tls = nullptr;
  {
    AppTlsLock lock;
    AppTlsSession *session = lock.held() ? find_app_tls(handle) : nullptr;
    if (session == nullptr)
      return;
    // A connect task still running sees the handle gone and frees its own
    // connection; the slot stays taken until then.
    tls = session->tls;
    session->handle = -1;
    session->tls = nullptr;
    session->state = TLS_FAILED;
  }
  if (tls != nullptr)
    esp_tls_conn_destroy(tls);
}

// net_poll for a TLS handle; see psram_app.h.
static int poll_app_tls(int handle) {
  AppTlsLock lock;
  const AppTlsSession *session = lock.held() ? find_app_tls(handle) : nullptr;
  if (session == nullptr)
    return -1;
  switch (session->state) {
    case TLS_CONNECTING:
      return 0;
    case TLS_CLOSED:
      return 1;  // net_tls_recv reports the close
    case TLS_READY:
      break;
    default:
      return 4;
  }
  int fd = -1;
  if (session->tls == nullptr || esp_tls_get_conn_sockfd(session->tls, &fd) != ESP_OK || fd < 0)
    return 4;
  int ready = esp_tls_get_bytes_avail(session->tls) > 0 ? 1 : 0;
  fd_set readable, writable, failed;
  FD_ZERO(&readable);
  FD_ZERO(&writable);
  FD_ZERO(&failed);
  FD_SET(fd, &readable);
  FD_SET(fd, &writable);
  FD_SET(fd, &failed);
  timeval now{0, 0};
  if (::select(fd + 1, &readable, &writable, &failed, &now) < 0)
    return ready | 4;
  return ready | (FD_ISSET(fd, &readable) ? 1 : 0) | (FD_ISSET(fd, &writable) ? 2 : 0) |
         (FD_ISSET(fd, &failed) ? 4 : 0);
}

void PappLoader::close_app_tls_() {
  if (s_app_tls_lock == nullptr)
    return;
  esp_tls_t *open[APP_TLS_MAX] = {};
  {
    AppTlsLock lock(pdMS_TO_TICKS(2000));
    if (!lock.held()) {
      // Only a task deleted inside a TLS call leaves the lock taken. Give up on
      // its sessions (their memory is lost, the rest works) and start a new
      // lock, so later apps still get TLS. Connect tasks still running find
      // their handle gone and free their own connections.
      ESP_LOGW(TAG, "PAPP TLS: lock still taken after the app exited; abandoning its sessions");
      for (auto &session : s_app_tls) {
        session.handle = -1;
        session.tls = nullptr;
        session.state = TLS_FAILED;
      }
      s_app_tls_lock = xSemaphoreCreateMutex();
      return;
    }
    for (int i = 0; i < APP_TLS_MAX; i++) {
      AppTlsSession &session = s_app_tls[i];
      if (session.handle >= 0)
        ESP_LOGI(TAG, "PAPP TLS: closing a session the app left open");
      open[i] = session.tls;
      session.handle = -1;
      session.tls = nullptr;
      session.state = TLS_FAILED;
    }
  }
  for (esp_tls_t *tls : open) {
    if (tls != nullptr)
      esp_tls_conn_destroy(tls);
  }
}

// ── App hand-off (app_open, app_get_arg, app_set_resume_arg) ────────────────
// app_open runs on the calling app's task: it finds the target, queues the
// request in chain_ and asks the app to close. finish_app_ (the main loop)
// then takes the next launch from chain_ (continue_chain_).

// An HTTP catalog page fetched for app_open on a task of the loader's own (the
// calling app's stack may be small or in PSRAM). The job outlives a caller
// that stops waiting.
struct CatalogLookup {
  std::string url;
  std::string html;
  esp_err_t result{ESP_FAIL};
  volatile bool done{false};
};

static void catalog_lookup_task(void *arg) {
  auto *slot = static_cast<std::shared_ptr<CatalogLookup> *>(arg);
  std::shared_ptr<CatalogLookup> job = std::move(*slot);
  delete slot;
  job->result = fetch_http_text(job->url.c_str(), &job->html);
  job->done = true;
  job.reset();
  vTaskDelete(nullptr);
}

static constexpr int64_t CATALOG_LOOKUP_TIMEOUT_US = 20000000;

std::vector<std::string> PappLoader::folder_papps_(const std::string &folder) const {
  std::vector<std::string> papps;
  for (const auto &place : this->catalog_folders_(folder)) {
    DIR *dir = opendir(runtime_path(place.second.c_str()).c_str());
    if (dir == nullptr)
      continue;
    while (dirent *entry = readdir(dir)) {
      const std::string file = entry->d_name;
      if (handoff::is_papp_file(file))
        papps.push_back(place.second + file);
    }
    closedir(dir);
  }
  return papps;
}

std::string PappLoader::resolve_app_(const std::string &name) {
  // The installed copy, as the store's Launch button prefers it.
  const std::string installed = this->install_dir_ + "/" + name + ".papp";
  struct stat st{};
  if (stat(runtime_path(installed.c_str()).c_str(), &st) == 0 && S_ISREG(st.st_mode))
    return installed;
  // Then the library sources in order. catalogs_ and a lone catalog_url_ are
  // only set up from YAML, so they are safe to read from the app's task.
  std::vector<std::string> sources;
  for (const auto &catalog : this->catalogs_)
    sources.push_back(catalog.url);
  if (sources.empty() && !this->catalog_url_.empty())
    sources.push_back(this->catalog_url_);
  for (const auto &source : sources) {
    std::vector<std::string> candidates;
    if (!source.empty() && source[0] == '/') {
      candidates = this->folder_papps_(source);
    } else if (is_network_url(source.c_str()) && network::is_connected()) {
      auto job = std::make_shared<CatalogLookup>();
      job->url = source;
      auto *slot = new std::shared_ptr<CatalogLookup>(job);
      if (xTaskCreatePinnedToCore(&catalog_lookup_task, "papp_lookup", 8192, slot, 4, nullptr, 0) != pdPASS) {
        delete slot;
        ESP_LOGW(TAG, "App hand-off: could not start the catalog lookup for %s", source.c_str());
        continue;
      }
      const int64_t start = esp_timer_get_time();
      while (!job->done && esp_timer_get_time() - start < CATALOG_LOOKUP_TIMEOUT_US)
        vTaskDelay(pdMS_TO_TICKS(20));
      if (!job->done) {
        ESP_LOGW(TAG, "App hand-off: catalog %s did not answer in time", source.c_str());
        continue;
      }
      if (job->result != ESP_OK) {
        ESP_LOGW(TAG, "App hand-off: catalog %s failed: %s", source.c_str(), esp_err_to_name(job->result));
        continue;
      }
      for (const auto &entry : parse_papp_catalog(job->html, source))
        candidates.push_back(entry.second);
    }
    const std::string found = handoff::best_match(candidates, name);
    if (!found.empty())
      return found;
  }
  return {};
}

int PappLoader::svc_app_open(const char *target, const char *arg, int return_after) {
  PappLoader *self = active_;
  if (self == nullptr || !self->launched_ || self->handoff_mutex_ == nullptr || target == nullptr)
    return -1;
  const std::string wanted(target, strnlen(target, handoff::MAX_TARGET + 1));
  const size_t arg_len = arg != nullptr ? strnlen(arg, handoff::MAX_ARG + 1) : 0;
  if (arg_len > handoff::MAX_ARG) {
    ESP_LOGW(TAG, "App hand-off to %.64s refused: the argument is longer than %u bytes", wanted.c_str(),
             static_cast<unsigned>(handoff::MAX_ARG));
    return -1;
  }
  if (self->global_close_requested_ || self->toggle_close_requested_) {
    ESP_LOGW(TAG, "App hand-off to %.64s refused: the app is already closing", wanted.c_str());
    return -1;
  }
  std::string source;
  switch (handoff::classify(wanted)) {
    case handoff::Target::URL:
      if (network::is_connected())
        source = wanted;
      else
        ESP_LOGW(TAG, "App hand-off to %s refused: the device is offline", wanted.c_str());
      break;
    case handoff::Target::PATH: {
      struct stat st{};
      if (stat(runtime_path(wanted.c_str()).c_str(), &st) == 0 && S_ISREG(st.st_mode))
        source = wanted;
      else
        ESP_LOGW(TAG, "App hand-off: %s not found", wanted.c_str());
      break;
    }
    case handoff::Target::NAME:
      source = self->resolve_app_(wanted);
      if (source.empty())
        ESP_LOGW(TAG, "App hand-off: no app %s installed or in the library sources", wanted.c_str());
      break;
    default:
      ESP_LOGW(TAG, "App hand-off refused: '%.64s' is not an app name, .papp URL or /sd path", wanted.c_str());
      break;
  }
  if (source.empty())
    return -1;

  bool accepted = false;
  if (xSemaphoreTake(self->handoff_mutex_, pdMS_TO_TICKS(1000)) == pdTRUE) {
    const handoff::Frame caller{self->running_source_, self->resume_arg_};
    const handoff::Frame next{source, arg_len > 0 ? std::string(arg, arg_len) : std::string()};
    accepted = self->chain_.request(caller, next, return_after != 0);
    xSemaphoreGive(self->handoff_mutex_);
  }
  if (!accepted) {
    ESP_LOGW(TAG, "App hand-off to %s refused: another one is under way, or %u apps already wait to be returned to",
             source.c_str(), static_cast<unsigned>(handoff::MAX_DEPTH));
    return -1;
  }
  ESP_LOGI(TAG, "App hand-off: %s opens %s%s; closing it", self->running_source_.c_str(), source.c_str(),
           return_after != 0 ? " and comes back afterwards" : "");
  self->begin_close_();
  return 0;
}

int PappLoader::svc_app_get_arg(char *buf, int len) {
  PappLoader *self = active_;
  // launch_arg_ is set before the app starts and not changed while it runs.
  return handoff::copy_arg(self != nullptr ? self->launch_arg_ : std::string(), buf, len);
}

int PappLoader::svc_app_set_resume_arg(const char *arg) {
  PappLoader *self = active_;
  const size_t len = arg != nullptr ? strnlen(arg, handoff::MAX_ARG + 1) : 0;
  if (self == nullptr || self->handoff_mutex_ == nullptr || len > handoff::MAX_ARG)
    return -1;
  if (xSemaphoreTake(self->handoff_mutex_, pdMS_TO_TICKS(1000)) != pdTRUE)
    return -1;
  self->resume_arg_.assign(arg != nullptr ? arg : "", len);
  xSemaphoreGive(self->handoff_mutex_);
  return 0;
}

int PappLoader::svc_file_list_dir(const char *path, char *buf, int len) {
  if (path == nullptr)
    return -1;
  const std::string folder = runtime_path(path);
  DIR *dir = opendir(folder.c_str());
  if (dir == nullptr)
    return -1;
  int count = 0;
  size_t used = 0;
  const size_t size = buf != nullptr && len > 0 ? static_cast<size_t>(len) : 0;
  while (dirent *entry = readdir(dir)) {
    const char *name = entry->d_name;
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
      continue;
    bool is_dir = entry->d_type == DT_DIR;
    if (entry->d_type == DT_UNKNOWN) {
      struct stat st{};
      const std::string full = folder + (folder.empty() || folder.back() == '/' ? "" : "/") + name;
      is_dir = stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
    const size_t need = std::strlen(name) + (is_dir ? 1 : 0) + 1;
    if (used + need > size)
      continue;  // does not fit; a shorter name later might
    std::memcpy(buf + used, name, need - 1 - (is_dir ? 1 : 0));
    used += need - 1 - (is_dir ? 1 : 0);
    if (is_dir)
      buf[used++] = '/';
    buf[used++] = '\0';
    count++;
  }
  closedir(dir);
  return count;
}

// ── File management services ───────────────────────────────────────────────
// The card (/sd) and the loader's data roots only; papp_files.h checks the path.

std::string PappLoader::app_file_path_(const char *path, std::string *root) {
  PappLoader *self = active_;
  if (self == nullptr)
    return {};
  std::vector<std::string> roots = self->data_roots_();
  roots.emplace_back("/sd");
  std::string clean, matched;
  if (!files::clean_app_path(path, roots, &clean, &matched)) {
    ESP_LOGW(TAG, "App file path refused: %.80s", path != nullptr ? path : "(null)");
    return {};
  }
  if (root != nullptr)
    *root = runtime_path(matched.c_str());
  return runtime_path(clean.c_str());
}

// A storage root is a mount point, which FAT cannot stat: it counts as a
// folder when it can be listed.
static bool stat_app_path(const std::string &local, const std::string &root, struct stat *st) {
  if (local != root)
    return stat(local.c_str(), st) == 0;
  DIR *dir = opendir(local.c_str());
  if (dir == nullptr)
    return false;
  closedir(dir);
  *st = {};
  st->st_mode = S_IFDIR;
  return true;
}

int PappLoader::svc_file_mkdir(const char *path) {
  std::string root;
  const std::string local = app_file_path_(path, &root);
  struct stat st{};
  if (local.empty())
    return -1;
  if (local == root)
    return stat_app_path(local, root, &st) ? 0 : -1;
  // Each folder below the root, from the top down.
  for (size_t slash = local.find('/', root.size() + 1);; slash = local.find('/', slash + 1)) {
    const std::string part = slash == std::string::npos ? local : local.substr(0, slash);
    if (stat(part.c_str(), &st) == 0) {
      if (!S_ISDIR(st.st_mode))
        return -1;  // a file is in the way
    } else if (mkdir(part.c_str(), 0775) != 0 && errno != EEXIST) {
      ESP_LOGW(TAG, "App mkdir %s failed (errno %d)", part.c_str(), errno);
      return -1;
    }
    if (slash == std::string::npos)
      return 0;
  }
}

int PappLoader::svc_file_remove(const char *path) {
  std::string root;
  const std::string local = app_file_path_(path, &root);
  struct stat st{};
  if (local.empty() || local == root || stat(local.c_str(), &st) != 0)
    return -1;
  const int result = S_ISDIR(st.st_mode) ? rmdir(local.c_str()) : unlink(local.c_str());
  if (result != 0)
    ESP_LOGW(TAG, "App remove %s failed (errno %d)", local.c_str(), errno);
  return result == 0 ? 0 : -1;
}

int PappLoader::svc_file_rename(const char *from, const char *to) {
  std::string from_root, to_root;
  const std::string source = app_file_path_(from, &from_root);
  const std::string target = app_file_path_(to, &to_root);
  struct stat st{};
  if (source.empty() || target.empty() || source == from_root || target == to_root ||
      stat(source.c_str(), &st) != 0)
    return -1;
  if (stat(target.c_str(), &st) == 0)
    return -1;  // never replaces: FAT refuses that anyway, so every file system behaves alike
  if (std::rename(source.c_str(), target.c_str()) != 0) {
    ESP_LOGW(TAG, "App rename %s -> %s failed (errno %d)", source.c_str(), target.c_str(), errno);
    return -1;
  }
  return 0;
}

int PappLoader::svc_file_stat(const char *path, papp_file_stat_t *out) {
  if (out == nullptr)
    return -1;
  std::memset(out, 0, sizeof(*out));
  std::string root;
  const std::string local = app_file_path_(path, &root);
  struct stat st{};
  if (local.empty() || !stat_app_path(local, root, &st))
    return -1;
  out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
  out->size = out->is_dir ? 0 : static_cast<uint64_t>(st.st_size);
  out->mtime = st.st_mtime > 0 ? static_cast<int64_t>(st.st_mtime) : 0;
  return 0;
}

void *PappLoader::svc_file_open(const char *path, const char *mode) {
  const std::string mapped = runtime_path(path);
  FILE *file = std::fopen(mapped.c_str(), mode);
  // Apps open their data at fixed /sd/... paths. When a read-only open misses
  // the SD card, try the same path under data_root and the data_search roots,
  // so data kept on (or downloaded to) /usb0 works without changing the app.
  // Writes (saves, configs) always stay where the app asked.
  if (file != nullptr || active_ == nullptr || path == nullptr || mode == nullptr || mode[0] != 'r' ||
      std::strchr(mode, '+') != nullptr || std::strncmp(path, "/sd/", 4) != 0)
    return file;
  const std::string found = active_->find_data_file_(path + 4);
  if (found.empty() || found == mapped)
    return nullptr;
  ESP_LOGD(TAG, "App read %s -> %s", path, found.c_str());
  return std::fopen(found.c_str(), mode);
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
  services->net_udp_open = &PappLoader::svc_net_udp_open;
  services->net_udp_send = &PappLoader::svc_net_udp_send;
  services->net_udp_recv = &PappLoader::svc_net_udp_recv;
  services->net_udp_close = &PappLoader::svc_net_udp_close;
  services->net_ipv4 = &PappLoader::svc_net_ipv4;
  services->net_tcp_connect = &PappLoader::svc_net_tcp_connect;
  services->net_tcp_listen = &PappLoader::svc_net_tcp_listen;
  services->net_tcp_accept = &PappLoader::svc_net_tcp_accept;
  services->net_tcp_send = &PappLoader::svc_net_tcp_send;
  services->net_tcp_recv = &PappLoader::svc_net_tcp_recv;
  services->net_poll = &PappLoader::svc_net_poll;
  services->net_resolve = &PappLoader::svc_net_resolve;
  services->display_get_size = &PappLoader::svc_display_get_size;
  services->display_set_canvas = &PappLoader::svc_display_set_canvas;
  services->midi_read = &PappLoader::svc_midi_read;
  services->midi_write = &PappLoader::svc_midi_write;
  services->touch_read_points = &PappLoader::svc_touch_read_points;
  if (s_app_tls_lock == nullptr)
    s_app_tls_lock = xSemaphoreCreateMutex();
  services->net_tls_connect = &PappLoader::svc_net_tls_connect;
  services->net_tls_status = &PappLoader::svc_net_tls_status;
  services->net_tls_send = &PappLoader::svc_net_tls_send;
  services->net_tls_recv = &PappLoader::svc_net_tls_recv;
  services->net_tls_close = &PappLoader::svc_net_tls_close;
  services->app_open = &PappLoader::svc_app_open;
  services->app_get_arg = &PappLoader::svc_app_get_arg;
  services->app_set_resume_arg = &PappLoader::svc_app_set_resume_arg;
  services->file_list_dir = &PappLoader::svc_file_list_dir;
  services->file_mkdir = &PappLoader::svc_file_mkdir;
  services->file_remove = &PappLoader::svc_file_remove;
  services->file_rename = &PappLoader::svc_file_rename;
  services->file_stat = &PappLoader::svc_file_stat;
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

esp_err_t fetch_http_text(const char *url, std::string *out, size_t max_bytes) {
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
  const int status_code = esp_http_client_get_status_code(client);
  if (status_code == 404) {
    // Callers decide whether a missing file is an error (the catalog) or
    // normal (an app without a data list).
    ESP_LOGD(TAG, "HTTP 404: %s", url);
    return http_finish(client, ESP_ERR_NOT_FOUND);
  }
  if (status_code != 200) {
    ESP_LOGE(TAG, "HTTP status %d: %s", status_code, url);
    return http_finish(client, ESP_ERR_INVALID_RESPONSE);
  }
  if (content_length > static_cast<int64_t>(max_bytes)) {
    ESP_LOGE(TAG, "%s is too large: %lld bytes", url, static_cast<long long>(content_length));
    return http_finish(client, ESP_ERR_INVALID_SIZE);
  }

  char buffer[2048];
  uint32_t idle_reads = 0;
  while (true) {
    const int result = esp_http_client_read(client, buffer, sizeof(buffer));
    if (result > 0) {
      if (out->size() + static_cast<size_t>(result) > max_bytes)
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

// ── Launch progress and app data (data_root) ───────────────────────────────

void PappLoader::set_progress_(bool active, uint32_t done, uint32_t total, const char *format, ...) {
  char status[PROGRESS_STATUS_SIZE];
  va_list args;
  va_start(args, format);
  std::vsnprintf(status, sizeof(status), format, args);
  va_end(args);
  portENTER_CRITICAL(&this->progress_lock_);
  std::memcpy(this->progress_status_, status, sizeof(status));
  this->progress_active_ = active;
  this->progress_done_ = done;
  this->progress_total_ = total;
  this->progress_seq_ = this->progress_seq_ + 1;
  portEXIT_CRITICAL(&this->progress_lock_);
}

float PappLoader::get_load_progress() {
  portENTER_CRITICAL(&this->progress_lock_);
  const bool active = this->progress_active_;
  const uint32_t done = this->progress_done_;
  const uint32_t total = this->progress_total_;
  portEXIT_CRITICAL(&this->progress_lock_);
  if (!active || total == 0)
    return -1.0f;
  return static_cast<float>(static_cast<double>(done) / static_cast<double>(total));
}

std::string PappLoader::get_load_status() {
  char status[PROGRESS_STATUS_SIZE];
  portENTER_CRITICAL(&this->progress_lock_);
  std::memcpy(status, this->progress_status_, sizeof(status));
  portEXIT_CRITICAL(&this->progress_lock_);
  status[sizeof(status) - 1] = '\0';
  return status;
}

#ifdef PAPP_LOADER_USE_LVGL
// Plain objects and a label, like the page widgets, so this works whichever
// LVGL widgets the config enables.
void PappLoader::ensure_progress_overlay_() {
  if (this->overlay_panel_ != nullptr)
    return;
  lv_obj_t *panel = lv_obj_create(lv_layer_top());
  lv_obj_set_size(panel, 560, 84);
  lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -28);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x0B1626), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x38BDF8), 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_radius(panel, 14, 0);
  lv_obj_set_style_pad_all(panel, 14, 0);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);  // never blocks touches on the page
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *label = lv_label_create(panel);
  lv_obj_set_width(label, lv_pct(100));
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(label, lv_color_hex(0xE2E8F0), 0);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *track = lv_obj_create(panel);
  lv_obj_set_size(track, lv_pct(100), 10);
  lv_obj_align(track, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_pad_all(track, 0, 0);
  lv_obj_set_style_border_width(track, 0, 0);
  lv_obj_set_style_radius(track, 5, 0);
  lv_obj_set_style_bg_color(track, lv_color_hex(0x1E293B), 0);
  lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *fill = lv_obj_create(track);
  lv_obj_set_size(fill, 0, lv_pct(100));
  lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_border_width(fill, 0, 0);
  lv_obj_set_style_radius(fill, 5, 0);
  lv_obj_set_style_bg_color(fill, lv_color_hex(0x38BDF8), 0);
  lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);

  this->overlay_panel_ = panel;
  this->overlay_label_ = label;
  this->overlay_fill_ = fill;
}
#endif

void PappLoader::update_progress_ui_() {
#ifdef PAPP_LOADER_USE_LVGL
  if (this->lvgl_ == nullptr || !this->lvgl_->is_loop_started() || this->lvgl_->is_paused())
    return;
  // Redraw when the progress changed, or when another page came on screen
  // (whether the page's own widgets or the loader's panel shows it).
  lv_obj_t *screen = lv_screen_active();
  const uint32_t seq = this->progress_seq_;
  if (seq == this->progress_ui_seq_ && screen == this->progress_ui_screen_)
    return;
  this->progress_ui_seq_ = seq;
  this->progress_ui_screen_ = screen;

  char status[PROGRESS_STATUS_SIZE];
  portENTER_CRITICAL(&this->progress_lock_);
  std::memcpy(status, this->progress_status_, sizeof(status));
  const bool active = this->progress_active_;
  const uint32_t done = this->progress_done_;
  const uint32_t total = this->progress_total_;
  portEXIT_CRITICAL(&this->progress_lock_);
  status[sizeof(status) - 1] = '\0';

  // The page's widgets when they are on screen (and no store detail page covers
  // them), otherwise the loader's panel.
  lv_obj_t *page_widget = this->progress_fill_ != nullptr ? this->progress_fill_ : this->progress_label_;
  const bool page_shows =
      page_widget != nullptr && lv_obj_get_screen(page_widget) == screen && this->detail_panel_ == nullptr;
  if (!page_shows) {
    const bool show = active || status[0] != '\0';
    if (!show && this->overlay_panel_ == nullptr)
      return;
    this->ensure_progress_overlay_();
    if (!show) {
      lv_obj_add_flag(this->overlay_panel_, LV_OBJ_FLAG_HIDDEN);
      return;
    }
    const int32_t percent =
        active && total > 0 ? static_cast<int32_t>(std::min<uint64_t>(100, static_cast<uint64_t>(done) * 100 / total)) : 0;
    lv_obj_set_width(this->overlay_fill_, lv_pct(percent));
    lv_obj_set_style_bg_opa(lv_obj_get_parent(this->overlay_fill_), active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_label_set_text(this->overlay_label_, status[0] != '\0' ? status : "Loading...");
    lv_obj_remove_flag(this->overlay_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(this->overlay_panel_);  // above a store detail page
    if (active) {
      this->cancel_timeout("papp_progress_overlay");
    } else {
      // A failure message floats over every page, so let it go after a while.
      this->set_timeout("papp_progress_overlay", 8000, [this]() {
        if (this->overlay_panel_ != nullptr)
          lv_obj_add_flag(this->overlay_panel_, LV_OBJ_FLAG_HIDDEN);
      });
    }
    return;
  }
  if (this->overlay_panel_ != nullptr)
    lv_obj_add_flag(this->overlay_panel_, LV_OBJ_FLAG_HIDDEN);

  if (this->progress_fill_ != nullptr) {
    lv_obj_t *track = lv_obj_get_parent(this->progress_fill_);
    lv_obj_t *shown = track != nullptr ? track : this->progress_fill_;
    if (active && total > 0) {
      const int32_t percent = static_cast<int32_t>(std::min<uint64_t>(100, static_cast<uint64_t>(done) * 100 / total));
      lv_obj_set_width(this->progress_fill_, lv_pct(percent));
      lv_obj_remove_flag(shown, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(shown, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (this->progress_label_ != nullptr) {
    if (status[0] != '\0') {
      lv_label_set_text(this->progress_label_, status);
      lv_obj_remove_flag(this->progress_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(this->progress_label_, LV_OBJ_FLAG_HIDDEN);
    }
  }
#endif
}

// Creates the folders between `root` and the file `target` (root itself must
// already exist: it is the card's mount point or a folder on it).
bool make_parent_dirs(const std::string &root, const std::string &target) {
  size_t slash = target.find('/');
  while (slash != std::string::npos) {
    const std::string dir = root + "/" + target.substr(0, slash);
    struct stat st{};
    if (stat(dir.c_str(), &st) != 0) {
      if (mkdir(dir.c_str(), 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Cannot create %s (errno %d)", dir.c_str(), errno);
        return false;
      }
    } else if (!S_ISDIR(st.st_mode)) {
      ESP_LOGE(TAG, "%s exists and is not a folder", dir.c_str());
      return false;
    }
    slash = target.find('/', slash + 1);
  }
  return true;
}

// Returns where `target` (relative, e.g. roms/doom/doom1.wad) already exists:
// under data_root first, then each data_search root; "" when nowhere.
std::vector<std::string> PappLoader::data_roots_() const {
  std::vector<std::string> roots{this->data_root_};
  for (const auto &root : this->data_search_) {
    if (std::find(roots.begin(), roots.end(), root) == roots.end())
      roots.push_back(root);
  }
  return roots;
}

std::string PappLoader::find_data_file_(const std::string &target) const {
  for (const auto &root : this->data_roots_()) {
    const std::string path = runtime_path(root.c_str()) + "/" + target;
    struct stat st{};
    if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
      return path;
  }
  return {};
}

esp_err_t PappLoader::sync_app_data_(const std::string &papp_url, const std::string &app) {
  if (!this->download_data_)
    return ESP_OK;
  const std::string app_name = app.empty() ? canvas::app_key(papp_url) : app;
  const std::string list_url = data::list_url_for(papp_url);
  if (list_url.empty())
    return ESP_OK;

  this->set_progress_(true, 0, 0, "%s", "Checking app data...");
  std::string text;
  esp_err_t err = fetch_http_text(list_url.c_str(), &text);
  if (err == ESP_ERR_NOT_FOUND) {
    ESP_LOGD(TAG, "No data list for this app: %s", list_url.c_str());
    return ESP_OK;
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not read the data list %s: %s", list_url.c_str(), esp_err_to_name(err));
    this->set_progress_(false, 0, 0, "Could not read the app's data list: %s", esp_err_to_name(err));
    return err;
  }
  std::vector<data::DataFile> files;
  std::string error;
  if (!data::parse_list(text, &files, &error)) {
    ESP_LOGE(TAG, "Bad data list %s: %s", list_url.c_str(), error.c_str());
    this->set_progress_(false, 0, 0, "Bad data list: %.60s", error.c_str());
    return ESP_ERR_INVALID_RESPONSE;
  }

  // A file that is already on any storage root (data_root, data_search) is
  // kept whatever its size: it may be the
  // user's own copy (for example a full game instead of the shareware one).
  // Downloads go to <file>.part and are renamed only once verified, so an
  // interrupted download never leaves a partial file under the real name.
  const std::string root = runtime_path(this->data_root_.c_str());
  std::vector<size_t> missing;
  uint64_t total = 0;
  for (size_t i = 0; i < files.size(); i++) {
    const std::string found = this->find_data_file_(files[i].target);
    if (!found.empty()) {
      ESP_LOGI(TAG, "Have %s at %s; not downloading", files[i].target.c_str(), found.c_str());
      continue;
    }
    missing.push_back(i);
    total += files[i].size;
  }
  if (missing.empty()) {
    ESP_LOGI(TAG, "App data present: all %u file(s) found", static_cast<unsigned>(files.size()));
    return ESP_OK;
  }
  if (total > UINT32_MAX) {
    this->set_progress_(false, 0, 0, "%s", "App data is too large");
    return ESP_ERR_INVALID_SIZE;
  }
  ESP_LOGI(TAG, "Downloading %u of %u app data file(s), %lu bytes, into %s", static_cast<unsigned>(missing.size()),
           static_cast<unsigned>(files.size()), static_cast<unsigned long>(total), root.c_str());

  uint32_t done = 0;
  for (size_t n = 0; n < missing.size(); n++) {
    const data::DataFile &file = files[missing[n]];
    if (!make_parent_dirs(root, file.target)) {
      this->set_progress_(false, 0, 0, "Cannot write to %.40s - is the card in?", this->data_root_.c_str());
      return ESP_ERR_NOT_FOUND;
    }
    err = this->download_data_file_(file, root + "/" + file.target, done, static_cast<uint32_t>(total), n + 1,
                                    missing.size());
    if (err != ESP_OK) {
      if (err == ESP_ERR_INVALID_STATE) {
        this->set_progress_(false, 0, 0, "%s", "Download cancelled");
      } else {
        this->set_progress_(false, 0, 0, "Download failed: %.48s (%s)", file.target.c_str(), esp_err_to_name(err));
      }
      return err;
    }
    done += file.size;
    // Remembered for Uninstall, which deletes only what the store put here.
    this->record_download_(app_name, this->data_root_ + "/" + file.target, file.size, file.sha256);
  }
  ESP_LOGI(TAG, "App data ready under %s", root.c_str());
  return ESP_OK;
}

esp_err_t PappLoader::download_data_file_(const data::DataFile &file, const std::string &path, uint32_t done_before,
                                          uint32_t total, size_t index, size_t count) {
  ESP_LOGI(TAG, "Downloading %s (%lu bytes) from %s", path.c_str(), static_cast<unsigned long>(file.size),
           file.url.c_str());
  esp_http_client_config_t config{};
  config.url = file.url.c_str();
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.buffer_size = HTTP_READ_BUFFER_SIZE;
  config.buffer_size_tx = 4096;
  config.disable_auto_redirect = false;
  config.max_redirection_count = HTTP_MAX_REDIRECTIONS;
  config.keep_alive_enable = false;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (file.url.rfind("https://", 0) == 0)
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
    ESP_LOGE(TAG, "HTTP status %d: %s", status_code, file.url.c_str());
    return http_finish(client, ESP_ERR_INVALID_RESPONSE);
  }
  if (content_length > 0 && static_cast<uint64_t>(content_length) != file.size) {
    ESP_LOGE(TAG, "%s is %lld bytes, the list says %lu", file.url.c_str(), static_cast<long long>(content_length),
             static_cast<unsigned long>(file.size));
    return http_finish(client, ESP_ERR_INVALID_SIZE);
  }

  const std::string part = path + ".part";
  FILE *out = std::fopen(part.c_str(), "wb");
  if (out == nullptr) {
    ESP_LOGE(TAG, "Cannot create %s (errno %d)", part.c_str(), errno);
    return http_finish(client, ESP_ERR_NOT_FOUND);
  }
  auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(HTTP_READ_BUFFER_SIZE, MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    std::fclose(out);
    std::remove(part.c_str());
    return http_finish(client, ESP_ERR_NO_MEM);
  }

  const char *slash = std::strrchr(file.target.c_str(), '/');
  const char *name = slash != nullptr ? slash + 1 : file.target.c_str();
  data::Sha256 hash;
  uint32_t received = 0;
  while (received < file.size) {
    if (this->global_close_requested_) {
      err = ESP_ERR_INVALID_STATE;
      break;
    }
    const size_t chunk = std::min<size_t>(HTTP_READ_BUFFER_SIZE, file.size - received);
    err = http_read_exact(client, buffer, chunk);
    if (err != ESP_OK)
      break;
    if (std::fwrite(buffer, 1, chunk, out) != chunk) {
      ESP_LOGE(TAG, "Write to %s failed (errno %d); is the card full?", part.c_str(), errno);
      err = ESP_FAIL;
      break;
    }
    hash.update(buffer, chunk);
    received += static_cast<uint32_t>(chunk);
    const uint32_t overall = done_before + received;
    this->set_progress_(true, overall, total, "%u/%u %.40s  %.1f / %.1f MB", static_cast<unsigned>(index),
                        static_cast<unsigned>(count), name, overall / 1048576.0, total / 1048576.0);
  }
  heap_caps_free(buffer);
  if (std::fclose(out) != 0 && err == ESP_OK)
    err = ESP_FAIL;
  http_finish(client, ESP_OK);

  if (err == ESP_OK) {
    const std::string digest = hash.hex();
    if (digest != file.sha256) {
      ESP_LOGE(TAG, "%s: sha256 %s, expected %s", path.c_str(), digest.c_str(), file.sha256.c_str());
      err = ESP_ERR_INVALID_CRC;
    }
  }
  if (err == ESP_OK && std::rename(part.c_str(), path.c_str()) != 0) {
    ESP_LOGE(TAG, "Cannot rename %s (errno %d)", part.c_str(), errno);
    err = ESP_FAIL;
  }
  if (err != ESP_OK) {
    std::remove(part.c_str());
    return err;
  }
  ESP_LOGI(TAG, "Saved %s (sha256 ok)", path.c_str());
  return ESP_OK;
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

  const char *slash = std::strrchr(url, '/');
  const char *file_name = slash != nullptr ? slash + 1 : url;
  size_t received = 0;
  while (received < load_size) {
    const size_t chunk = std::min(HTTP_READ_BUFFER_SIZE, load_size - received);
    err = http_read_exact(client, transfer_buffer, chunk);
    if (err != ESP_OK)
      break;
    std::memcpy(static_cast<uint8_t *>(app->code_buf) + received, transfer_buffer, chunk);
    received += chunk;
    if (PappLoader::active() != nullptr) {
      PappLoader::active()->set_progress_(true, static_cast<uint32_t>(received), static_cast<uint32_t>(load_size),
                                          "Loading %.60s  %u / %u KB", file_name,
                                          static_cast<unsigned>(received / 1024),
                                          static_cast<unsigned>(load_size / 1024));
    }
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

// The PAPP writes its .data and .bss through exec_ptr, so the data cache can
// still hold dirty lines for that alias when the app returns. esp_mmu_unmap()
// removes the MMU entries without writing them back. A later eviction then
// targets an address with no mapping, and the PSRAM controller aborts the
// system ("MSPI PSRAM error"); Red Alert's large globals hit this on every
// close. Write the alias back and drop its lines before unmapping.
static void unmap_exec_alias(psram_app_handle_t handle) {
  const esp_err_t err = esp_cache_msync(handle->exec_ptr, handle->code_alloc,
                                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                                            ESP_CACHE_MSYNC_FLAG_INVALIDATE);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "PAPP cache writeback before unmap failed: %s", esp_err_to_name(err));
  esp_mmu_unmap(handle->exec_ptr);
  handle->exec_ptr = nullptr;
  handle->mapped = false;
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

  // Zeroed room after the table: an app built against a newer psram_app.h
  // reads NULL for services appended after this loader, not stack garbage.
  struct {
    app_services_t services;
    void *appended_later[PAPP_SERVICES_SPARE_SLOTS];
  } table{};
  PappLoader::populate_services(&table.services);
  auto entry = reinterpret_cast<papp_entry_fn_t>(static_cast<uint8_t *>(handle->exec_ptr) + handle->header.entry_off);
  ESP_LOGI(TAG, "Calling PAPP entry point at %p", reinterpret_cast<void *>(entry));
  const int result = entry(&table.services);

  unmap_exec_alias(handle);
  return result;
}

void psram_app_unload(psram_app_handle_t handle) {
  if (handle == nullptr)
    return;
  if (handle->mapped && handle->exec_ptr != nullptr)
    unmap_exec_alias(handle);
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
