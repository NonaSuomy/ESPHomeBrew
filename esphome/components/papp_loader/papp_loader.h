#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_timer.h"
#include "esphome/core/hal.h"
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
#ifdef PAPP_LOADER_USE_USB_MIDI
#include "esphome/components/usb_midi/usb_midi.h"
#endif

#include "papp_canvas.h"
#include "papp_data.h"
#include "papp_files.h"
#include "papp_handoff.h"
#include "psram_app.h"

namespace esphome {
namespace papp_loader {

// A button in a library source's side menu (YAML catalogs: actions:).
class CatalogActionTrigger : public Trigger<> {};

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
  // ESPHOMEBREW store view (YAML `library_style: grid`): the library shows
  // icon tiles instead of a list, and tapping an app opens its detail page
  // (about, controls, sizes, Stream / Install / Launch / Update). App info
  // comes from the <app>.json next to each .papp (see docs/building.md).
  void set_store_ui(bool enabled) { this->store_ui_ = enabled; }
  // Where Install puts apps (as <name>.papp plus its <name>.json).
  void set_install_dir(const std::string &dir) { this->install_dir_ = dir; }
  // The store view's side menu, which slides out from the right edge: Refresh
  // plus these buttons for the source on screen (YAML `actions:` of a catalog).
  void add_catalog_action(size_t catalog, const std::string &label, Trigger<> *trigger) {
    if (catalog < this->catalogs_.size())
      this->catalogs_[catalog].actions.emplace_back(label, trigger);
  }
  void set_side_menu(bool open);
  void toggle_side_menu() { this->set_side_menu(!this->side_menu_open_); }
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
    this->chain_launch_ = false;  // no argument; an app hand-off chain is forgotten
    this->launch_pending_ = true;
    ESP_LOGI("papp_loader", "Launch requested from menu: %s", path.c_str());
  }
  void request_launch_url(const std::string &url) { this->request_launch(url); }
  // Select a local emulator PAPP and a ROM/game path from ESPHome's native API
  // or a template text entity. The ROM path must be under /sd or /usb0; the
  // loader stores it in the shared PAPP setting before starting the emulator.
  void request_launch_rom(const std::string &emulator, const std::string &rom_path);
  // Download a PAPP into install_dir without requiring a store listing. The
  // caller supplies the expected size and SHA-256 so a local HTTP helper can
  // safely stage a build onto the SD card before launching it from Storage.
  void request_install_url(const std::string &url, const std::string &name, uint32_t size,
                           const std::string &sha256);
  // Download any regular file into a validated path under /sd. The transfer is
  // staged as <path>.part and only becomes visible after the size and SHA-256
  // have been verified. Refused while an app or another storage operation runs.
  void request_upload_url(const std::string &url, const std::string &path, uint32_t size,
                          const std::string &sha256);
  void request_close() {
    if (!this->launched_) {
      ESP_LOGW("papp_loader", "Ignoring PAPP close request because no app is running");
      return;
    }
    this->begin_close_();
    ESP_LOGI("papp_loader", "Close requested remotely");
  }
  void request_screen_stream() {
    this->stream_enabled_ = true;
    ESP_LOGI("papp_loader", "Screen stream enabled remotely");
  }
  // One full capture of the running PAPP's canvas (800x480 unless the app
  // chose another size), sent to the next (or current) client of the
  // diagnostic stream on TCP port 3233 as a PAPPSS01 packet. With no app
  // running it is the menu (or an empty 0x0 packet).
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
  // ── Canvas size (papp_canvas.h) ──
  // The panel's size (YAML panel_width/panel_height); by default the
  // display's own. Frame buffers are allocated for a canvas this large.
  void set_panel_size(int width, int height) {
    this->panel_config_w_ = width;
    this->panel_config_h_ = height;
  }
  // The canvas offered to apps that ask for one (display_get_size) and have no
  // per-app setting (YAML canvas_width/canvas_height); by default the panel.
  // Apps that never ask keep 800x480.
  void set_default_canvas(int width, int height) {
    this->default_canvas_w_ = width;
    this->default_canvas_h_ = height;
  }
  int get_panel_width() const { return this->geometry_.panel_w; }
  int get_panel_height() const { return this->geometry_.panel_h; }
  int get_default_canvas_width() const { return this->default_canvas_w_; }
  int get_default_canvas_height() const { return this->default_canvas_h_; }
  // The per-app Screen setting ("1024x600", or "" for the device default), kept
  // in <install_dir>/settings.json by app name (canvas::app_key: the .papp's
  // file name without folders, ".papp" and a "-<version>" suffix). set returns
  // false for a size this device cannot show, or when the file can't be written.
  std::string get_app_canvas(const std::string &app);
  bool set_app_canvas(const std::string &app, const std::string &size);
  // The sizes the Screen setting offers after "Default" (canvas::choices): the
  // app's listed sizes that fit, or the panel, 1024x600, 800x480 and 640x480.
  std::vector<std::string> canvas_choices(const std::vector<std::string> &listed = {}) const {
    return canvas::choices(this->max_canvas_w_, this->max_canvas_h_, listed);
  }
  void set_display(display::Display *display) { this->display_ = display; }
  void set_touchscreen(touchscreen::Touchscreen *touchscreen) { this->touchscreen_ = touchscreen; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  // Master output volume shared by every PAPP. The value is a percentage and
  // is applied immediately when an app is playing and again when its audio
  // stream is initialized.
  void set_master_volume(int32_t level);
  int32_t get_master_volume() const { return this->master_volume_; }
#ifdef PAPP_LOADER_USE_LVGL
  void set_lvgl(lvgl::LvglComponent *lvgl) { this->lvgl_ = lvgl; }
  void set_catalog_container(lv_obj_t *container) {
    this->catalog_container_ = container;
    this->favorites_page_active_ = false;
    this->catalog_ui_pending_ = true;
  }
  // The favorites page uses the same store grid and detail actions, but filters
  // the current catalog down to entries the user marked with the star action.
  void set_favorites_container(lv_obj_t *container) {
    this->catalog_container_ = container;
    this->favorites_page_active_ = true;
    this->catalog_ui_pending_ = true;
  }
  // Used by the YAML startup choice: favorites first, Store when none exist.
  bool has_favorites();
  // Optional ROM picker for emulator pages built in YAML. The picker scans
  // every configured data root (for example /sd and /usb0), and launching a
  // row sets the shared ROM-path service before starting the named PAPP.
  void set_rom_selector_container(lv_obj_t *container) { this->rom_selector_container_ = container; }
  void configure_rom_selector(const std::string &app, const std::string &folder,
                              const std::string &extensions) {
    this->rom_selector_app_ = app;
    this->rom_selector_folder_ = folder;
    this->rom_selector_extensions_ = extensions;
    this->rom_selector_source_.clear();
    this->rom_selector_sidecar_ = "/sd/roms/papp/" + app + ".rom";
  }
  // Open the shared ROM picker for a cartridge-style emulator PAPP.  The
  // mapping mirrors the standalone RetroESP32-P4 launcher: it derives the
  // system from the .papp name, scans /sd and /usb0, and filters each list to
  // that emulator's accepted ROM extensions.  Returns false for data-driven
  // ports (Doom, Quake, ScummVM, etc.) which should launch directly.
  bool open_rom_selector_for_app(const std::string &source);
  bool supports_rom_selector(const std::string &source) const;
  void refresh_rom_selector();
  void set_rom_selector_favorites_button(lv_obj_t *button) { this->rom_selector_favorites_button_ = button; }
  void toggle_rom_favorites();
  void toggle_selected_rom_favorite();
  void select_rom(const std::string &path);
  void handle_launcher_controls_();
  void set_catalog_selection_(uint16_t index);
  void ensure_catalog_volume_control_();
  void raise_catalog_volume_control_();
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
#ifdef PAPP_LOADER_USE_USB_MIDI
  void set_usb_midi(usb_midi::UsbMidi *usb_midi) { this->usb_midi_ = usb_midi; }
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
  static void svc_display_get_size(int *width, int *height);
  static int svc_display_set_canvas(int width, int height);
  static int svc_midi_read(uint8_t *buf, int len);
  static int svc_midi_write(const uint8_t *data, int len);
  static int svc_touch_read_points(papp_touch_point_t *points, int max);
  void snapshot_touches_();
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
  static int svc_net_tls_connect(const char *host, uint16_t port);
  static int svc_net_tls_status(int handle);
  static int svc_net_tls_send(int handle, const void *buf, int len);
  static int svc_net_tls_recv(int handle, void *buf, int len);
  static void svc_net_tls_close(int handle);
  static void close_app_tls_();
  // App hand-off (psram_app.h app_open / app_get_arg / app_set_resume_arg).
  static int svc_app_open(const char *target, const char *arg, int return_after);
  static int svc_app_get_arg(char *buf, int len);
  static int svc_app_set_resume_arg(const char *arg);
  static int svc_file_list_dir(const char *path, char *buf, int len);
  // File management (psram_app.h file_mkdir / file_remove / file_rename / file_stat).
  static int svc_file_mkdir(const char *path);
  static int svc_file_remove(const char *path);
  static int svc_file_rename(const char *from, const char *to);
  static int svc_file_stat(const char *path, papp_file_stat_t *out);
  static int svc_file_zip_extract_first(const char *archive, const char *extensions,
                                        const char *cache_dir, const char *cache_stem,
                                        char *out_path, size_t out_size);
  // The runtime (VFS) path for an app's path, or "" when files::clean_app_path
  // refuses it (outside /sd and the data roots, "..", ...). *root gets the
  // runtime path of the storage root it is on (/sdcard, /usb0).
  static std::string app_file_path_(const char *path, std::string *root = nullptr);
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
  static void papp_info_task_entry_(void *arg);
  static void papp_install_task_entry_(void *arg);
  // Downloads the app's missing data files. Each one downloaded is recorded
  // in the install manifest of `app` (default: the .papp's name), so Uninstall
  // can offer to delete it.
  esp_err_t sync_app_data_(const std::string &papp_url, const std::string &app = std::string());
  std::string find_data_file_(const std::string &target) const;
  // data_root, then each data_search root not already listed.
  std::vector<std::string> data_roots_() const;
  esp_err_t download_data_file_(const data::DataFile &file, const std::string &path, uint32_t done_before,
                                uint32_t total, size_t index, size_t count);
  void finish_app_();
  // ── App hand-off (papp_handoff.h) ──
  // The .papp for an app name: the installed copy, then each library source
  // (folder or HTTP catalog) in order; "" when none has it. Runs on the
  // calling app's task and may block while an HTTP catalog is fetched.
  std::string resolve_app_(const std::string &name);
  // The .papp files of a folder library source, under every data search root.
  std::vector<std::string> folder_papps_(const std::string &folder) const;
  // Where a folder library source is looked for: (root label, folder) pairs,
  // the folder under every data search root, the configured one first.
  std::vector<std::pair<std::string, std::string>> catalog_folders_(const std::string &folder) const;
  // After an app ends or a launch fails: queue the next app of the hand-off
  // chain (the one asked for, or the one to go back to), if any.
  void continue_chain_();
  handoff::Chain chain_;        // guarded by handoff_mutex_
  SemaphoreHandle_t handoff_mutex_{nullptr};
  std::string running_source_;  // the running app's .papp, as it was launched
  std::string launch_arg_;      // what app_get_arg gives the running app
  std::string resume_arg_;      // what it gets back after an app it opened (handoff_mutex_)
  std::string next_arg_;        // the argument for the queued launch
  bool chain_launch_{false};    // the queued launch comes from chain_
  void update_catalog_ui_();
  void update_rom_selector_ui_();
  void set_rom_selector_selection_(uint16_t index);
  bool rom_selector_page_active_() const;
  void update_progress_ui_();
  void flush_framebuffer_();
  // Sends a panel-oriented (turned 180 degrees) canvas-sized frame to the
  // display (plus the remote-view sample and the close button). The caller
  // holds display_mutex_.
  void send_display_buffer_(const uint16_t *display_buffer, int64_t start_us);
  // Canvas size. geometry_ is the panel and the running app's canvas; it only
  // changes under display_mutex_ (tasks that just hit-test touches read it
  // without the lock). The frame buffers hold max_canvas_w_ x max_canvas_h_
  // (the panel, unless that much PSRAM was not available).
  canvas::Geometry geometry_{};
  int max_canvas_w_{canvas::LEGACY_WIDTH};
  int max_canvas_h_{canvas::LEGACY_HEIGHT};
  size_t frame_alloc_bytes_{0};  // each of framebuffer_, rotated_framebuffer_, ppa_framebuffer_
  int panel_config_w_{0};  // YAML panel size; 0 = the display's
  int panel_config_h_{0};
  int default_canvas_w_{0};  // YAML default canvas; 0 = the panel
  int default_canvas_h_{0};
  // For the running app: the size display_get_size offers, and whether the
  // app has chosen a canvas (then display_get_size reports that one).
  int offered_canvas_w_{canvas::LEGACY_WIDTH};
  int offered_canvas_h_{canvas::LEGACY_HEIGHT};
  volatile bool canvas_chosen_{false};
  bool allocate_frame_buffers_(int width, int height);
  void free_frame_buffers_();
  int set_canvas_(int width, int height);
  // Switches the canvas to width x height and blanks the frame buffers (and,
  // with blank_panel, the whole panel). The caller holds display_mutex_.
  void apply_canvas_(int width, int height, bool blank_panel);
  // Back to 800x480 for the next app, and what display_get_size offers it.
  void prepare_canvas_for_app_(const std::string &source);
  void restore_legacy_canvas_();
  // The canvas size the app's listing recommends ("" for none): from the
  // store's listings, else the .json next to a .papp on the card.
  std::string listing_recommended_canvas_(const std::string &source) const;
  std::string settings_path_() const;
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
  void draw_volume_overlay_();
  void draw_volume_slider_overlay_();
  void clear_volume_overlay_();
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
  bool close_touch_(int x, int y);
  bool volume_touch_(int x, int y);
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
#ifdef PAPP_LOADER_USE_USB_MIDI
  usb_midi::UsbMidi *usb_midi_{nullptr};
#endif
  binary_sensor::BinarySensor *buttons_[BUTTON_COUNT]{};
  binary_sensor::BinarySensor *toggle_button_{nullptr};
  binary_sensor::BinarySensor *launch_button_{nullptr};
  binary_sensor::BinarySensor *fire_button_{nullptr};
  binary_sensor::BinarySensor *touch_button_{nullptr};
  // A capacitive pad can read ON for good (toggle mode latched, calibrated
  // while touched): after 10 s ON without a change it is ignored as A until it
  // turns OFF, so it cannot hold A down in apps or keep the launcher unarmed.
  int64_t touch_on_since_us_{0};
  bool touch_stuck_logged_{false};
  bool touch_button_stuck_() {
    const int64_t now = esp_timer_get_time();
    if (this->touch_on_since_us_ == 0)
      this->touch_on_since_us_ = now;
    const bool stuck = now - this->touch_on_since_us_ > 10000000;
    if (stuck && !this->touch_stuck_logged_) {
      ESP_LOGW("papp_loader", "Touch button ON for over 10 s: ignored as A until it is released");
      this->touch_stuck_logged_ = true;
    }
    return stuck;
  }
  void touch_button_released_() {
    this->touch_on_since_us_ = 0;
    this->touch_stuck_logged_ = false;
  }
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
  // The fingers on the panel, copied in loop() (the touchscreen's own list is
  // not safe to read from the app task); guarded by touch_points_lock_.
  static constexpr int MAX_TOUCH_POINTS = 5;
  touchscreen::TouchPoint touch_points_[MAX_TOUCH_POINTS]{};
  int touch_point_count_{0};
  portMUX_TYPE touch_points_lock_ = portMUX_INITIALIZER_UNLOCKED;
  // Set by the on-screen close control.  It is deliberately independent of
  // the normal PAPP X/action input so every loader-backed app gets the same
  // exit request, including apps that do not draw their own controls.
  volatile bool global_close_requested_{false};
  // When a touch on a close control drawn over the canvas started (us, 0 =
  // none): that one closes only when held (close_touch_).
  int64_t close_hold_since_us_{0};
  // Prevent one held touch from stepping the master volume every loop.
  bool volume_touch_active_{false};
  bool volume_slider_visible_{false};
  // When it was requested (esp_timer us): the buttons a close shows the app
  // follow a short sequence from then on (close_buttons_()).
  volatile int64_t close_requested_us_{0};
  void begin_close_();
  // The close buttons down right now: bit 0 Menu, bit 1 X, bit 2 L3.
  uint8_t close_buttons_() const;
  int audio_sample_rate_{0};
  int32_t master_volume_{100};
  // Heap diagnostics (log_heap in papp_loader.cpp): the running app's first
  // short audio write was logged; when loop() logs the heap next (esp_timer us).
  volatile bool audio_trouble_logged_{false};
  int64_t next_heap_log_us_{0};
  SemaphoreHandle_t display_mutex_{nullptr};
  SemaphoreHandle_t keyboard_mutex_{nullptr};
  portMUX_TYPE mouse_input_lock_ = portMUX_INITIALIZER_UNLOCKED;
  int32_t mouse_dx_accum_{0};
  int32_t mouse_dy_accum_{0};
  // Text reports are converted to press/release pairs. Keep enough room for
  // a short console command while Quake drains the queue on its worker task.
  // Until when (esp_timer us) an arrow that arrived only as text holds its
  // D-pad direction: up, right, down, left (the PAPP_INPUT_UP.. order).
  static constexpr int64_t ARROW_TAP_US = 150000;
  volatile int64_t arrow_tap_until_us_[4]{0, 0, 0, 0};
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
  // Warned once that LVGL woke on input while an app ran (loop()).
  bool lvgl_resume_warned_{false};
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
    std::vector<std::pair<std::string, Trigger<> *>> actions;
  };
  bool side_menu_open_{false};
  std::vector<CatalogSource> catalogs_;
  size_t catalog_index_{0};
  // The URL the running fetch is for: a switch mid-fetch refetches afterwards.
  std::string catalog_fetch_url_;

