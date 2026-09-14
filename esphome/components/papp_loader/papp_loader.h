#pragma once

#include <string>
#include <utility>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/display/display.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/touchscreen/touchscreen.h"
#ifdef PAPP_LOADER_USE_LVGL
#include "esphome/components/lvgl/lvgl_esphome.h"
#endif
#ifdef PAPP_LOADER_USE_USB_HIDX
#include "esphome/components/usb_hidx/usb_hidx.h"
#endif

#include "papp_data.h"
#include "psram_app.h"

namespace esphome {
namespace papp_loader {

class PappLoader : public Component {
 public:
  static constexpr uint8_t BUTTON_COUNT = PAPP_INPUT_MAX;

  void set_path(const std::string &path) { this->path_ = path; }
  void set_catalog_url(const std::string &url) { this->catalog_url_ = url; }
  void refresh_catalog();
  // Named library sources (YAML `catalogs:`): an HTTP catalog page, or a folder
  // such as /sd/roms/papp/ whose .papp files are listed from every data
  // search root (/sd, /usb0, ...). With more than one, the library list starts
  // with a "◀ name ▶" row; tap it, or press left/right, to switch.
  void add_catalog(const std::string &name, const std::string &url) { this->catalogs_.push_back({name, url}); }
  // Show the catalog with this name (case-insensitive). Unknown names are ignored.
  void select_catalog(const std::string &name);
  void select_catalog_index(size_t index);
  // Step through the catalogs: +1 next, -1 previous (wraps around).
  void next_catalog(int step = 1);
  // The catalog shown first (YAML `default_catalog`); set before setup().
  void set_initial_catalog(size_t index) {
    if (index < this->catalogs_.size()) {
      this->catalog_index_ = index;
      this->catalog_url_ = this->catalogs_[index].url;
    }
  }
  // When set, every app run ends with a JSON test report POSTed here: the app,
  // how it ended, its return code or load error, runtime and the tail of its log.
  void set_report_url(const std::string &url) { this->report_url_ = url; }
  void set_report_log_bytes(size_t bytes) { this->report_log_bytes_ = bytes; }
  // App data: before a network launch the loader reads <app>.files next to the
  // .papp and downloads every listed file that is missing under data_root.
  // Files already on the card are never replaced.
  void set_data_root(const std::string &root) { this->data_root_ = root; }
  void set_download_data(bool download) { this->download_data_ = download; }
  // Storage roots (for example /sd, /usb0) where the user may already have an
  // app's files. A file found under any of them is not downloaded, and an app
  // reading /sd/<path> that is not on the card gets <root>/<path> instead.
  void add_data_search_root(const std::string &root) { this->data_search_.push_back(root); }
  // Launch progress for a UI: true while a network app (and its data) loads,
  // a fraction 0..1 (-1 when unknown or idle), and a one-line status that
  // stays after a failure until the next launch ("" when there is nothing to say).
  bool is_loading() const { return this->papp_loading_; }
  float get_load_progress();
  std::string get_load_status();
  // Called by the download code on the loader task; thread safe.
  void set_progress_(bool active, uint32_t done, uint32_t total, const char *format, ...)
      __attribute__((format(printf, 5, 6)));
  void request_launch(const std::string &path) {
    if (path.empty()) {
      ESP_LOGW("papp_loader", "Ignoring empty PAPP launch request");
      return;
    }
    if (this->launched_) {
      ESP_LOGW("papp_loader", "Ignoring PAPP launch while another app is running: %s", path.c_str());
      return;
    }
    if (this->launch_pending_) {
      ESP_LOGW("papp_loader", "Ignoring duplicate PAPP launch request: %s", path.c_str());
      return;
    }
    this->path_ = path;
    this->launch_pending_ = true;
    ESP_LOGI("papp_loader", "Launch requested from menu: %s", path.c_str());
  }
  void request_launch_url(const std::string &url) { this->request_launch(url); }
  void request_close() {
    if (!this->launched_) {
      ESP_LOGW("papp_loader", "Ignoring PAPP close request because no app is running");
      return;
    }
    this->global_close_requested_ = true;
    ESP_LOGI("papp_loader", "Close requested remotely");
  }
  void request_screen_stream() {
    this->stream_enabled_ = true;
    ESP_LOGI("papp_loader", "Screen stream enabled remotely");
  }
  // One full 800x480 capture of the running PAPP's canvas, sent to the next
  // (or current) client of the diagnostic stream on TCP port 3233 as a
  // PAPPSS01 packet. With no app running the packet is empty (0x0).
  void request_screenshot() {
    this->screenshot_requested_ = true;
    this->stream_enabled_ = true;
    ESP_LOGI("papp_loader", "Screenshot requested remotely");
  }
  // One file from the SD card (or, for a path ending in '/', a directory
  // listing), sent to the diagnostic stream as a PAPPFL01 packet. Only paths
  // under /sd/ are served (bridge `readfile`).
  void request_file(const std::string &path);
  // Replaces a small text file under /sd/ (bridge `writefile`), keeping the old
  // one as <path>.bak. Refused while an app runs, since the app may rewrite it.
  void write_file(const std::string &path, const std::string &data);
  // Removes one text or leftover file (.bak, .log) under /sd/ (bridge
  // `deletefile`). Never game data or apps; refused while an app runs.
  void delete_file(const std::string &path);
  void set_autostart(bool autostart) { this->autostart_ = autostart; }
  void set_display(display::Display *display) { this->display_ = display; }
  void set_touchscreen(touchscreen::Touchscreen *touchscreen) { this->touchscreen_ = touchscreen; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
#ifdef PAPP_LOADER_USE_LVGL
  void set_lvgl(lvgl::LvglComponent *lvgl) { this->lvgl_ = lvgl; }
  void set_catalog_container(lv_obj_t *container) {
    this->catalog_container_ = container;
    this->catalog_ui_pending_ = true;
  }
  void handle_launcher_controls_();
  void set_catalog_selection_(uint16_t index);
  // Optional progress widgets the loader keeps up to date while an app and
  // its data download: `fill` is an object inside a track object; its width is
  // set to the percentage done and the track (its parent) is hidden when idle.
  // `label` shows the status line. Either may be null. Plain objects and a
  // label keep this independent of which LVGL widgets the config enables.
  void set_progress_widgets(lv_obj_t *fill, lv_obj_t *label) {
    this->progress_fill_ = fill;
    this->progress_label_ = label;
    this->progress_ui_seq_ = this->progress_seq_ - 1;  // redraw on the next loop
  }
#endif
  void set_toggle_button(binary_sensor::BinarySensor *sensor) { this->toggle_button_ = sensor; }
  void set_launch_button(binary_sensor::BinarySensor *sensor) { this->launch_button_ = sensor; }
  void set_fire_button(binary_sensor::BinarySensor *sensor) { this->fire_button_ = sensor; }
  void set_touch_button(binary_sensor::BinarySensor *sensor) { this->touch_button_ = sensor; }
  void set_adc_button_sensor(sensor::Sensor *sensor) { this->adc_button_sensor_ = sensor; }
  void set_left_stick_x_sensor(sensor::Sensor *sensor) { this->left_stick_x_sensor_ = sensor; }
  void set_left_stick_y_sensor(sensor::Sensor *sensor) { this->left_stick_y_sensor_ = sensor; }
  void set_right_stick_x_sensor(sensor::Sensor *sensor) { this->right_stick_x_sensor_ = sensor; }
  void set_right_stick_y_sensor(sensor::Sensor *sensor) { this->right_stick_y_sensor_ = sensor; }
  // Called by the USB HID text sensor. Events are queued only while a PAPP is
  // running, so launcher/menu keyboard input cannot leak into an app.
  void enqueue_keyboard_text(const std::string &text);
  // Called by USB HIDX sensor callbacks. Mouse reports are accumulated here
  // because the PAPP worker and USB host run concurrently.
  void enqueue_mouse_delta(float dx, float dy);
#ifdef PAPP_LOADER_USE_USB_HIDX
  void set_usb_hidx(usb_hidx::USBHIDXComponent *usb_hidx) { this->usb_hidx_ = usb_hidx; }
#endif
  void set_button(uint8_t index, binary_sensor::BinarySensor *sensor) {
    if (index < BUTTON_COUNT)
      this->buttons_[index] = sensor;
  }

  float get_setup_priority() const override { return setup_priority::LATE; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  static PappLoader *active() { return active_; }

  // Static callbacks installed in the C ABI table used by the PAPP.
  static uint16_t *svc_display_get_framebuffer();
  static uint16_t *svc_display_get_emu_buffer();
  static void svc_display_flush();
  static void svc_display_emu_flush();
  static void svc_display_clear(uint16_t color);
  static void svc_display_set_scale(float sx, float sy);
  static void svc_display_write_frame_rgb565(const uint16_t *buffer);
  static void svc_display_write_frame_custom(const uint16_t *buffer, uint16_t in_w, uint16_t in_h,
                                             float scale, bool byte_swap);
  static void svc_display_write_rect(int x, int y, int w, int h, const uint16_t *data);
  static int svc_sprite_blit(uint16_t *framebuf, uint32_t fb_w, uint32_t fb_h,
                             uint32_t x, uint32_t y, const uint16_t *sprite,
                             uint32_t sp_w, uint32_t sp_h, uint16_t colorkey);
  static int svc_fb_copy(const uint16_t *src, uint16_t *dst, uint32_t w, uint32_t h);
  static uint16_t *svc_png_load_rgb565(const char *path,
                                       uint16_t *out_w, uint16_t *out_h);
  static void svc_display_lock();
  static void svc_display_unlock();
  static void svc_audio_init(int sample_rate);
  static void svc_audio_submit(short *stereo_buf, int frame_count);
  static void svc_input_gamepad_read(papp_gamepad_state_t *state);
  static int svc_input_l3_read();
  static int svc_input_mouse_read(int *dx, int *dy, int *buttons);
  static int svc_input_keyboard_read(papp_keyboard_event_t *event);
  static int svc_touch_read(int *x, int *y);
  static int svc_net_udp_open(uint16_t port, int broadcast);
  static int svc_net_udp_send(int handle, const void *buf, int len, uint32_t ip, uint16_t port);
  static int svc_net_udp_recv(int handle, void *buf, int len, uint32_t *ip, uint16_t *port);
  static void svc_net_udp_close(int handle);
  static int svc_net_ipv4(uint32_t *ip, uint32_t *netmask);
  static int svc_net_tcp_connect(uint32_t ip, uint16_t port);
  static int svc_net_tcp_listen(uint16_t port);
  static int svc_net_tcp_accept(int handle, uint32_t *ip, uint16_t *port);
  static int svc_net_tcp_send(int handle, const void *buf, int len);
  static int svc_net_tcp_recv(int handle, void *buf, int len);
  static int svc_net_poll(int handle);
  static int svc_net_resolve(const char *host, uint32_t *ip);
  static void close_app_sockets_();
  static void *svc_file_open(const char *path, const char *mode);
  static int svc_file_close(void *stream);
  static size_t svc_file_read(void *ptr, size_t size, size_t nmemb, void *stream);
  static size_t svc_file_write(const void *ptr, size_t size, size_t nmemb, void *stream);
  static int svc_file_seek(void *stream, long offset, int whence);
  static long svc_file_tell(void *stream);
  static void *svc_mem_caps_alloc(size_t size, uint32_t caps);
  static void *svc_mem_alloc(size_t size);
  static void *svc_mem_calloc(size_t n, size_t size);
  static void *svc_mem_realloc(void *ptr, size_t size);
  static void svc_mem_free(void *ptr);
  static int svc_log_printf(const char *fmt, ...);
  static int svc_log_vprintf(const char *fmt, va_list args);
  static void svc_delay_ms(int ms);
  static int64_t svc_get_time_us();
  static char *svc_settings_rom_path_get();
  static void svc_settings_rom_path_set(const char *path);
  static int32_t svc_settings_volume_get();
  static void svc_settings_volume_set(int32_t level);
  static int32_t svc_settings_brightness_get();
  static void svc_settings_brightness_set(int32_t level);
  static int svc_task_create(void (*fn)(void *), const char *name, uint32_t stack_depth, void *arg,
                             int priority, void *out_handle, int core);
  static void svc_task_delete(void *handle);
  static void populate_services(app_services_t *services);

 protected:
  bool launch_();
  bool start_loaded_app_(psram_app_handle_t app, const std::string &source);
  static void papp_task_entry_(void *arg);
  static void papp_load_task_entry_(void *arg);
  static void papp_catalog_task_entry_(void *arg);
  esp_err_t sync_app_data_(const std::string &papp_url);
  std::string find_data_file_(const std::string &target) const;
  esp_err_t download_data_file_(const data::DataFile &file, const std::string &path, uint32_t done_before,
                                uint32_t total, size_t index, size_t count);
  void finish_app_();
  void update_catalog_ui_();
  void update_progress_ui_();
  void flush_framebuffer_();
  // Sends a panel-oriented 800x480 frame to the display (plus the remote-view
  // sample and the close button). The caller holds display_mutex_.
  void send_display_buffer_(const uint16_t *display_buffer, int64_t start_us);
  void render_custom_(const uint16_t *buffer, uint16_t in_w, uint16_t in_h, float scale, bool byte_swap);
  void log_render_time_(int64_t render_start_us, uint16_t in_w, uint16_t in_h, float scale);
  // Where render_custom_ last put a frame in rotated_framebuffer_ (x, y, w, h):
  // the black border around it only needs clearing when that changes.
  uint16_t direct_frame_[4]{0, 0, 0, 0};
  // True when the newest frame is only in rotated_framebuffer_ (direct path),
  // not in framebuffer_; screenshots then read it from there.
  volatile bool last_frame_direct_{false};
  void render_emu_();
  void clear_(uint16_t color);
  void draw_close_overlay_();
  void clear_close_overlay_();
  void restore_lvgl_();
  void begin_report_(const std::string &source);
  void append_report_log_(const char *line);
  void send_report_(const char *outcome, int result, const std::string &source);
  static void report_task_entry_(void *arg);
  void read_input_(papp_gamepad_state_t *state);
  int read_mouse_(int *dx, int *dy, int *buttons);
  int read_keyboard_(papp_keyboard_event_t *event);
  void clear_keyboard_queue_();
  void clear_mouse_delta_();
  void enqueue_keyboard_event_(int key, bool down);
  void enqueue_keyboard_tap_(int key);
  void poll_close_button_();
  int read_touch_(int *x, int *y);
  void audio_init_(int sample_rate);
  void audio_submit_(short *stereo_buf, int frame_count);
  static void screen_stream_task_entry_(void *arg);
  void screen_stream_task_();
  bool send_screenshot_(int client_fd);
  bool send_file_(int client_fd);

  static PappLoader *active_;

  std::string path_;
  display::Display *display_{nullptr};
  touchscreen::Touchscreen *touchscreen_{nullptr};
  speaker::Speaker *speaker_{nullptr};
#ifdef PAPP_LOADER_USE_LVGL
  lvgl::LvglComponent *lvgl_{nullptr};
#endif
#ifdef PAPP_LOADER_USE_USB_HIDX
  usb_hidx::USBHIDXComponent *usb_hidx_{nullptr};
#endif
  binary_sensor::BinarySensor *buttons_[BUTTON_COUNT]{};
  binary_sensor::BinarySensor *toggle_button_{nullptr};
  binary_sensor::BinarySensor *launch_button_{nullptr};
  binary_sensor::BinarySensor *fire_button_{nullptr};
  binary_sensor::BinarySensor *touch_button_{nullptr};
  sensor::Sensor *adc_button_sensor_{nullptr};
  sensor::Sensor *left_stick_x_sensor_{nullptr};
  sensor::Sensor *left_stick_y_sensor_{nullptr};
  sensor::Sensor *right_stick_x_sensor_{nullptr};
  sensor::Sensor *right_stick_y_sensor_{nullptr};

  uint16_t *framebuffer_{nullptr};
  // PAPP renders in its logical landscape orientation.  The Elecrow panel is
  // mounted 180 degrees around, so direct PAPP flushes use this second PSRAM
  // buffer to avoid mutating the app-owned framebuffer in place.
  uint16_t *rotated_framebuffer_{nullptr};
  // Hardware SRM output used to rotate the PAPP canvas without a CPU pixel
  // copy.  The software buffer remains as a fallback if PPA is unavailable.
  void *ppa_srm_client_{nullptr};
  uint16_t *ppa_framebuffer_{nullptr};
  uint16_t *emu_buffer_{nullptr};
  // Optional diagnostic framebuffer published to the host over TCP. The
  // stream is inactive unless a recorder connects to port 3233.
  uint16_t *stream_framebuffer_{nullptr};
  // Network writes can block. These snapshots keep stream_mutex_ confined to
  // the short producer/consumer copy instead of the TCP transfer.
  uint16_t *stream_frame_packet_{nullptr};
  uint8_t *stream_audio_buffer_{nullptr};
  uint8_t *stream_audio_packet_{nullptr};
  size_t stream_audio_read_{0};
  size_t stream_audio_write_{0};
  size_t stream_audio_available_{0};
  SemaphoreHandle_t stream_mutex_{nullptr};
  volatile bool stream_frame_ready_{false};
  volatile bool stream_client_connected_{false};
  volatile bool stream_enabled_{false};
  volatile bool screenshot_requested_{false};
  // With no app running, a screenshot is the LVGL menu: the main loop (the
  // LVGL thread) snapshots the active screen into this RGB565 buffer and the
  // stream task sends it. Needs LV_USE_SNAPSHOT (e.g. -DLV_USE_SNAPSHOT=1).
  uint16_t *menu_shot_{nullptr};
  uint16_t menu_shot_w_{0};
  uint16_t menu_shot_h_{0};
  volatile bool menu_shot_wanted_{false};  // set by the stream task, taken by loop()
  volatile bool menu_shot_ready_{false};
  void take_menu_shot_();
  volatile bool file_requested_{false};
  char file_request_path_[128]{};
  uint32_t stream_frame_sequence_{0};
  TaskHandle_t stream_task_handle_{nullptr};
  float scale_x_{1.0f};
  float scale_y_{1.0f};
  bool autostart_{false};
  bool launch_pending_{false};
  bool launched_{false};
  bool toggle_button_state_{false};
  bool toggle_wait_release_{false};
  bool toggle_close_requested_{false};
  bool launch_button_state_{false};
  bool launch_wait_release_{false};
  bool touch_active_{false};
  // Set by the on-screen close control.  It is deliberately independent of
  // the normal PAPP X/action input so every loader-backed app gets the same
  // exit request, including apps that do not draw their own controls.
  volatile bool global_close_requested_{false};
  int audio_sample_rate_{0};
  SemaphoreHandle_t display_mutex_{nullptr};
  SemaphoreHandle_t keyboard_mutex_{nullptr};
  portMUX_TYPE mouse_input_lock_ = portMUX_INITIALIZER_UNLOCKED;
  int32_t mouse_dx_accum_{0};
  int32_t mouse_dy_accum_{0};
  // Text reports are converted to press/release pairs. Keep enough room for
  // a short console command while Quake drains the queue on its worker task.
  static constexpr size_t KEYBOARD_QUEUE_SIZE = 128;
  papp_keyboard_event_t keyboard_queue_[KEYBOARD_QUEUE_SIZE]{};
  size_t keyboard_head_{0};
  size_t keyboard_tail_{0};
  TaskHandle_t papp_task_handle_{nullptr};
  TaskHandle_t papp_load_task_handle_{nullptr};
  psram_app_handle_t app_handle_{nullptr};
  psram_app_handle_t papp_load_handle_{nullptr};
  std::string papp_load_source_;
  volatile bool papp_loading_{false};
  volatile bool papp_load_done_{false};
  volatile int papp_load_result_{-1};
  volatile bool papp_task_done_{false};
  volatile int papp_task_result_{-1};
  volatile bool papp_load_data_failed_{false};
  // App data (data_root) and launch progress. The loader task writes the
  // progress under progress_lock_; the loop task reads it for the UI.
  std::string data_root_{"/sd"};
  bool download_data_{true};
  std::vector<std::string> data_search_;
  static constexpr size_t PROGRESS_STATUS_SIZE = 96;
  portMUX_TYPE progress_lock_ = portMUX_INITIALIZER_UNLOCKED;
  char progress_status_[PROGRESS_STATUS_SIZE]{};
  uint32_t progress_done_{0};
  uint32_t progress_total_{0};
  bool progress_active_{false};
  volatile uint32_t progress_seq_{0};
  uint32_t progress_ui_seq_{0};
  // Test reports (report_url). report_log_ holds the newest app log lines, up to
  // report_log_bytes_; the PAPP worker appends and the loop task sends, so both
  // go through report_mutex_.
  std::string report_url_;
  size_t report_log_bytes_{4096};
  std::string report_log_;
  std::string report_source_;
  int64_t report_started_us_{0};
  SemaphoreHandle_t report_mutex_{nullptr};
  std::string catalog_url_;
  std::string catalog_html_;
  std::vector<std::pair<std::string, std::string>> catalog_entries_;
  struct CatalogSource {
    std::string name;
    std::string url;
  };
  std::vector<CatalogSource> catalogs_;
  size_t catalog_index_{0};
  // The URL the running fetch is for: a switch mid-fetch refetches afterwards.
  std::string catalog_fetch_url_;
  void list_catalog_folder_();
  TaskHandle_t papp_catalog_task_handle_{nullptr};
  volatile bool catalog_loading_{false};
  volatile bool catalog_done_{false};
  volatile int catalog_result_{-1};
#ifdef PAPP_LOADER_USE_LVGL
  lv_obj_t *catalog_container_{nullptr};
  // Transparent LVGL hit-test shield. Direct-rendered PAPPs do not create a
  // LVGL root object, so this prevents launcher widgets underneath them from
  // seeing a touch release while the PAPP is active.
  lv_obj_t *touch_modal_shield_{nullptr};
  bool catalog_ui_pending_{false};
  lv_obj_t *progress_fill_{nullptr};
  lv_obj_t *progress_label_{nullptr};
  // The loader's own progress panel on the top layer, used when the page on
  // screen has no progress widgets (e.g. a store source picked from another page).
  lv_obj_t *overlay_panel_{nullptr};
  lv_obj_t *overlay_fill_{nullptr};
  lv_obj_t *overlay_label_{nullptr};
  lv_obj_t *progress_ui_screen_{nullptr};
  void ensure_progress_overlay_();
  uint16_t catalog_selection_{0};
  // 1 when the list starts with the catalog switcher row, else 0.
  uint8_t catalog_header_rows_{0};
  uint8_t launcher_direction_state_{0};
  bool launcher_a_state_{false};
  bool launcher_touch_state_{false};
  bool launcher_input_armed_{false};
#endif
};

template<typename... Ts> class LaunchUrlAction : public Action<Ts...> {
 public:
  explicit LaunchUrlAction(PappLoader *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, url)

  // ESPHome's Action::play signature changed from by-value to const-reference
  // arguments between supported releases.  Keep this action compatible with
  // both forms; Action::play_complex still dispatches it normally.
  void play(const Ts &...x) { this->parent_->request_launch_url(this->url_.value(x...)); }

 protected:
  PappLoader *parent_;
};

}  // namespace papp_loader
}  // namespace esphome