  // ── Store view (papp_store.cpp) ──
 public:
  // One app's listing, from its <app>.json (all fields optional).
  struct AppInfo {
    bool has_info{false};
    std::string name, title, version, author, category, license, about, changelog, upstream, source;
    std::vector<std::string> controls;
    // The listing's "canvas": true (any size) or the sizes the app can draw
    // ("1024x600", ...). Either one adds the Screen setting to its detail page.
    bool canvas_any{false};
    std::vector<std::string> canvas_sizes;
    bool supports_canvas() const { return this->canvas_any || !this->canvas_sizes.empty(); }
    // The size it recommends ("canvas_recommended", or "recommended" in an
    // object "canvas"): the Screen setting's default instead of the device's.
    std::string canvas_recommended;
    // What it needs on the card ("requires"), plus the data files the store
    // downloads for it (download = true). The detail page checks them.
    struct RequiredFile {
      std::string path;  // as the app sees it: /sd/roms/doom/doom1.wad, a folder/ or a pattern
      std::string note;
      bool optional{false};
      bool download{false};
    };
    std::vector<RequiredFile> required_files;
    uint32_t size{0}, data_size{0};
    std::string sha256;
    std::string sidecar;            // the raw JSON, saved next to an installed copy
    std::vector<uint8_t> icon_png;  // decoded from the listing's base64 icon
    std::shared_ptr<uint16_t> tile_icon;  // 112x112 RGB565 in PSRAM, decoded by the info task
  };

 protected:
  bool store_ui_{false};
  std::string install_dir_{"/sd/roms/papp"};
  // Info for catalog_entries_ (same order), fetched by a task after each listing.
  std::vector<AppInfo> app_info_;
  std::vector<AppInfo> info_result_;
  std::vector<std::string> info_urls_;
  uint32_t catalog_generation_{0};
  uint32_t info_generation_{0};
  TaskHandle_t info_task_handle_{nullptr};
  volatile bool info_loading_{false};
  volatile bool info_done_{false};
  // Store network work waits while a PAPP loads or runs: TLS takes internal
  // DMA-capable RAM that the SD card and I2S drivers need at app start.
  volatile bool info_interrupted_{false};  // the info task stopped early for an app
  bool info_deferred_{false};              // listings to fetch once the app ends
  bool catalog_deferred_{false};           // a catalog refresh asked for during an app
  void start_info_fetch_();
  void poll_info_fetch_();
  // name -> installed version, from <install_dir>/*.json.
  std::vector<std::pair<std::string, std::string>> installed_;
  // The same, as found by the info and install tasks; the loop takes it over.
  std::vector<std::pair<std::string, std::string>> info_installed_;
  std::vector<std::pair<std::string, std::string>> install_installed_;
  static std::vector<std::pair<std::string, std::string>> scan_installed_(const std::string &install_dir);
  std::string installed_version_(const std::string &name) const;
  // Install/Update of one app: the .papp (checked against its size and
  // sha256), its data, then its listing, in a task.
  int install_index_{-1};
  AppInfo install_info_;  // copies for the task: app_info_ can be replaced meanwhile
  std::string install_url_;
  bool direct_install_{false};  // API/tool upload, not a catalog Install/Update
  bool direct_file_upload_{false};  // API/tool upload of an arbitrary SD file
  std::string upload_path_;         // virtual path, validated under /sd
  TaskHandle_t install_task_handle_{nullptr};
  volatile bool install_done_{false};
  volatile esp_err_t install_result_{ESP_OK};
  void start_install_(int index);
  void poll_install_();
  // ── Install manifest and Uninstall (papp_store.cpp) ──
  // <install_dir>/<app>.installed.json lists every data file the store
  // downloaded for the app (path and size); Uninstall deletes only those.
  std::string manifest_path_(const std::string &app) const;
  void record_download_(const std::string &app, const std::string &path, uint32_t size, const std::string &sha256);
  // The manifest's files still on the card at their recorded size: what
  // Uninstall would delete (path, size).
  std::vector<std::pair<std::string, uint32_t>> downloaded_files_(const std::string &app) const;
  // Where a listing's required path is on the card (a data root's path to
  // it), or "" when it is on none of them.
  std::string find_required_(const std::string &path) const;

 public:
  // Removes an installed app: <install_dir>/<app>.papp and its listing, and
  // with delete_data the data files the store downloaded for it (listed in
  // its install manifest; files changed since, and everything else, stay).
  // Refused (false) while an app is loading or running or an install runs.
  bool uninstall_app(const std::string &app, bool delete_data);

 protected:
  void list_catalog_folder_();
  TaskHandle_t papp_catalog_task_handle_{nullptr};
  volatile bool catalog_loading_{false};
  volatile bool catalog_done_{false};
  volatile int catalog_result_{-1};
#ifdef PAPP_LOADER_USE_LVGL
  lv_obj_t *catalog_container_{nullptr};
  lv_obj_t *catalog_header_{nullptr};
  lv_obj_t *catalog_volume_button_{nullptr};
  lv_obj_t *catalog_volume_slider_{nullptr};
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
  bool launcher_b_state_{false};
  bool launcher_l_state_{false};
  bool launcher_r_state_{false};
  // Store view widgets.
  struct AppIcon {
    std::shared_ptr<uint16_t> pixels;
    lv_image_dsc_t dsc{};
  };
  std::vector<AppIcon> tile_icons_;  // per app, 112x112
  std::vector<lv_obj_t *> catalog_tiles_;
  // Source indices represented by the visible tile order. In the normal grid
  // this is 0..catalog_entries_.size()-1; on Favorites it is a filtered view.
  std::vector<size_t> visible_catalog_indices_;
  bool favorites_page_active_{false};
  lv_obj_t *rom_selector_container_{nullptr};
  lv_obj_t *rom_selector_favorites_button_{nullptr};
  std::vector<lv_obj_t *> rom_selector_buttons_;
  std::vector<std::string> rom_selector_paths_;
  std::string rom_selector_app_{"nes"};
  std::string rom_selector_folder_{"roms/nes"};
  std::string rom_selector_extensions_{".nes|.zip|"};
  // Exact local path or URL of the PAPP that owns the selected ROM.  This is
  // needed when the picker was opened from a streamed store entry.
  std::string rom_selector_source_;
  std::string rom_selector_sidecar_{"/sd/roms/papp/nes.rom"};
  bool rom_selector_favorites_only_{false};
  uint16_t rom_selector_selection_{0};
  uint16_t grid_columns_{1};
  lv_obj_t *detail_panel_{nullptr};
  AppIcon detail_icon_{};
  std::vector<lv_obj_t *> detail_buttons_;
  std::vector<uint8_t> detail_actions_;
  uint8_t detail_focus_{0};
  int detail_index_{-1};
  std::string detail_url_;  // reopened after the grid is rebuilt, if still listed
  // Side menu: a panel on the library page, parked off the right edge with its tab showing.
  lv_obj_t *drawer_{nullptr};
  std::vector<lv_obj_t *> drawer_buttons_;
  std::vector<int> drawer_actions_;  // -1 = Refresh, else an index into the source's actions
  uint8_t drawer_focus_{0};
  bool launcher_select_state_{false};
  void build_drawer_();
  void focus_drawer_button_(uint8_t index);
  void run_drawer_action_(int action);
  static void store_drawer_event_cb_(lv_event_t *event);
  static void store_tile_event_cb_(lv_event_t *event);
  static void store_button_event_cb_(lv_event_t *event);
  static void release_icon_(AppIcon *icon);
  static void set_icon_(AppIcon *icon, std::shared_ptr<uint16_t> pixels, uint16_t side);
  void free_icons_();
  void build_store_grid_();
  size_t store_source_index_(int visible_index) const;
  bool is_app_favorite_(const std::string &app) const;
  bool set_app_favorite_(const std::string &app, bool favorite);
  void toggle_app_favorite_(int index);
  bool is_rom_favorite_(const std::string &path) const;
  bool set_rom_favorite_(const std::string &path, bool favorite);
  void open_detail_(int index);
  void close_detail_();
  void focus_detail_button_(uint8_t index);
  void run_detail_action_(uint8_t action);
  // The detail page's Screen button: the app's settings key and label, and
  // one press (the next choice, saved).
  std::string detail_app_key_(int index) const;
  std::string screen_label_(int index);
  // The Screen button's choices for this app ("" = no saved setting) and the
  // recommended size that "" stands for ("" when there is none).
  std::vector<std::string> screen_options_(int index, std::string *recommended) const;
  void cycle_screen_setting_(int index);
  // Uninstall's confirmation, a dialog over the detail page: a title, a text,
  // a check box for the downloaded data (when there is any) and two buttons.
  lv_obj_t *dialog_{nullptr};
  std::vector<lv_obj_t *> dialog_items_;  // touch and d-pad targets, top to bottom
  std::vector<uint8_t> dialog_actions_;
  uint8_t dialog_focus_{0};
  bool dialog_delete_data_{false};
  std::string dialog_app_;  // the app it asks about
  void open_uninstall_dialog_(int index);
  void close_dialog_();
  void focus_dialog_item_(uint8_t index);
  void run_dialog_action_(uint8_t action);
  static void store_dialog_event_cb_(lv_event_t *event);
  bool handle_store_controls_(uint8_t newly_pressed, bool a_pressed, bool b_pressed, bool l_pressed, bool r_pressed,
                              bool select_pressed);
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

template<typename... Ts> class LaunchRomAction : public Action<Ts...> {
 public:
  explicit LaunchRomAction(PappLoader *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, emulator)
  TEMPLATABLE_VALUE(std::string, rom)

  void play(const Ts &...x) {
    this->parent_->request_launch_rom(this->emulator_.value(x...), this->rom_.value(x...));
  }

 protected:
  PappLoader *parent_;
};

}  // namespace papp_loader
}  // namespace esphome
