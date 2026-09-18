// ESPHOMEBREW store view: app listings (<app>.json next to each .papp), an
// icon grid with INSTALLED / UPDATE badges, a detail page and Install.
//
// Everything that touches LVGL runs on the ESPHome main loop (the LVGL
// thread); listings are fetched and apps installed in their own tasks, which
// hand results back through flags the loop polls.

#include "papp_loader.h"
#include "papp_internal.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esphome/components/json/json_util.h"
#include "esphome/core/log.h"
#include "mbedtls/base64.h"

#include "PNGdec.h"

namespace esphome {
namespace papp_loader {

static const char *const TAG = "papp_store";

// A listing with a 256x256 icon is about 90 KB of JSON.
static constexpr size_t INFO_MAX_BYTES = 192 * 1024;
static constexpr uint16_t TILE_W = 176;
static constexpr uint16_t TILE_H = 188;
static constexpr uint16_t TILE_GAP = 12;
static constexpr uint16_t TILE_ICON = 112;
static constexpr uint16_t DETAIL_ICON = 192;

static constexpr uint32_t COLOR_TILE = 0x111C33;
static constexpr uint32_t COLOR_TILE_BORDER = 0x1E2A44;
static constexpr uint32_t COLOR_ACCENT = 0x38BDF8;
static constexpr uint32_t COLOR_PAGE = 0x08111F;
static constexpr uint32_t COLOR_TEXT = 0xE2E8F0;
static constexpr uint32_t COLOR_MUTED = 0x94A3B8;
static constexpr uint32_t COLOR_INSTALLED = 0x22C55E;
static constexpr uint32_t COLOR_UPDATE = 0xF59E0B;
// Transparent icon pixels are blended onto the tile colour (PNGdec takes 0xBBGGRR).
static constexpr uint32_t ICON_BLEND_BGR = 0x331C11;

enum DetailAction : uint8_t {
  ACTION_STREAM,
  ACTION_INSTALL,
  ACTION_LAUNCH,
  ACTION_ROM,
  ACTION_SCREEN,
  ACTION_FAVORITE,
  ACTION_BACK,
  ACTION_UNINSTALL,
};
// The buttons of Uninstall's confirmation.
enum DialogAction : uint8_t { DIALOG_TOGGLE_DATA, DIALOG_CONFIRM, DIALOG_CANCEL };

// The listing next to a .papp: the same name with .json.
static std::string sidecar_for(const std::string &papp) {
  const std::string path = papp.substr(0, papp.find_first_of("?#"));
  if (path.size() < 6)
    return {};
  std::string ext = path.substr(path.size() - 5);
  for (char &c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".papp" ? path.substr(0, path.size() - 5) + ".json" : std::string();
}

// <install_dir>/<app>.installed.json: the data files the store downloaded.
static constexpr const char *MANIFEST_SUFFIX = ".installed.json";

static bool is_manifest_name(const std::string &file) {
  const size_t n = std::strlen(MANIFEST_SUFFIX);
  return file.size() > n && file.compare(file.size() - n, n, MANIFEST_SUFFIX) == 0;
}

static esp_err_t read_small_file(const std::string &path, std::string *out, size_t max_bytes) {
  FILE *file = std::fopen(runtime_path(path.c_str()).c_str(), "rb");
  if (file == nullptr)
    return ESP_ERR_NOT_FOUND;
  out->clear();
  char buffer[1024];
  size_t got;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    if (out->size() + got > max_bytes) {
      std::fclose(file);
      return ESP_ERR_INVALID_SIZE;
    }
    out->append(buffer, got);
  }
  std::fclose(file);
  return ESP_OK;
}

// A listing's canvas sizes: ["1024x600", "800x480"].
static void parse_canvas_sizes(JsonArray sizes, std::vector<std::string> *out) {
  for (JsonVariant entry : sizes) {
    const char *value = entry | "";
    int w = 0, h = 0;
    if (canvas::parse_size(value, &w, &h))
      out->emplace_back(canvas::format_size(w, h));
  }
}

static std::string parse_canvas_size(const char *value) {
  int w = 0, h = 0;
  return value != nullptr && canvas::parse_size(value, &w, &h) ? canvas::format_size(w, h) : std::string();
}

// icon: decode the base64 icon too (not needed to read a size at launch).
static bool parse_app_info(const std::string &text, PappLoader::AppInfo *info, bool icon = true) {
  return json::parse_json(text, [info, icon](JsonObject root) -> bool {
    info->name = root["name"] | "";
    info->title = root["title"] | "";
    info->version = root["version"] | "";
    info->author = root["author"] | "";
    info->category = root["category"] | "";
    info->license = root["license"] | "";
    info->about = root["about"] | (root["description"] | "");
    info->changelog = root["changelog"] | "";
    info->size = root["size"] | 0u;
    info->data_size = root["data_size"] | 0u;
    info->sha256 = root["sha256"] | "";
    JsonArray controls = root["controls"].as<JsonArray>();
    for (JsonVariant line : controls) {
      const char *value = line | "";
      if (*value != '\0')
        info->controls.emplace_back(value);
    }
    // "canvas": true, or the sizes the app draws: ["1024x600", "800x480"], or
    // {"sizes": [...], "recommended": "800x480"} (no sizes: any). The store
    // publishes the recommendation as "canvas_recommended", next to a plain
    // "canvas" that older loaders read.
    if (root["canvas"].is<bool>()) {
      info->canvas_any = root["canvas"].as<bool>();
    } else if (root["canvas"].is<JsonObject>()) {
      JsonArray sizes = root["canvas"]["sizes"].as<JsonArray>();  // named: no dangling-reference warning
      parse_canvas_sizes(sizes, &info->canvas_sizes);
      info->canvas_any = sizes.isNull();
      info->canvas_recommended = parse_canvas_size(root["canvas"]["recommended"] | "");
    } else {
      JsonArray sizes = root["canvas"].as<JsonArray>();
      parse_canvas_sizes(sizes, &info->canvas_sizes);
    }
    if (info->canvas_recommended.empty())
      info->canvas_recommended = parse_canvas_size(root["canvas_recommended"] | "");
    // "requires": what the app needs on the card. The data files the store
    // downloads count too (the store lists them there; a listing that only
    // has them under "data" gets them from there).
    JsonArray required = root["requires"].as<JsonArray>();
    for (JsonVariant entry : required) {
      PappLoader::AppInfo::RequiredFile file;
      file.path = entry["path"] | "";
      file.note = entry["note"] | "";
      file.optional = entry["optional"] | false;
      file.download = entry["download"] | false;
      if (files::parse_required(file.path, nullptr) && info->required_files.size() < 96)
        info->required_files.push_back(std::move(file));
    }
    JsonArray data_files = root["data"]["files"].as<JsonArray>();
    for (JsonVariant entry : data_files) {
      PappLoader::AppInfo::RequiredFile file;
      file.path = std::string("/sd/") + (entry["target"] | "");
      file.download = true;
      bool listed = false;
      for (const auto &have : info->required_files)
        listed = listed || have.path == file.path;
      if (!listed && files::parse_required(file.path, nullptr) && info->required_files.size() < 96)
        info->required_files.push_back(std::move(file));
    }
    const char *project = root["upstream"]["project"] | "";
    const char *upstream_version = root["upstream"]["version"] | "";
    if (*project != '\0')
      info->upstream = *upstream_version != '\0' ? std::string(project) + " " + upstream_version : project;
    const char *repo = root["source"]["repo"] | "";
    const char *ref = root["source"]["ref"] | "";
    if (*repo != '\0') {
      std::string short_repo = repo;
      const size_t at = short_repo.find("github.com/");
      if (at != std::string::npos)
        short_repo = short_repo.substr(at + 11);
      info->source = short_repo + (*ref != '\0' ? std::string(" @ ") + std::string(ref).substr(0, 7) : "");
    }
    const char *icon_base64 = icon ? (root["icon"]["base64"] | "") : "";
    const size_t icon_len = std::strlen(icon_base64);
    if (icon_len > 0) {
      info->icon_png.resize(icon_len * 3 / 4 + 4);
      size_t decoded = 0;
      if (mbedtls_base64_decode(info->icon_png.data(), info->icon_png.size(), &decoded,
                                reinterpret_cast<const unsigned char *>(icon_base64), icon_len) == 0) {
        info->icon_png.resize(decoded);
      } else {
        info->icon_png.clear();
      }
    }
    info->has_info = true;
    return true;
  });
}

static std::string size_text(uint32_t bytes) {
  char text[24];
  if (bytes >= 1024 * 1024)
    std::snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
  else
    std::snprintf(text, sizeof(text), "%u KB", static_cast<unsigned>((bytes + 1023) / 1024));
  return text;
}

// ── Icon decoding (runs in the info task, and for the detail page) ─────────

struct IconDecode {
  PNG *png;
  uint16_t *row;       // one decoded line
  uint32_t *acc;       // box-filter accumulators, 3 per output pixel
  uint16_t *counts;    // source pixels per output pixel
  uint16_t *out;
  uint16_t src_w, src_h, side;
};

static void icon_draw(PNGDRAW *draw) {
  auto *d = static_cast<IconDecode *>(draw->pUser);
  d->png->getLineAsRGB565(draw, d->row, PNG_RGB565_LITTLE_ENDIAN, ICON_BLEND_BGR);
  const uint32_t oy = static_cast<uint32_t>(draw->y) * d->side / d->src_h;
  for (uint32_t x = 0; x < d->src_w; x++) {
    const uint32_t ox = x * d->side / d->src_w;
    const uint16_t p = d->row[x];
    uint32_t *a = d->acc + (oy * d->side + ox) * 3;
    a[0] += p >> 11;
    a[1] += (p >> 5) & 0x3F;
    a[2] += p & 0x1F;
    d->counts[oy * d->side + ox]++;
  }
}

// Decodes a PNG and box-filters it down (or nearest-up) to side x side RGB565
// in PSRAM. Safe from any task: each call has its own decoder.
static std::shared_ptr<uint16_t> decode_png_icon(const std::vector<uint8_t> &png_bytes, uint16_t side) {
  if (png_bytes.empty())
    return nullptr;
  void *memory = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory == nullptr)
    return nullptr;
  PNG *png = new (memory) PNG();
  std::shared_ptr<uint16_t> result;
  if (png->openRAM(const_cast<uint8_t *>(png_bytes.data()), static_cast<int>(png_bytes.size()), icon_draw) ==
      PNG_SUCCESS) {
    const uint16_t w = png->getWidth(), h = png->getHeight();
    const size_t cells = static_cast<size_t>(side) * side;
    IconDecode d{};
    d.png = png;
    d.src_w = w;
    d.src_h = h;
    d.side = side;
    bool ok = w > 0 && h > 0 && w <= 512 && h <= 512;
    if (ok) {
      d.row = static_cast<uint16_t *>(heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
      d.acc = static_cast<uint32_t *>(heap_caps_calloc(cells * 3, sizeof(uint32_t), MALLOC_CAP_SPIRAM));
      d.counts = static_cast<uint16_t *>(heap_caps_calloc(cells, sizeof(uint16_t), MALLOC_CAP_SPIRAM));
      d.out = static_cast<uint16_t *>(heap_caps_malloc(cells * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
      ok = d.row && d.acc && d.counts && d.out && png->decode(&d, 0) == PNG_SUCCESS;  // state rides in pUser
    }
    png->close();
    if (ok) {
      for (size_t i = 0; i < cells; i++) {
        const uint32_t n = d.counts[i];
        if (n == 0) {
          // Upscaling leaves gaps: take the cell above or to the left.
          d.out[i] = i >= side ? d.out[i - side] : (i > 0 ? d.out[i - 1] : 0);
          continue;
        }
        const uint32_t *a = d.acc + i * 3;
        d.out[i] = static_cast<uint16_t>(((a[0] / n) << 11) | ((a[1] / n) << 5) | (a[2] / n));
      }
      result = std::shared_ptr<uint16_t>(d.out, [](uint16_t *p) { heap_caps_free(p); });
      d.out = nullptr;
    }
    heap_caps_free(d.row);
    heap_caps_free(d.acc);
    heap_caps_free(d.counts);
    heap_caps_free(d.out);
  }
  png->~PNG();
  heap_caps_free(memory);
  return result;
}

// ── Listings ────────────────────────────────────────────────────────────────

void PappLoader::start_info_fetch_() {
  this->catalog_generation_++;
  this->app_info_.assign(this->catalog_entries_.size(), AppInfo{});
  if (!this->store_ui_ || this->info_loading_ || this->catalog_entries_.empty())
    return;  // a running fetch notices the new generation and starts again
  if (this->launched_) {
    // Nobody sees the store while an app runs, and nine TLS fetches then took
    // the internal DMA memory the app's SD reads and I2S needed (OpenLara's
    // intro video failed to load). loop() starts this when the app ends.
    this->info_deferred_ = true;
    return;
  }
  this->info_urls_.clear();
  for (const auto &entry : this->catalog_entries_)
    this->info_urls_.push_back(entry.second);
  this->info_generation_ = this->catalog_generation_;
  this->info_done_ = false;
  this->info_loading_ = true;
  if (xTaskCreatePinnedToCore(&PappLoader::papp_info_task_entry_, "papp_info", 10240, this, 3, &this->info_task_handle_,
                              0) != pdPASS) {
    this->info_loading_ = false;
    this->info_task_handle_ = nullptr;
    ESP_LOGW(TAG, "Could not start the app info task");
  }
}

void PappLoader::papp_info_task_entry_(void *arg) {
  auto *self = static_cast<PappLoader *>(arg);
  const std::vector<std::string> urls = self->info_urls_;
  std::vector<AppInfo> infos(urls.size());
  size_t found = 0;
  self->info_interrupted_ = false;
  for (size_t i = 0; i < urls.size(); i++) {
    if (self->launched_) {
      self->info_interrupted_ = true;  // an app started: stop between fetches
      break;
    }
    const std::string sidecar = sidecar_for(urls[i]);
    if (sidecar.empty())
      continue;
    std::string text;
    const esp_err_t err = is_network_url(sidecar.c_str()) ? fetch_http_text(sidecar.c_str(), &text, INFO_MAX_BYTES)
                                                          : read_small_file(sidecar, &text, INFO_MAX_BYTES);
    if (err == ESP_OK && parse_app_info(text, &infos[i])) {
      infos[i].sidecar = std::move(text);
      // Decoded here, not on the LVGL loop: a folder of 30+ apps took seconds there.
      infos[i].tile_icon = decode_png_icon(infos[i].icon_png, TILE_ICON);
      found++;
    }
  }
  ESP_LOGI(TAG, "App info: %u of %u app(s) have a listing", static_cast<unsigned>(found),
           static_cast<unsigned>(urls.size()));
  self->info_installed_ = scan_installed_(self->install_dir_);
  self->info_result_ = std::move(infos);
  self->info_done_ = true;
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(100));  // the loop deletes this task
}

void PappLoader::poll_info_fetch_() {
  if (!this->info_loading_ || !this->info_done_)
    return;
  if (this->info_task_handle_ != nullptr) {
    vTaskDelete(this->info_task_handle_);
    this->info_task_handle_ = nullptr;
  }
  this->info_loading_ = false;
  if (this->info_interrupted_) {
    // Stopped early because an app started: fetch them all once it ends.
    this->info_interrupted_ = false;
    this->info_deferred_ = true;
    return;
  }
  if (this->info_generation_ != this->catalog_generation_) {
    // The catalog changed while this ran: fetch the current one.
    this->catalog_generation_--;
    this->start_info_fetch_();
    return;
  }
  this->app_info_ = std::move(this->info_result_);
  this->installed_ = std::move(this->info_installed_);
#ifdef PAPP_LOADER_USE_LVGL
  this->catalog_ui_pending_ = true;
#endif
}

// name -> version of every app with a listing in install_dir. Reads the card,
// so it runs in the info and install tasks.
std::vector<std::pair<std::string, std::string>> PappLoader::scan_installed_(const std::string &install_dir) {
  std::vector<std::pair<std::string, std::string>> installed;
  DIR *folder = opendir(runtime_path(install_dir.c_str()).c_str());
  if (folder == nullptr)
    return installed;
  while (dirent *entry = readdir(folder)) {
    const std::string file = entry->d_name;
    if (file.size() < 6 || file.compare(file.size() - 5, 5, ".json") != 0 || file == "settings.json" ||
        is_manifest_name(file))
      continue;
    std::string text;
    AppInfo info;
    if (read_small_file(install_dir + "/" + file, &text, INFO_MAX_BYTES) == ESP_OK &&
        parse_app_info(text, &info, false) && !info.name.empty())
      installed.emplace_back(info.name, info.version);
  }
  closedir(folder);
  return installed;
}

std::string PappLoader::installed_version_(const std::string &name) const {
  for (const auto &item : this->installed_) {
    if (item.first == name)
      return item.second;
  }
  return {};
}

// ── Per-app settings ────────────────────────────────────────────────────────
// <install_dir>/settings.json maps app names to their settings:
//   {"psram_tulip": {"canvas": "1024x600"}}
// Other apps and other fields are kept when it is rewritten. Read when an app
// starts and by the store's detail page, both on the main loop.

static constexpr size_t SETTINGS_MAX_BYTES = 16 * 1024;

std::string PappLoader::settings_path_() const { return this->install_dir_ + "/settings.json"; }

std::string PappLoader::get_app_canvas(const std::string &app) {
  std::string text;
  if (app.empty() || read_small_file(this->settings_path_(), &text, SETTINGS_MAX_BYTES) != ESP_OK)
    return {};
  std::string value;
  json::parse_json(text, [&app, &value](JsonObject root) -> bool {
    value = root[app]["canvas"] | "";
    return true;
  });
  int w = 0, h = 0;
  return canvas::parse_size(value, &w, &h) ? canvas::format_size(w, h) : std::string();
}

bool PappLoader::set_app_canvas(const std::string &app, const std::string &size) {
  int w = 0, h = 0;
  if (app.empty() || app.size() > 96)
    return false;
  if (!size.empty() &&
      (!canvas::parse_size(size, &w, &h) || !canvas::size_ok(w, h, this->max_canvas_w_, this->max_canvas_h_))) {
    ESP_LOGW(TAG, "Screen setting %s for %s refused: even sizes from %dx%d to %dx%d", size.c_str(), app.c_str(),
             canvas::MIN_WIDTH, canvas::MIN_HEIGHT, this->max_canvas_w_, this->max_canvas_h_);
    return false;
  }
  std::string text;
  JsonDocument doc;
  if (read_small_file(this->settings_path_(), &text, SETTINGS_MAX_BYTES) != ESP_OK || deserializeJson(doc, text) ||
      !doc.is<JsonObject>())
    doc.to<JsonObject>();  // missing or unreadable: start a new one
  JsonObject root = doc.as<JsonObject>();
  JsonObject entry = root[app].as<JsonObject>();
  if (size.empty()) {
    if (!entry.isNull()) {
      entry.remove("canvas");
      if (entry.size() == 0)
        root.remove(app);
    }
  } else {
    if (entry.isNull())
      entry = root[app].to<JsonObject>();
    entry["canvas"] = canvas::format_size(w, h);
  }
  std::string out;
  serializeJsonPretty(doc, out);
  out.push_back('\n');

  // install_dir is a storage root and a folder on it: /sd/roms/papp -> /sd + roms/papp.
  const std::string &dir = this->install_dir_;
  const size_t cut = dir.find('/', 1);
  const std::string storage = runtime_path((cut == std::string::npos ? dir : dir.substr(0, cut)).c_str());
  const std::string target = (cut == std::string::npos ? std::string() : dir.substr(cut + 1) + "/") + "settings.json";
  bool ok = make_parent_dirs(storage, target);
  FILE *file = ok ? std::fopen((storage + "/" + target).c_str(), "wb") : nullptr;
  ok = file != nullptr && std::fwrite(out.data(), 1, out.size(), file) == out.size();
  if (file != nullptr)
    ok = std::fclose(file) == 0 && ok;
  if (ok) {
    ESP_LOGI(TAG, "Screen setting for %s: %s", app.c_str(), size.empty() ? "default" : size.c_str());
  } else {
    ESP_LOGW(TAG, "Could not write %s", this->settings_path_().c_str());
  }
  return ok;
}

bool PappLoader::has_favorites() {
  std::string text;
  if (read_small_file(this->settings_path_(), &text, SETTINGS_MAX_BYTES) != ESP_OK)
    return false;
  bool found = false;
  json::parse_json(text, [&found](JsonObject root) -> bool {
    for (JsonPair pair : root) {
      if (pair.value()["favorite"] | false) {
        found = true;
        break;
      }
    }
    return true;
  });
  return found;
}

#ifdef PAPP_LOADER_USE_LVGL
bool PappLoader::is_app_favorite_(const std::string &app) const {
  if (app.empty())
    return false;
  std::string text;
  if (read_small_file(this->settings_path_(), &text, SETTINGS_MAX_BYTES) != ESP_OK)
    return false;
  bool favorite = false;
  json::parse_json(text, [&app, &favorite](JsonObject root) -> bool {
    favorite = root[app]["favorite"] | false;
    return true;
  });
  return favorite;
}

bool PappLoader::set_app_favorite_(const std::string &app, bool favorite) {
  if (app.empty() || app.size() > 96)
    return false;
  std::string text;
  JsonDocument doc;
  if (read_small_file(this->settings_path_(), &text, SETTINGS_MAX_BYTES) != ESP_OK || deserializeJson(doc, text) ||
      !doc.is<JsonObject>())
    doc.to<JsonObject>();
  JsonObject root = doc.as<JsonObject>();
  JsonObject entry = root[app].as<JsonObject>();
  if (favorite) {
    if (entry.isNull())
      entry = root[app].to<JsonObject>();
    entry["favorite"] = true;
  } else if (!entry.isNull()) {
    entry.remove("favorite");
    if (entry.size() == 0)
      root.remove(app);
  }
  std::string out;
  serializeJsonPretty(doc, out);
  out.push_back('\n');

  const std::string &dir = this->install_dir_;
  const size_t cut = dir.find('/', 1);
  const std::string storage = runtime_path((cut == std::string::npos ? dir : dir.substr(0, cut)).c_str());
  const std::string target = (cut == std::string::npos ? std::string() : dir.substr(cut + 1) + "/") + "settings.json";
  bool ok = make_parent_dirs(storage, target);
  FILE *file = ok ? std::fopen((storage + "/" + target).c_str(), "wb") : nullptr;
  ok = file != nullptr && std::fwrite(out.data(), 1, out.size(), file) == out.size();
  if (file != nullptr)
    ok = std::fclose(file) == 0 && ok;
  if (!ok)
    ESP_LOGW(TAG, "Could not write favorites to %s", this->settings_path_().c_str());
  return ok;
}

void PappLoader::toggle_app_favorite_(int index) {
  const std::string app = this->detail_app_key_(index);
  if (app.empty())
    return;
  const bool favorite = !this->is_app_favorite_(app);
  if (!this->set_app_favorite_(app, favorite)) {
    this->set_progress_(false, 0, 0, "%s", "Could not save Favorites - is the card in?");
    return;
  }
  ESP_LOGI(TAG, "%s favorite: %s", favorite ? "Added" : "Removed", app.c_str());
  this->close_detail_();
  this->catalog_ui_pending_ = true;
}

// ROM favorites share settings.json with app favorites, but live under a
// reserved object so an app name can never collide with a game path.
bool PappLoader::is_rom_favorite_(const std::string &path) const {
  if (path.empty())
    return false;
  std::string text;
  if (read_small_file(this->settings_path_(), &text, SETTINGS_MAX_BYTES) != ESP_OK)
    return false;
  bool favorite = false;
  const std::string app = this->rom_selector_app_;
  json::parse_json(text, [&app, &path, &favorite](JsonObject root) -> bool {
    JsonArray list = root["__rom_favorites"][app].as<JsonArray>();
    for (JsonVariant item : list) {
      if ((item | "") == path) {
        favorite = true;
        break;
      }
    }
    return true;
  });
  return favorite;
}

bool PappLoader::set_rom_favorite_(const std::string &path, bool favorite) {
  if (path.empty() || path.size() > 240 || this->rom_selector_app_.empty())
    return false;
  std::string text;
  JsonDocument doc;
  if (read_small_file(this->settings_path_(), &text, SETTINGS_MAX_BYTES) != ESP_OK || deserializeJson(doc, text) ||
      !doc.is<JsonObject>())
    doc.to<JsonObject>();
  JsonObject root = doc.as<JsonObject>();
  JsonObject all = root["__rom_favorites"].as<JsonObject>();
  if (all.isNull())
    all = root["__rom_favorites"].to<JsonObject>();
  JsonArray list = all[this->rom_selector_app_].as<JsonArray>();
  if (favorite) {
    bool present = false;
    for (JsonVariant item : list) {
      if ((item | "") == path) {
        present = true;
        break;
      }
    }
    if (!present) {
      if (list.isNull())
        list = all[this->rom_selector_app_].to<JsonArray>();
      list.add(path);
    }
  } else if (!list.isNull()) {
    for (size_t i = list.size(); i > 0; i--) {
      if ((list[i - 1] | "") == path)
        list.remove(i - 1);
    }
    if (list.size() == 0)
      all.remove(this->rom_selector_app_);
  }
  if (all.size() == 0)
    root.remove("__rom_favorites");

  std::string out;
  serializeJsonPretty(doc, out);
  out.push_back('\n');
  const std::string &dir = this->install_dir_;
  const size_t cut = dir.find('/', 1);
  const std::string storage = runtime_path((cut == std::string::npos ? dir : dir.substr(0, cut)).c_str());
  const std::string target = (cut == std::string::npos ? std::string() : dir.substr(cut + 1) + "/") + "settings.json";
  bool ok = make_parent_dirs(storage, target);
  FILE *file = ok ? std::fopen((storage + "/" + target).c_str(), "wb") : nullptr;
  ok = file != nullptr && std::fwrite(out.data(), 1, out.size(), file) == out.size();
  if (file != nullptr)
    ok = std::fclose(file) == 0 && ok;
  if (!ok)
    ESP_LOGW(TAG, "Could not write ROM favorites to %s", this->settings_path_().c_str());
  return ok;
}

void PappLoader::toggle_rom_favorites() {
  this->rom_selector_favorites_only_ = !this->rom_selector_favorites_only_;
  this->update_rom_selector_ui_();
}

void PappLoader::toggle_selected_rom_favorite() {
  if (this->rom_selector_selection_ >= this->rom_selector_paths_.size())
    return;
  const std::string path = this->rom_selector_paths_[this->rom_selector_selection_];
  const bool favorite = !this->is_rom_favorite_(path);
  if (!this->set_rom_favorite_(path, favorite)) {
    this->set_progress_(false, 0, 0, "%s", "Could not save game favorite - is the card in?");
    return;
  }
  ESP_LOGI(TAG, "%s ROM favorite: %s", favorite ? "Added" : "Removed", path.c_str());
  this->update_rom_selector_ui_();
}
#endif

// The size an app's listing recommends, for display_get_size when it has no
// Screen setting. Runs on the main loop as the app starts.
std::string PappLoader::listing_recommended_canvas_(const std::string &source) const {
  const std::string key = canvas::app_key(source);
  const AppInfo *found = nullptr;
  for (size_t i = 0; i < this->app_info_.size() && i < this->catalog_entries_.size() && found == nullptr; i++) {
    const AppInfo &info = this->app_info_[i];
    if (info.has_info && (this->catalog_entries_[i].second == source || (!info.name.empty() && info.name == key)))
      found = &info;
  }
  AppInfo sidecar_info;
  if (found == nullptr && !is_network_url(source.c_str())) {
    // The listing next to the .papp (an installed copy's, say).
    std::string text;
    const std::string sidecar = sidecar_for(source);
    if (!sidecar.empty() && read_small_file(sidecar, &text, INFO_MAX_BYTES) == ESP_OK &&
        parse_app_info(text, &sidecar_info, false))
      found = &sidecar_info;
  }
  if (found == nullptr || !found->supports_canvas())
    return {};
  return canvas::recommended(this->max_canvas_w_, this->max_canvas_h_,
                             found->canvas_any ? std::vector<std::string>() : found->canvas_sizes,
                             found->canvas_recommended);
}

// ── Install manifest ────────────────────────────────────────────────────────
// <install_dir>/<app>.installed.json lists the data files the store
// downloaded for an app, so Uninstall can offer to delete exactly those:
//   {"app": "psram_doom", "files": [{"path": "/sd/roms/doom/doom1.wad",
//                                    "size": 4196020, "sha256": "1d7d..."}]}
// No "name" field: older loaders would take it for an installed app's listing.

static constexpr size_t MANIFEST_MAX_BYTES = 16 * 1024;

// Writes a small file at a /sd/... or /usb0/... path, creating its folders.
static bool write_text_file(const std::string &path, const std::string &text) {
  const size_t cut = path.find('/', 1);
  if (cut == std::string::npos)
    return false;
  const std::string storage = runtime_path(path.substr(0, cut).c_str());
  const std::string target = path.substr(cut + 1);
  bool ok = make_parent_dirs(storage, target);
  FILE *file = ok ? std::fopen((storage + "/" + target).c_str(), "wb") : nullptr;
  ok = file != nullptr && std::fwrite(text.data(), 1, text.size(), file) == text.size();
  if (file != nullptr)
    ok = std::fclose(file) == 0 && ok;
  return ok;
}

std::string PappLoader::manifest_path_(const std::string &app) const {
  return this->install_dir_ + "/" + app + MANIFEST_SUFFIX;
}

// Called from the install and load tasks after each verified download.
void PappLoader::record_download_(const std::string &app, const std::string &path, uint32_t size,
                                  const std::string &sha256) {
  if (!data::safe_target(app) || app.find('/') != std::string::npos)
    return;
  const std::string manifest = this->manifest_path_(app);
  std::string text;
  JsonDocument doc;
  if (read_small_file(manifest, &text, MANIFEST_MAX_BYTES) != ESP_OK || deserializeJson(doc, text) ||
      !doc.is<JsonObject>())
    doc.to<JsonObject>();
  JsonObject root = doc.as<JsonObject>();
  root["app"] = app;
  JsonArray list = root["files"].is<JsonArray>() ? root["files"].as<JsonArray>() : root["files"].to<JsonArray>();
  for (size_t i = 0; i < list.size();) {
    if (path == (list[i]["path"] | ""))
      list.remove(i);  // downloaded again: the new entry replaces it
    else
      i++;
  }
  JsonObject entry = list.add<JsonObject>();
  entry["path"] = path;
  entry["size"] = size;
  entry["sha256"] = sha256;
  std::string out;
  serializeJsonPretty(doc, out);
  out.push_back('\n');
  if (!write_text_file(manifest, out))
    ESP_LOGW(TAG, "Could not write %s; Uninstall will not know about %s", manifest.c_str(), path.c_str());
}

std::vector<std::pair<std::string, uint32_t>> PappLoader::downloaded_files_(const std::string &app) const {
  std::vector<std::pair<std::string, uint32_t>> result;
  std::string text;
  if (!data::safe_target(app) || read_small_file(this->manifest_path_(app), &text, MANIFEST_MAX_BYTES) != ESP_OK)
    return result;
  std::vector<std::string> roots = this->data_roots_();
  roots.emplace_back("/sd");
  json::parse_json(text, [&result, &roots](JsonObject root) -> bool {
    JsonArray list = root["files"].as<JsonArray>();
    for (JsonVariant entry : list) {
      const std::string path = entry["path"] | "";
      const uint32_t size = entry["size"] | 0u;
      std::string clean, storage;
      struct stat st{};
      // Only files still exactly as the store left them: a file the user
      // replaced since (a full game over the shareware one) is theirs.
      if (!files::clean_app_path(path.c_str(), roots, &clean, &storage) || clean == storage ||
          stat(runtime_path(clean.c_str()).c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
          static_cast<uint64_t>(st.st_size) != size)
        continue;
      result.emplace_back(clean, size);
    }
    return true;
  });
  return result;
}

// ── Required files ──────────────────────────────────────────────────────────

// Where a listing's "requires" path is: the path under the first data root
// that has it (data_root, then data_search), "" when none does. A stat per
// root, or one folder listing for a pattern; the detail page calls it.
std::string PappLoader::find_required_(const std::string &path) const {
  files::Required req;
  if (!files::parse_required(path, &req))
    return {};
  for (const auto &root : this->data_roots_()) {
    const std::string base = runtime_path(root.c_str());
    struct stat st{};
    if (!req.is_glob) {
      if (stat((base + "/" + req.relative()).c_str(), &st) == 0 &&
          (req.is_dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode)))
        return root + "/" + req.relative();
      continue;
    }
    const std::string folder = req.folder.empty() ? base : base + "/" + req.folder;
    DIR *dir = opendir(folder.c_str());
    if (dir == nullptr)
      continue;
    std::string match;
    while (dirent *entry = readdir(dir)) {
      if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0 ||
          !files::glob_match(req.name.c_str(), entry->d_name))
        continue;
      bool is_dir = entry->d_type == DT_DIR;
      if (entry->d_type == DT_UNKNOWN)
        is_dir = stat((folder + "/" + entry->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
      if (is_dir == req.is_dir) {
        match = entry->d_name;
        break;
      }
    }
    closedir(dir);
    if (!match.empty())
      return root + "/" + (req.folder.empty() ? std::string() : req.folder + "/") + match;
  }
  return {};
}

// ── Uninstall ───────────────────────────────────────────────────────────────

bool PappLoader::uninstall_app(const std::string &app, bool delete_data) {
  if (!data::safe_target(app) || app.find('/') != std::string::npos) {
    ESP_LOGW(TAG, "Uninstall refused: bad app name %s", app.c_str());
    return false;
  }
  if (this->launched_ || this->papp_loading_) {
    // The store is hidden while an app runs, but a lambda could still ask.
    this->set_progress_(false, 0, 0, "%s", "Close the running app before uninstalling");
    ESP_LOGW(TAG, "Uninstall of %s refused while an app is running (%s)", app.c_str(), this->running_source_.c_str());
    return false;
  }
  if (this->install_task_handle_ != nullptr) {
    ESP_LOGW(TAG, "Uninstall of %s refused while an install runs", app.c_str());
    return false;
  }
  const std::vector<std::pair<std::string, uint32_t>> data =
      delete_data ? this->downloaded_files_(app) : std::vector<std::pair<std::string, uint32_t>>();

  const std::string papp = this->install_dir_ + "/" + app + ".papp";
  struct stat st{};
  if (stat(runtime_path(papp.c_str()).c_str(), &st) == 0 && unlink(runtime_path(papp.c_str()).c_str()) != 0) {
    ESP_LOGW(TAG, "Uninstall: could not remove %s (errno %d)", papp.c_str(), errno);
    this->set_progress_(false, 0, 0, "Could not remove %.60s", papp.c_str());
    return false;
  }
  // The listing next to it is what marks the app installed.
  unlink(runtime_path((this->install_dir_ + "/" + app + ".json").c_str()).c_str());
  ESP_LOGI(TAG, "Uninstalled %s: removed %s", app.c_str(), papp.c_str());

  unsigned deleted = 0;
  uint64_t bytes = 0;
  if (delete_data) {
    std::vector<std::string> roots = this->data_roots_();
    roots.emplace_back("/sd");
    for (const auto &file : data) {
      if (unlink(runtime_path(file.first.c_str()).c_str()) != 0) {
        ESP_LOGW(TAG, "Uninstall: could not delete %s (errno %d)", file.first.c_str(), errno);
        continue;
      }
      ESP_LOGI(TAG, "Uninstall: deleted %s (%u bytes)", file.first.c_str(), static_cast<unsigned>(file.second));
      deleted++;
      bytes += file.second;
      // Folders it leaves empty go too; the first one still in use stops it.
      std::string clean, storage;
      if (files::clean_app_path(file.first.c_str(), roots, &clean, &storage) && clean.size() > storage.size()) {
        for (const auto &folder : files::parent_folders(clean.substr(storage.size() + 1))) {
          if (!remove_empty_folder(runtime_path((storage + "/" + folder).c_str())))
            break;
        }
      }
    }
    // Everything it listed is gone or no longer the store's: forget it.
    unlink(runtime_path(this->manifest_path_(app).c_str()).c_str());
  }

  this->installed_.erase(std::remove_if(this->installed_.begin(), this->installed_.end(),
                                        [&app](const std::pair<std::string, std::string> &item) {
                                          return item.first == app;
                                        }),
                         this->installed_.end());
  if (deleted > 0) {
    this->set_progress_(false, 0, 0, "Uninstalled %.40s and deleted %u data file(s), %s", app.c_str(), deleted,
                        size_text(static_cast<uint32_t>(std::min<uint64_t>(bytes, UINT32_MAX))).c_str());
  } else {
    this->set_progress_(false, 0, 0, "Uninstalled %.60s", app.c_str());
  }
  if (!this->catalog_url_.empty() && this->catalog_url_[0] == '/') {
    this->refresh_catalog();  // a folder source may have listed the removed copy
  } else {
#ifdef PAPP_LOADER_USE_LVGL
    this->catalog_ui_pending_ = true;  // badges again; an open detail page reopens with Install
#endif
  }
  return true;
}

// ── Install ─────────────────────────────────────────────────────────────────

void PappLoader::start_install_(int index) {
  if (this->install_task_handle_ != nullptr || this->launched_ || index < 0 ||
      static_cast<size_t>(index) >= this->app_info_.size())
    return;
  const AppInfo &info = this->app_info_[index];
  if (!info.has_info || info.name.empty() || info.size == 0 || info.sha256.size() != 64) {
    this->set_progress_(false, 0, 0, "%s", "This app has no install information");
    return;
  }
  this->install_index_ = index;
  this->direct_install_ = false;
  this->direct_file_upload_ = false;
  this->upload_path_.clear();
  this->install_info_ = info;
  this->install_url_ = this->catalog_entries_[index].second;
  this->install_done_ = false;
  if (xTaskCreatePinnedToCore(&PappLoader::papp_install_task_entry_, "papp_install", 12288, this, 4,
                              &this->install_task_handle_, 0) != pdPASS) {
    this->install_task_handle_ = nullptr;
    this->set_progress_(false, 0, 0, "%s", "Could not start the install");
  }
}

void PappLoader::request_install_url(const std::string &url, const std::string &name, uint32_t size,
                                     const std::string &sha256) {
  if (!is_network_url(url.c_str())) {
    ESP_LOGW(TAG, "Ignoring PAPP install with a non-network URL: %s", url.c_str());
    return;
  }
  if (!data::safe_target(name) || name.find('/') != std::string::npos || name == "." || name == ".." || size == 0 ||
      size > 16 * 1024 * 1024 || sha256.size() != 64) {
    this->set_progress_(false, 0, 0, "%s", "Invalid PAPP upload details");
    ESP_LOGW(TAG, "Ignoring invalid PAPP install: name=%s size=%lu sha256=%u", name.c_str(),
             static_cast<unsigned long>(size), static_cast<unsigned>(sha256.size()));
    return;
  }
  for (const char c : sha256) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      this->set_progress_(false, 0, 0, "%s", "Invalid PAPP SHA-256");
      return;
    }
  }
  if (this->install_task_handle_ != nullptr || this->launched_ || this->papp_loading_) {
    this->set_progress_(false, 0, 0, "%s", "Close the running app before saving a PAPP");
    ESP_LOGW(TAG, "Ignoring PAPP install while the loader is busy: %s", name.c_str());
    return;
  }

  AppInfo info;
  info.name = name;
  info.title = name;
  info.size = size;
  info.sha256 = sha256;
  // Give a command-line upload the same installed badge and basic detail
  // metadata as a catalog install. The name is restricted to safe filename
  // characters above, so it needs no additional JSON escaping here.
  info.sidecar = "{\"name\":\"" + name + "\",\"title\":\"" + name +
                 "\",\"version\":\"local\",\"size\":" + std::to_string(size) +
                 ",\"sha256\":\"" + sha256 + "\"}\n";
  info.has_info = true;
  this->install_index_ = -1;
  this->direct_install_ = true;
  this->direct_file_upload_ = false;
  this->upload_path_.clear();
  this->install_info_ = std::move(info);
  this->install_url_ = url;
  this->install_done_ = false;
  if (xTaskCreatePinnedToCore(&PappLoader::papp_install_task_entry_, "papp_upload", 12288, this, 4,
                              &this->install_task_handle_, 0) != pdPASS) {
    this->install_task_handle_ = nullptr;
    this->direct_install_ = false;
    this->set_progress_(false, 0, 0, "%s", "Could not start the PAPP upload");
    return;
  }
  ESP_LOGI(TAG, "PAPP save requested: %s -> %s/%s.papp", url.c_str(), this->install_dir_.c_str(), name.c_str());
}

void PappLoader::request_upload_url(const std::string &url, const std::string &path, uint32_t size,
                                    const std::string &sha256) {
  const bool safe_path = path.rfind("/sd/", 0) == 0 && data::safe_target(path.substr(4));
  if (!is_network_url(url.c_str()) || !safe_path || size == 0 ||
      size > 1024U * 1024U * 1024U || !data::is_sha256_hex(sha256)) {
    this->set_progress_(false, 0, 0, "%s", "Invalid SD upload details");
    ESP_LOGW(TAG, "Ignoring invalid SD upload: path=%s size=%lu sha256=%u", path.c_str(),
             static_cast<unsigned long>(size), static_cast<unsigned>(sha256.size()));
    return;
  }
  if (path.size() >= sizeof(this->file_request_path_)) {
    this->set_progress_(false, 0, 0, "%s", "SD upload path is too long");
    ESP_LOGW(TAG, "Ignoring SD upload with a path that is too long: %s", path.c_str());
    return;
  }
  if (this->install_task_handle_ != nullptr || this->launched_ || this->papp_loading_) {
    this->set_progress_(false, 0, 0, "%s", "Close the running app before uploading");
    ESP_LOGW(TAG, "Ignoring SD upload while the loader is busy: %s", path.c_str());
    return;
  }

  this->install_index_ = -1;
  this->direct_install_ = true;
  this->direct_file_upload_ = true;
  this->upload_path_ = path;
  this->install_info_ = AppInfo{};
  this->install_info_.name = path.substr(path.find_last_of('/') + 1);
  this->install_info_.title = this->install_info_.name;
  this->install_info_.size = size;
  this->install_info_.sha256 = sha256;
  this->install_url_ = url;
  this->install_done_ = false;
  if (xTaskCreatePinnedToCore(&PappLoader::papp_install_task_entry_, "papp_file_upload", 12288, this, 4,
                              &this->install_task_handle_, 0) != pdPASS) {
    this->install_task_handle_ = nullptr;
    this->direct_install_ = false;
    this->direct_file_upload_ = false;
    this->upload_path_.clear();
    this->set_progress_(false, 0, 0, "%s", "Could not start the SD upload");
    return;
  }
  ESP_LOGI(TAG, "SD upload requested: %s -> %s", url.c_str(), path.c_str());
}

void PappLoader::papp_install_task_entry_(void *arg) {
  auto *self = static_cast<PappLoader *>(arg);
  const AppInfo &info = self->install_info_;
  const std::string &url = self->install_url_;
  esp_err_t err = ESP_OK;
  std::string root;
  std::string target;
  if (self->direct_file_upload_) {
    // The request was validated as /sd/<safe relative path>; split it into
    // the mounted storage root and a relative target for the shared downloader.
    const size_t cut = self->upload_path_.find('/', 1);
    root = runtime_path(self->upload_path_.substr(0, cut).c_str());
    target = self->upload_path_.substr(cut + 1);
  } else {
    // install_dir is a storage root and a folder on it: /sd/roms/papp -> /sd + roms/papp.
    const std::string &dir = self->install_dir_;
    const size_t cut = dir.find('/', 1);
    root = runtime_path((cut == std::string::npos ? dir : dir.substr(0, cut)).c_str());
    const std::string folder = cut == std::string::npos ? std::string() : dir.substr(cut + 1) + "/";
    target = folder + info.name + ".papp";
  }
  const std::string path = root + "/" + target;
  const std::string backup = path + ".upload.bak";
  bool backed_up = false;
  if (!make_parent_dirs(root, target)) {
    self->set_progress_(false, 0, 0, "Cannot write to %.40s - is the card in?",
                        self->direct_file_upload_ ? self->upload_path_.c_str() : self->install_dir_.c_str());
    err = ESP_ERR_NOT_FOUND;
  }
  if (err == ESP_OK) {
    struct stat existing{};
    if (stat(path.c_str(), &existing) == 0) {
      if (!S_ISREG(existing.st_mode)) {
        self->set_progress_(false, 0, 0, "%s", "Install target is not a regular file");
        err = ESP_ERR_INVALID_ARG;
      } else {
        std::remove(backup.c_str());
        if (std::rename(path.c_str(), backup.c_str()) != 0) {
          ESP_LOGE(TAG, "Cannot stage existing install target %s (errno %d)", path.c_str(), errno);
          self->set_progress_(false, 0, 0, "%s", "Could not replace the existing file");
          err = ESP_FAIL;
        } else {
          backed_up = true;
        }
      }
    }
  }
  if (err == ESP_OK) {
    data::DataFile file;
    file.size = info.size;
    file.sha256 = info.sha256;
    file.target = target;
    file.url = url;
    err = self->download_data_file_(file, root + "/" + target, 0, info.size, 1, 1);
    if (err != ESP_OK)
      self->set_progress_(false, 0, 0, "%s failed: %s", self->direct_file_upload_ ? "Upload" : "Install",
                          esp_err_to_name(err));
  }
  if (self->direct_file_upload_) {
    if (err == ESP_OK) {
      if (backed_up)
        std::remove(backup.c_str());
      self->set_progress_(false, 0, 0, "Uploaded %.60s", self->upload_path_.c_str());
    } else if (backed_up) {
      std::remove(path.c_str());
      if (std::rename(backup.c_str(), path.c_str()) != 0)
        ESP_LOGE(TAG, "Could not restore previous upload target %s (errno %d)", path.c_str(), errno);
    }
  } else if (err != ESP_OK && backed_up) {
    std::remove(path.c_str());
    if (std::rename(backup.c_str(), path.c_str()) != 0)
      ESP_LOGE(TAG, "Could not restore previous install target %s (errno %d)", path.c_str(), errno);
  } else if (err == ESP_OK && backed_up) {
    std::remove(backup.c_str());
  }
  if (err == ESP_OK && is_network_url(url.c_str()) && !self->direct_install_)
    err = self->sync_app_data_(url, info.name);  // reports its own progress and errors
  if (err == ESP_OK && !self->direct_file_upload_) {
    // The listing next to the installed copy marks it installed (and at which version).
    const std::string listing = path.substr(0, path.size() - 5) + ".json";
    if (!info.sidecar.empty()) {
      FILE *out = std::fopen(listing.c_str(), "wb");
      if (out != nullptr) {
        std::fwrite(info.sidecar.data(), 1, info.sidecar.size(), out);
        std::fclose(out);
      }
    }
    self->set_progress_(false, 0, 0, "%s %.60s", self->direct_install_ ? "Saved" : "Installed",
                        info.title.empty() ? info.name.c_str() : info.title.c_str());
  }
  self->install_installed_ = scan_installed_(self->install_dir_);
  self->install_result_ = err;
  self->install_done_ = true;
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(100));  // the loop deletes this task
}

void PappLoader::poll_install_() {
  if (this->install_task_handle_ == nullptr || !this->install_done_)
    return;
  vTaskDelete(this->install_task_handle_);
  this->install_task_handle_ = nullptr;
  this->direct_file_upload_ = false;
  this->upload_path_.clear();
  ESP_LOGI(TAG, "Install finished: %s", esp_err_to_name(this->install_result_));
  this->installed_ = std::move(this->install_installed_);
#ifdef PAPP_LOADER_USE_LVGL
  // Rebuilt with the new badges; an open detail page reopens with Launch.
  this->catalog_ui_pending_ = true;
#endif
}

#ifdef PAPP_LOADER_USE_LVGL

// ── Icons ───────────────────────────────────────────────────────────────────

// Shows decoded pixels through an image descriptor; the icon keeps them alive.
void PappLoader::set_icon_(AppIcon *icon, std::shared_ptr<uint16_t> pixels, uint16_t side) {
  *icon = AppIcon{};
  icon->pixels = std::move(pixels);
  icon->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  icon->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  icon->dsc.header.w = side;
  icon->dsc.header.h = side;
  icon->dsc.header.stride = side * sizeof(uint16_t);
  icon->dsc.data_size = static_cast<uint32_t>(side) * side * sizeof(uint16_t);
  icon->dsc.data = reinterpret_cast<const uint8_t *>(icon->pixels.get());
}

void PappLoader::release_icon_(AppIcon *icon) {
  // Raw RGB565 is drawn straight from these pixels (nothing decoded into
  // LVGL's image cache), so dropping the reference once no object shows them
  // is enough; the listing may still hold its own.
  *icon = AppIcon{};
}

void PappLoader::free_icons_() {
  for (auto &icon : this->tile_icons_)
    release_icon_(&icon);
  this->tile_icons_.clear();
}

size_t PappLoader::store_source_index_(int visible_index) const {
  if (visible_index < 0)
    return static_cast<size_t>(-1);
  const size_t visible = static_cast<size_t>(visible_index);
  if (visible >= this->visible_catalog_indices_.size())
    return static_cast<size_t>(-1);
  return this->visible_catalog_indices_[visible];
}

// ── Grid ────────────────────────────────────────────────────────────────────

struct TileContext {
  PappLoader *loader;
  int index;
};

// Bigger and smaller type where the config has those Montserrat sizes;
// otherwise the default font.
static const lv_font_t *title_font() {
#if LV_FONT_MONTSERRAT_32
  return &lv_font_montserrat_32;
#elif LV_FONT_MONTSERRAT_28
  return &lv_font_montserrat_28;
#elif LV_FONT_MONTSERRAT_26
  return &lv_font_montserrat_26;
#elif LV_FONT_MONTSERRAT_24
  return &lv_font_montserrat_24;
#else
  return nullptr;
#endif
}

static const lv_font_t *small_font() {
#if LV_FONT_MONTSERRAT_16
  return &lv_font_montserrat_16;
#elif LV_FONT_MONTSERRAT_14
  return &lv_font_montserrat_14;
#elif LV_FONT_MONTSERRAT_18
  return &lv_font_montserrat_18;
#else
  return nullptr;
#endif
}

static void set_font(lv_obj_t *obj, const lv_font_t *font) {
  if (font != nullptr)
    lv_obj_set_style_text_font(obj, font, 0);
}

static lv_obj_t *plain_box(lv_obj_t *parent) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  return box;
}

static lv_obj_t *text_label(lv_obj_t *parent, const std::string &text, uint32_t color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text.c_str());
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  return label;
}

// Keeps a label to one line ending in "...": LONG_DOT alone still wraps when
// the height follows the content.
static void one_line(lv_obj_t *label, int32_t width) {
  lv_obj_set_width(label, width);
  lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

// Store app names are often longer than a tile or detail heading. Keep the
// heading on one line and let LVGL continuously scroll it instead of cutting
// off the identifying part of the title.
static void scrolling_line(lv_obj_t *label, int32_t width) {
  lv_obj_set_width(label, width);
  lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
  lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
}

// A coloured square with the app's initials, for apps without an icon.
static void letter_tile(lv_obj_t *box, const std::string &title) {
  uint32_t hash = 2166136261u;
  for (char c : title)
    hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
  static const uint32_t palette[] = {0x7C3AED, 0x2563EB, 0x0891B2, 0x059669, 0xD97706, 0xDC2626, 0xDB2777, 0x4F46E5};
  lv_obj_set_style_bg_color(box, lv_color_hex(palette[hash % 8]), 0);
  std::string initials;
  bool start = true;
  for (char c : title) {
    if (std::isalnum(static_cast<unsigned char>(c)) && start && initials.size() < 2) {
      initials += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      start = false;
    } else if (c == ' ' || c == '_' || c == '-') {
      start = true;
    }
  }
  lv_obj_t *label = text_label(box, initials.empty() ? "?" : initials, 0xFFFFFF);
  lv_obj_center(label);
}

static std::string display_title(const PappLoader::AppInfo *info, const std::string &file_label) {
  if (info != nullptr && !info->title.empty())
    return info->title;
  std::string title = file_label;
  if (title.size() > 5 && title.compare(title.size() - 5, 5, ".papp") == 0)
    title = title.substr(0, title.size() - 5);
  return title;
}

void PappLoader::store_tile_event_cb_(lv_event_t *event) {
  auto *context = static_cast<TileContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    // A tap moves the d-pad selection too, so only one tile is ever highlighted.
    context->loader->set_catalog_selection_(static_cast<uint16_t>(context->index));
    context->loader->open_detail_(context->index);
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}

void PappLoader::build_store_grid_() {
  this->free_icons_();
  this->catalog_tiles_.clear();
  if (this->visible_catalog_indices_.empty())
    return;

  lv_obj_update_layout(this->catalog_container_);
  int32_t width = lv_obj_get_content_width(this->catalog_container_);
  if (width <= 0)
    width = 960;
  this->grid_columns_ = static_cast<uint16_t>(std::max<int32_t>(1, (width + TILE_GAP) / (TILE_W + TILE_GAP)));
  const uint16_t cols = this->grid_columns_;
  const size_t count = this->visible_catalog_indices_.size();
  const int32_t rows = static_cast<int32_t>((count + cols - 1) / cols);
  const int32_t left = std::max<int32_t>(0, (width - (cols * TILE_W + (cols - 1) * TILE_GAP)) / 2);

  lv_obj_t *grid = plain_box(this->catalog_container_);
  lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
  // Leave a full gap below the final row. The tiles start half a gap down,
  // so without this extra space the last row extends past the grid's content
  // bounds and can be clipped while the list is being scrolled.
  lv_obj_set_size(grid, width, rows * (TILE_H + TILE_GAP) + TILE_GAP);

  this->tile_icons_.resize(count);
  for (size_t i = 0; i < count; i++) {
    const size_t source_index = this->visible_catalog_indices_[i];
    const AppInfo *info = source_index < this->app_info_.size() && this->app_info_[source_index].has_info
                              ? &this->app_info_[source_index]
                              : nullptr;
    const std::string title = display_title(info, this->catalog_entries_[source_index].first);
    lv_obj_t *tile = plain_box(grid);
    lv_obj_set_pos(tile, left + static_cast<int32_t>(i % cols) * (TILE_W + TILE_GAP),
                   static_cast<int32_t>(i / cols) * (TILE_H + TILE_GAP) + TILE_GAP / 2);
    lv_obj_set_size(tile, TILE_W, TILE_H);
    lv_obj_set_style_radius(tile, 18, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COLOR_TILE), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 2, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(COLOR_TILE_BORDER), 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(COLOR_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(tile, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x172544), LV_STATE_PRESSED);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICK_FOCUSABLE);  // focus follows the selection only
    lv_obj_add_event_cb(tile, store_tile_event_cb_, LV_EVENT_ALL, new TileContext{this, static_cast<int>(i)});  // NOLINT

    lv_obj_t *icon = plain_box(tile);
    lv_obj_set_size(icon, TILE_ICON, TILE_ICON);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_radius(icon, 24, 0);
    lv_obj_set_style_clip_corner(icon, true, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    if (info != nullptr && info->tile_icon) {
      set_icon_(&this->tile_icons_[i], info->tile_icon, TILE_ICON);
      lv_obj_set_style_bg_image_src(icon, &this->tile_icons_[i].dsc, 0);
      lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    } else {
      letter_tile(icon, title);
    }

    lv_obj_t *name = text_label(tile, title, COLOR_TEXT);
    scrolling_line(name, TILE_W - 16);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, TILE_ICON + 18);

    std::string sub;
    if (info != nullptr && !info->version.empty())
      sub = "v" + info->version;
    if (info != nullptr && info->size > 0)
      sub += (sub.empty() ? "" : "  |  ") + size_text(info->size + info->data_size);
    if (!sub.empty()) {
      lv_obj_t *meta = text_label(tile, sub, COLOR_MUTED);
      set_font(meta, small_font());
      one_line(meta, TILE_W - 16);
      lv_obj_set_style_text_align(meta, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(meta, LV_ALIGN_TOP_MID, 0, TILE_ICON + 44);
    }

    // INSTALLED / UPDATE, from the listing next to an installed copy.
    const std::string have = info != nullptr ? this->installed_version_(info->name) : std::string();
    if (!have.empty()) {
      // A round mark in the tile's corner: a tick when installed, an arrow when
      // the store has a newer version.
      const bool update = !info->version.empty() && have != info->version;
      lv_obj_t *badge = plain_box(tile);
      lv_obj_set_size(badge, 30, 30);
      lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -6, 6);
      lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(badge, lv_color_hex(update ? COLOR_UPDATE : COLOR_INSTALLED), 0);
      lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
      lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_t *mark = text_label(badge, update ? LV_SYMBOL_UPLOAD : LV_SYMBOL_OK, 0x0B1020);
      set_font(mark, small_font());
      lv_obj_center(mark);
    }
    this->catalog_tiles_.push_back(tile);
  }
}

// ── Detail page ─────────────────────────────────────────────────────────────

struct DetailButtonContext {
  PappLoader *loader;
  uint8_t action;
};

void PappLoader::store_button_event_cb_(lv_event_t *event) {
  auto *context = static_cast<DetailButtonContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    context->loader->run_detail_action_(context->action);
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}

void PappLoader::open_detail_(int index) {
  const size_t source_index = this->store_ui_ ? this->store_source_index_(index) : static_cast<size_t>(index);
  if (source_index == static_cast<size_t>(-1) || source_index >= this->catalog_entries_.size())
    return;
  this->close_detail_();
  this->detail_index_ = static_cast<int>(source_index);
  this->detail_url_ = this->catalog_entries_[source_index].second;
  const AppInfo *info =
      source_index < this->app_info_.size() && this->app_info_[source_index].has_info ? &this->app_info_[source_index]
                                                                                        : nullptr;
  const std::string title = display_title(info, this->catalog_entries_[source_index].first);
  const std::string url = this->catalog_entries_[source_index].second;
  const bool remote = is_network_url(url.c_str());
  const std::string have = info != nullptr ? this->installed_version_(info->name) : std::string();

  const int32_t screen_w = lv_display_get_horizontal_resolution(nullptr);
  const int32_t screen_h = lv_display_get_vertical_resolution(nullptr);
  lv_obj_t *panel = plain_box(lv_layer_top());
  lv_obj_set_size(panel, screen_w, screen_h);
  lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_PAGE), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);  // keeps touches off the page below
  this->detail_panel_ = panel;
  // The global toolbar volume control must remain usable over the detail
  // panel, just as it is over the library and Favorites pages.
  this->ensure_catalog_volume_control_();
  this->raise_catalog_volume_control_();

  const int32_t margin = 40;
  lv_obj_t *icon = plain_box(panel);
  lv_obj_set_size(icon, DETAIL_ICON, DETAIL_ICON);
  lv_obj_set_pos(icon, margin, margin);
  lv_obj_set_style_radius(icon, 36, 0);
  lv_obj_set_style_clip_corner(icon, true, 0);
  // One icon at the larger size is quick enough to decode here.
  std::shared_ptr<uint16_t> big = info != nullptr ? decode_png_icon(info->icon_png, DETAIL_ICON) : nullptr;
  if (big) {
    set_icon_(&this->detail_icon_, std::move(big), DETAIL_ICON);
    lv_obj_set_style_bg_image_src(icon, &this->detail_icon_.dsc, 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
  } else {
    letter_tile(icon, title);
  }

  // Buttons under the icon.
  std::vector<std::pair<uint8_t, std::string>> buttons;
  if (!have.empty()) {
    buttons.emplace_back(ACTION_LAUNCH, LV_SYMBOL_PLAY "  Launch");
    if (remote && info != nullptr && !info->version.empty() && have != info->version)
      buttons.emplace_back(ACTION_INSTALL, LV_SYMBOL_REFRESH "  Update to\nv" + info->version);
  } else if (remote) {
    buttons.emplace_back(ACTION_STREAM, LV_SYMBOL_PLAY "  Stream");
    if (info != nullptr && info->size > 0 && info->sha256.size() == 64)
      buttons.emplace_back(ACTION_INSTALL, LV_SYMBOL_DOWNLOAD "  Install");
  } else {
    buttons.emplace_back(ACTION_LAUNCH, LV_SYMBOL_PLAY "  Launch");
  }
  // Cartridge-style emulators get a selector on their information page;
  // data-driven ports such as Doom and Red Alert still launch directly.
  if (this->supports_rom_selector(url))
    buttons.emplace_back(ACTION_ROM, LV_SYMBOL_LIST "  Select game");
  if (info != nullptr && info->supports_canvas())
    buttons.emplace_back(ACTION_SCREEN, this->screen_label_(static_cast<int>(source_index)));
  buttons.emplace_back(ACTION_FAVORITE,
                       this->is_app_favorite_(this->detail_app_key_(static_cast<int>(source_index)))
                           ? "*  Remove favorite"
                           : "*  Add to favorites");
  if (!have.empty())
    buttons.emplace_back(ACTION_UNINSTALL, LV_SYMBOL_TRASH "  Uninstall");
  buttons.emplace_back(ACTION_BACK, LV_SYMBOL_CLOSE "  Back");
  this->detail_buttons_.clear();
  this->detail_actions_.clear();
  int32_t y = margin + DETAIL_ICON + 24;
  for (size_t i = 0; i < buttons.size(); i++) {
    lv_obj_t *button = plain_box(panel);
    lv_obj_set_size(button, DETAIL_ICON, 52);
    lv_obj_set_pos(button, margin, y);
    // Keep the added Favorites action and the Back action on the 600px panel
    // for the common six-button detail page while retaining room for the
    // two-line update label.
    y += 54;
    const bool primary = i == 0;
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(primary ? 0x0EA5E9 : 0x1E293B), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(button, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(primary ? 0x0284C7 : 0x334155), LV_STATE_PRESSED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_t *label = text_label(button, buttons[i].second, 0xFFFFFF);
    // Keep detail-button labels inside their fixed boxes. The update number
    // gets its own line; other labels scroll horizontally if a future label
    // is wider than the button.
    const int32_t label_width = DETAIL_ICON - 16;
    const int32_t line_height = lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN));
    lv_obj_set_width(label, label_width);
    if (buttons[i].second.find('\n') != std::string::npos) {
      lv_obj_set_height(label, line_height * 2);
      lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    } else {
      lv_obj_set_height(label, line_height);
      lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, store_button_event_cb_, LV_EVENT_ALL, new DetailButtonContext{this, buttons[i].first});  // NOLINT
    this->detail_buttons_.push_back(button);
    this->detail_actions_.push_back(buttons[i].first);
  }

  // Text column.
  const int32_t text_x = margin + DETAIL_ICON + 40;
  const int32_t text_w = screen_w - text_x - margin;
  lv_obj_t *name = text_label(panel, title, 0xFFFFFF);
  set_font(name, title_font());
  lv_obj_set_pos(name, text_x, margin - 4);
  scrolling_line(name, text_w);

  // Who made it, the facts, then the sizes, each under the one before.
  lv_obj_t *last = name;
  auto line_below = [&last, panel, text_w](const std::string &text, uint32_t color, int32_t gap) {
    lv_obj_t *label = text_label(panel, text, color);
    lv_obj_set_width(label, text_w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align_to(label, last, LV_ALIGN_OUT_BOTTOM_LEFT, 0, gap);
    last = label;
  };
  if (info != nullptr && !info->author.empty())
    line_below("by " + info->author, COLOR_MUTED, 8);
  std::string facts;
  auto add = [&facts](const std::string &part) {
    if (!part.empty())
      facts += (facts.empty() ? "" : "   |   ") + part;
  };
  if (info != nullptr) {
    add(info->version.empty() ? "" : "v" + info->version);
    add(info->category);
    add(info->license);
  }
  add(!have.empty() ? (info != nullptr && !info->version.empty() && have != info->version ? "installed v" + have
                                                                                          : "installed")
                    : "");
  if (info == nullptr)
    add(remote ? "Streams from the network" : url);
  if (!facts.empty())
    line_below(facts, COLOR_MUTED, 4);
  if (info != nullptr && info->size > 0) {
    std::string sizes = "PAPP " + size_text(info->size);
    if (info->data_size > 0)
      sizes += "   +   data " + size_text(info->data_size) + "   =   " + size_text(info->size + info->data_size);
    line_below(sizes, COLOR_ACCENT, 10);
  }

  // Scrollable body: about, then headed sections for controls, source and changelog.
  lv_obj_t *scroller = plain_box(panel);
  lv_obj_set_style_bg_opa(scroller, LV_OPA_TRANSP, 0);
  lv_obj_update_layout(panel);
  const int32_t body_y = lv_obj_get_y2(last) + 24;
  lv_obj_set_pos(scroller, text_x, body_y);
  lv_obj_set_size(scroller, text_w, screen_h - body_y - margin);
  lv_obj_add_flag(scroller, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *below = nullptr;
  auto block = [&below, scroller, text_w](const std::string &text, uint32_t color, int32_t gap) {
    lv_obj_t *label = text_label(scroller, text, color);
    lv_obj_set_width(label, text_w - 12);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    if (below != nullptr)
      lv_obj_align_to(label, below, LV_ALIGN_OUT_BOTTOM_LEFT, 0, gap);
    below = label;
  };
  auto section = [&block](const char *heading, const std::string &text) {
    block(heading, COLOR_ACCENT, 18);
    block(text, COLOR_TEXT, 4);
  };
  if (info != nullptr && !info->about.empty())
    block(info->about, COLOR_TEXT, 0);
  if (info != nullptr && !info->required_files.empty()) {
    // What the app needs on the card, looked for now under every data root:
    // a tick when found (and where, if not on the card), a cross when not.
    block("Files on the card", COLOR_ACCENT, below == nullptr ? 0 : 18);
    for (const auto &file : info->required_files) {
      const std::string found = this->find_required_(file.path);
      std::string line = std::string(found.empty() ? LV_SYMBOL_CLOSE : LV_SYMBOL_OK) + "  " + file.path;
      if (!found.empty() && found.rfind("/sd/", 0) != 0)
        line += "  (on " + found.substr(1, found.find('/', 1) - 1) + ")";
      std::string about = file.note;
      auto add_about = [&about](const std::string &part) { about += (about.empty() ? "" : " - ") + part; };
      if (file.download)
        add_about(found.empty() ? "the store downloads it at Install or first launch" : "downloaded by the store");
      if (file.optional)
        add_about("optional");
      const uint32_t color = !found.empty() ? COLOR_INSTALLED
                             : file.optional || file.download ? COLOR_UPDATE
                                                              : 0xF87171;  // missing: red
      block(line, color, 6);
      if (!about.empty()) {
        block("     " + about, COLOR_MUTED, 0);
        set_font(below, small_font());
      }
    }
  }
  if (info != nullptr && !info->controls.empty()) {
    std::string lines;
    for (const auto &line : info->controls)
      lines += (lines.empty() ? "" : "\n") + line;
    section("Controls", lines);
  }
  if (info != nullptr && (!info->upstream.empty() || !info->source.empty())) {
    std::string lines = info->upstream.empty() ? "" : "Based on " + info->upstream;
    if (!info->source.empty())
      lines += (lines.empty() ? "" : "\n") + std::string("Built from ") + info->source;
    section("Source", lines);
  }
  if (info != nullptr && !info->changelog.empty())
    section("What's new", info->changelog);
  if (below == nullptr)
    block("No listing for this app yet.\n\n" + url, COLOR_MUTED, 0);

  this->focus_detail_button_(0);
  this->progress_ui_screen_ = nullptr;  // progress moves to the loader's panel, above this page
}

void PappLoader::close_detail_() {
  this->close_dialog_();
  if (this->detail_panel_ != nullptr) {
    // Often called from a click on one of the page's own buttons, so the page
    // is hidden now (nothing draws its icon again) and deleted after the event.
    lv_obj_add_flag(this->detail_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(this->detail_panel_);
    this->detail_panel_ = nullptr;
  }
  release_icon_(&this->detail_icon_);
  this->detail_buttons_.clear();
  this->detail_actions_.clear();
  this->detail_index_ = -1;
  this->detail_url_.clear();
  this->progress_ui_screen_ = nullptr;
}

void PappLoader::focus_detail_button_(uint8_t index) {
  if (this->detail_buttons_.empty())
    return;
  index = std::min<uint8_t>(index, static_cast<uint8_t>(this->detail_buttons_.size() - 1));
  for (lv_obj_t *button : this->detail_buttons_)
    lv_obj_remove_state(button, LV_STATE_FOCUSED);
  lv_obj_add_state(this->detail_buttons_[index], LV_STATE_FOCUSED);
  this->detail_focus_ = index;
}

void PappLoader::run_detail_action_(uint8_t action) {
  const int index = this->detail_index_;
  if (index < 0 || static_cast<size_t>(index) >= this->catalog_entries_.size())
    return;
  if (this->install_task_handle_ != nullptr && action != ACTION_BACK)
    return;  // one thing at a time; the progress panel shows the install
  const AppInfo *info = static_cast<size_t>(index) < this->app_info_.size() ? &this->app_info_[index] : nullptr;
  switch (action) {
    case ACTION_STREAM: {
      const std::string url = this->catalog_entries_[index].second;
      if (this->open_rom_selector_for_app(url))
        break;
      this->close_detail_();
      this->request_launch_url(url);
      break;
    }
    case ACTION_LAUNCH: {
      std::string path = this->catalog_entries_[index].second;
      if (info != nullptr && !info->name.empty() && !this->installed_version_(info->name).empty())
        path = this->install_dir_ + "/" + info->name + ".papp";
      if (this->open_rom_selector_for_app(path))
        break;
      this->close_detail_();
      this->request_launch(path);
      break;
    }
    case ACTION_ROM: {
      std::string path = this->catalog_entries_[index].second;
      if (info != nullptr && !info->name.empty() && !this->installed_version_(info->name).empty())
        path = this->install_dir_ + "/" + info->name + ".papp";
      this->open_rom_selector_for_app(path);
      break;
    }
    case ACTION_INSTALL:
      this->start_install_(index);
      break;
    case ACTION_SCREEN:
      this->cycle_screen_setting_(index);
      break;
    case ACTION_FAVORITE:
      this->toggle_app_favorite_(index);
      break;
    case ACTION_UNINSTALL:
      this->open_uninstall_dialog_(index);
      break;
    case ACTION_BACK:
    default:
      this->close_detail_();
      break;
  }
}

// The settings key of the file Launch / Stream would run, as the loader keys
// it at launch (canvas::app_key).
std::string PappLoader::detail_app_key_(int index) const {
  if (index < 0 || static_cast<size_t>(index) >= this->catalog_entries_.size())
    return {};
  const AppInfo *info = static_cast<size_t>(index) < this->app_info_.size() ? &this->app_info_[index] : nullptr;
  if (info != nullptr && !info->name.empty() && !this->installed_version_(info->name).empty())
    return canvas::app_key(this->install_dir_ + "/" + info->name + ".papp");
  return canvas::app_key(this->catalog_entries_[index].second);
}

std::vector<std::string> PappLoader::screen_options_(int index, std::string *recommended) const {
  recommended->clear();
  if (index < 0 || static_cast<size_t>(index) >= this->app_info_.size() || !this->app_info_[index].supports_canvas())
    return {};
  const AppInfo &info = this->app_info_[index];
  const std::vector<std::string> listed = info.canvas_any ? std::vector<std::string>() : info.canvas_sizes;
  *recommended = canvas::recommended(this->max_canvas_w_, this->max_canvas_h_, listed, info.canvas_recommended);
  return canvas::screen_options(canvas::offered(this->max_canvas_w_, this->max_canvas_h_, listed, *recommended),
                                *recommended);
}

// "Screen 1024x600", "Screen Default", or the recommended size marked with
// an asterisk (also what an app with no setting gets).
std::string PappLoader::screen_label_(int index) {
  std::string recommended;
  this->screen_options_(index, &recommended);
  std::string size = this->get_app_canvas(this->detail_app_key_(index));
  if (size.empty())
    size = recommended;
  return std::string(LV_SYMBOL_IMAGE "  Screen ") + (size.empty() ? std::string("Default") : size) +
         (!size.empty() && size == recommended ? "*" : "");
}

// One press of Screen: the next size the app can use on this panel. Without a
// recommended size the list starts with Default (the device's canvas); with
// one, that size stands for "no setting" (and choosing it clears the
// setting). Saved at once; the app gets it from its next start.
void PappLoader::cycle_screen_setting_(int index) {
  std::string recommended;
  const std::vector<std::string> options = this->screen_options_(index, &recommended);
  if (options.empty())
    return;
  const std::string app = this->detail_app_key_(index);
  std::string current = this->get_app_canvas(app);
  if (!recommended.empty() && current == recommended)
    current.clear();
  if (!this->set_app_canvas(app, canvas::next_screen_option(options, current))) {
    this->set_progress_(false, 0, 0, "%s", "Could not save the Screen setting - is the card in?");
    return;
  }
  const std::string label = this->screen_label_(index);
  for (size_t i = 0; i < this->detail_buttons_.size() && i < this->detail_actions_.size(); i++) {
    if (this->detail_actions_[i] != ACTION_SCREEN)
      continue;
    lv_obj_t *text = lv_obj_get_child(this->detail_buttons_[i], 0);
    if (text != nullptr)
      lv_label_set_text(text, label.c_str());
  }
}

// ── Uninstall's confirmation ────────────────────────────────────────────────

struct DialogContext {
  PappLoader *loader;
  uint8_t action;
};

void PappLoader::store_dialog_event_cb_(lv_event_t *event) {
  auto *context = static_cast<DialogContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    context->loader->run_dialog_action_(context->action);
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}

// A touch and d-pad target in the dialog, styled like the detail page's buttons.
static lv_obj_t *dialog_item(lv_obj_t *parent, int32_t width, int32_t height, uint32_t color, uint32_t pressed) {
  lv_obj_t *item = plain_box(parent);
  lv_obj_set_size(item, width, height);
  lv_obj_set_style_radius(item, 14, 0);
  lv_obj_set_style_bg_color(item, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(item, lv_color_hex(pressed), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(item, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(item, 3, LV_STATE_FOCUSED);
  lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(item, LV_OBJ_FLAG_CLICK_FOCUSABLE);
  return item;
}

void PappLoader::open_uninstall_dialog_(int index) {
  if (index < 0 || static_cast<size_t>(index) >= this->app_info_.size() || this->launched_)
    return;
  const AppInfo &info = this->app_info_[index];
  if (!info.has_info || info.name.empty() || this->installed_version_(info.name).empty())
    return;
  this->close_dialog_();
  this->dialog_app_ = info.name;
  this->dialog_delete_data_ = false;
  const std::vector<std::pair<std::string, uint32_t>> data = this->downloaded_files_(info.name);
  uint64_t bytes = 0;
  for (const auto &file : data)
    bytes += file.second;

  // A dimmed layer over the detail page (it takes the touches), with the box on it.
  const int32_t screen_w = lv_display_get_horizontal_resolution(nullptr);
  const int32_t screen_h = lv_display_get_vertical_resolution(nullptr);
  lv_obj_t *shade = plain_box(lv_layer_top());
  lv_obj_set_size(shade, screen_w, screen_h);
  lv_obj_set_style_bg_color(shade, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(shade, LV_OPA_60, 0);
  lv_obj_add_flag(shade, LV_OBJ_FLAG_CLICKABLE);
  this->dialog_ = shade;

  const int32_t box_w = std::min<int32_t>(640, screen_w - 80);
  const int32_t pad = 28;
  const int32_t inner = box_w - 2 * pad;
  lv_obj_t *box = plain_box(shade);
  lv_obj_set_width(box, box_w);
  lv_obj_set_style_radius(box, 20, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(COLOR_TILE), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(box, 2, 0);
  lv_obj_set_style_border_color(box, lv_color_hex(COLOR_TILE_BORDER), 0);

  lv_obj_t *heading = text_label(box, "Uninstall " + (info.title.empty() ? info.name : info.title) + "?", 0xFFFFFF);
  set_font(heading, title_font());
  lv_obj_set_pos(heading, pad, pad);
  one_line(heading, inner);
  lv_obj_t *text = text_label(box,
                              "Removes " + this->install_dir_ + "/" + info.name +
                                  ".papp and its listing. Saves, settings and files you copied yourself stay.",
                              COLOR_TEXT);
  lv_obj_set_width(text, inner);
  lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
  lv_obj_align_to(text, heading, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 14);
  lv_obj_t *last = text;

  this->dialog_items_.clear();
  this->dialog_actions_.clear();
  auto add_item = [this](lv_obj_t *item, uint8_t action) {
    lv_obj_add_event_cb(item, store_dialog_event_cb_, LV_EVENT_ALL, new DialogContext{this, action});  // NOLINT
    this->dialog_items_.push_back(item);
    this->dialog_actions_.push_back(action);
  };
  if (!data.empty()) {
    // Only files from its install manifest that are still as downloaded. A
    // check box of plain objects: no LVGL checkbox widget needed.
    lv_obj_t *row = dialog_item(box, inner, 72, COLOR_TILE, 0x172544);
    lv_obj_align_to(row, last, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
    lv_obj_t *check = plain_box(row);
    lv_obj_set_size(check, 32, 32);
    lv_obj_align(check, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_radius(check, 6, 0);
    lv_obj_set_style_bg_color(check, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(check, 2, 0);
    lv_obj_set_style_border_color(check, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_remove_flag(check, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(text_label(check, "", COLOR_ACCENT));  // the tick, when checked
    char what[112];
    std::snprintf(what, sizeof(what), "Also delete the data the store downloaded for it: %u file(s), %s",
                  static_cast<unsigned>(data.size()),
                  size_text(static_cast<uint32_t>(std::min<uint64_t>(bytes, UINT32_MAX))).c_str());
    lv_obj_t *label = text_label(row, what, COLOR_TEXT);
    lv_obj_set_width(label, inner - 64);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 54, 0);
    add_item(row, DIALOG_TOGGLE_DATA);
    last = row;
  }
  // Uninstall (red) and Cancel side by side.
  const int32_t button_w = (inner - 16) / 2;
  lv_obj_t *confirm = dialog_item(box, button_w, 52, 0xDC2626, 0xB91C1C);
  lv_obj_align_to(confirm, last, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 24);
  lv_obj_center(text_label(confirm, LV_SYMBOL_TRASH "  Uninstall", 0xFFFFFF));
  add_item(confirm, DIALOG_CONFIRM);
  lv_obj_t *cancel = dialog_item(box, button_w, 52, 0x1E293B, 0x334155);
  lv_obj_align_to(cancel, confirm, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
  lv_obj_center(text_label(cancel, LV_SYMBOL_CLOSE "  Cancel", 0xFFFFFF));
  add_item(cancel, DIALOG_CANCEL);

  lv_obj_update_layout(box);
  lv_obj_set_height(box, lv_obj_get_y2(confirm) + pad + 4);
  lv_obj_center(box);
  // Cancel has the focus, so a second press of A changes nothing by accident.
  this->focus_dialog_item_(static_cast<uint8_t>(this->dialog_items_.size() - 1));
}

void PappLoader::close_dialog_() {
  if (this->dialog_ != nullptr) {
    // Usually closed from a click on its own button: hidden now, deleted after the event.
    lv_obj_add_flag(this->dialog_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(this->dialog_);
    this->dialog_ = nullptr;
  }
  this->dialog_items_.clear();
  this->dialog_actions_.clear();
  this->dialog_focus_ = 0;
}

void PappLoader::focus_dialog_item_(uint8_t index) {
  if (this->dialog_items_.empty())
    return;
  index = std::min<uint8_t>(index, static_cast<uint8_t>(this->dialog_items_.size() - 1));
  for (lv_obj_t *item : this->dialog_items_)
    lv_obj_remove_state(item, LV_STATE_FOCUSED);
  lv_obj_add_state(this->dialog_items_[index], LV_STATE_FOCUSED);
  this->dialog_focus_ = index;
}

void PappLoader::run_dialog_action_(uint8_t action) {
  if (this->dialog_ == nullptr)
    return;
  switch (action) {
    case DIALOG_TOGGLE_DATA:
      this->dialog_delete_data_ = !this->dialog_delete_data_;
      for (size_t i = 0; i < this->dialog_items_.size() && i < this->dialog_actions_.size(); i++) {
        if (this->dialog_actions_[i] != DIALOG_TOGGLE_DATA)
          continue;
        lv_obj_t *check = lv_obj_get_child(this->dialog_items_[i], 0);  // row -> check box -> tick
        lv_obj_t *mark = check != nullptr ? lv_obj_get_child(check, 0) : nullptr;
        if (mark != nullptr)
          lv_label_set_text(mark, this->dialog_delete_data_ ? LV_SYMBOL_OK : "");
        this->focus_dialog_item_(static_cast<uint8_t>(i));
      }
      break;
    case DIALOG_CONFIRM: {
      const std::string app = this->dialog_app_;
      const bool delete_data = this->dialog_delete_data_;
      this->close_dialog_();
      this->uninstall_app(app, delete_data);  // rebuilds the page, or says why not
      break;
    }
    case DIALOG_CANCEL:
    default:
      this->close_dialog_();
      break;
  }
}

// Returns true when the store view handled the input.
bool PappLoader::handle_store_controls_(uint8_t newly_pressed, bool a_pressed, bool b_pressed, bool l_pressed,
                                        bool r_pressed, bool select_pressed) {
  const bool up = newly_pressed & (1U << 0), right = newly_pressed & (1U << 1), down = newly_pressed & (1U << 2),
             left = newly_pressed & (1U << 3);
  if (this->dialog_ != nullptr) {
    // Uninstall's confirmation: any direction moves, A presses, B cancels.
    if ((up || left) && this->dialog_focus_ > 0)
      this->focus_dialog_item_(this->dialog_focus_ - 1);
    else if (down || right)
      this->focus_dialog_item_(this->dialog_focus_ + 1);
    if (a_pressed && this->dialog_focus_ < this->dialog_actions_.size())
      this->run_dialog_action_(this->dialog_actions_[this->dialog_focus_]);
    else if (b_pressed)
      this->close_dialog_();
    return true;
  }
  if (this->detail_panel_ != nullptr) {
    if (up && this->detail_focus_ > 0)
      this->focus_detail_button_(this->detail_focus_ - 1);
    else if (down)
      this->focus_detail_button_(this->detail_focus_ + 1);
    if (a_pressed && this->detail_focus_ < this->detail_actions_.size())
      this->run_detail_action_(this->detail_actions_[this->detail_focus_]);
    else if (b_pressed)
      this->close_detail_();
    return true;
  }
  if (!this->store_ui_)
    return false;
  if (select_pressed) {
    this->toggle_side_menu();
    return true;
  }
  if (this->side_menu_open_ && this->drawer_ != nullptr && !l_pressed && !r_pressed) {
    if (up && this->drawer_focus_ > 0)
      this->focus_drawer_button_(this->drawer_focus_ - 1);
    else if (down)
      this->focus_drawer_button_(this->drawer_focus_ + 1);
    if (a_pressed && this->drawer_focus_ < this->drawer_actions_.size())
      this->run_drawer_action_(this->drawer_actions_[this->drawer_focus_]);
    else if (b_pressed || right)
      this->set_side_menu(false);
    return true;
  }
  if (l_pressed || r_pressed) {
    this->next_catalog(r_pressed ? 1 : -1);
    return true;
  }
  const size_t count = this->catalog_tiles_.size();
  if (count == 0)
    return true;
  int next = this->catalog_selection_;
  const int cols = std::max<int>(1, this->grid_columns_);
  if (left)
    next -= 1;
  else if (right)
    next += 1;
  else if (up)
    next -= cols;
  else if (down)
    next += cols;
  next = std::max(0, std::min(static_cast<int>(count) - 1, next));
  if (next != this->catalog_selection_)
    this->set_catalog_selection_(static_cast<uint16_t>(next));
  if (a_pressed)
    this->open_detail_(this->catalog_selection_);
  return true;
}

// ── Side menu ───────────────────────────────────────────────────────────────

static constexpr int32_t DRAWER_W = 300;
static constexpr int32_t DRAWER_TAB = 34;
static constexpr int DRAWER_TOGGLE = -2;
static constexpr int DRAWER_REFRESH = -1;

struct DrawerContext {
  PappLoader *loader;
  int action;
};

// The drawer's x on its screen: parked with only the tab showing, or open.
// It sits over the screen's padding so it reaches the edge.
static int32_t drawer_x(lv_obj_t *drawer, bool open) {
  lv_obj_t *screen = lv_obj_get_parent(drawer);
  return lv_obj_get_content_width(screen) + lv_obj_get_style_pad_right(screen, LV_PART_MAIN) -
         (open ? DRAWER_W : DRAWER_TAB);
}

static void slide_drawer(lv_obj_t *drawer, int32_t to) {
  lv_anim_delete(drawer, nullptr);
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, drawer);
  lv_anim_set_values(&anim, lv_obj_get_x(drawer), to);
  lv_anim_set_duration(&anim, 200);
  lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&anim, [](void *obj, int32_t x) { lv_obj_set_x(static_cast<lv_obj_t *>(obj), x); });
  lv_anim_start(&anim);
}

void PappLoader::store_drawer_event_cb_(lv_event_t *event) {
  auto *context = static_cast<DrawerContext *>(lv_event_get_user_data(event));
  if (context == nullptr)
    return;
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    if (context->action == DRAWER_TOGGLE)
      context->loader->toggle_side_menu();
    else
      context->loader->run_drawer_action_(context->action);
  } else if (lv_event_get_code(event) == LV_EVENT_DELETE) {
    delete context;
  }
}

void PappLoader::build_drawer_() {
  if (this->drawer_ != nullptr) {
    lv_obj_add_flag(this->drawer_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(this->drawer_);
    this->drawer_ = nullptr;
  }
  this->drawer_buttons_.clear();
  this->drawer_actions_.clear();
  if (!this->store_ui_ || this->catalog_container_ == nullptr || this->favorites_page_active_)
    return;
  lv_obj_t *screen = lv_obj_get_screen(this->catalog_container_);
  lv_obj_t *drawer = plain_box(screen);
  lv_obj_add_flag(drawer, LV_OBJ_FLAG_FLOATING);  // not moved by scrolling or layouts
  // Leave the fixed top toolbar/header uncovered. The bottom margin also
  // keeps the drawer from touching the display edge on shorter panels.
  constexpr int32_t drawer_top = 48;
  constexpr int32_t drawer_bottom = 12;
  const int32_t drawer_height = std::max<int32_t>(0, lv_obj_get_height(screen) - drawer_top - drawer_bottom);
  lv_obj_set_size(drawer, DRAWER_W, drawer_height);
  lv_obj_set_y(drawer, drawer_top);
  lv_obj_set_style_bg_opa(drawer, LV_OPA_TRANSP, 0);
  this->drawer_ = drawer;
  lv_obj_set_x(drawer, drawer_x(drawer, this->side_menu_open_));

  // The panel: the source's name and where it reads from, then its buttons.
  const int32_t panel_w = DRAWER_W - DRAWER_TAB + 8;
  lv_obj_t *panel = plain_box(drawer);
  lv_obj_set_size(panel, panel_w, lv_pct(100));
  lv_obj_set_x(panel, DRAWER_TAB - 8);
  lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_TILE), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(COLOR_TILE_BORDER), 0);
  lv_obj_set_style_pad_all(panel, 20, 0);

  const CatalogSource *source = this->catalogs_.empty() ? nullptr : &this->catalogs_[this->catalog_index_];
  const int32_t inner = panel_w - 40;
  lv_obj_t *title = text_label(panel, source != nullptr ? source->name : std::string("Library"), COLOR_ACCENT);
  set_font(title, title_font());
  one_line(title, inner);
  char count[32];
  std::snprintf(count, sizeof(count), "%u app(s)", static_cast<unsigned>(this->catalog_entries_.size()));
  std::string about_source = (source != nullptr ? source->url : this->catalog_url_) + "\n" + count;
  // A folder source reads the storage roots: say what each holds (or that it
  // is missing), e.g. "Root files: sd 29 | usb0 -". Rebuilt on every refresh.
  const std::string &url = source != nullptr ? source->url : this->catalog_url_;
  if (!url.empty() && url[0] == '/') {
    std::string roots;
    for (const auto &root : this->data_search_) {
      int entries = -1;
      if (DIR *dir = opendir(runtime_path(root.c_str()).c_str())) {
        entries = 0;
        while (dirent *entry = readdir(dir)) {
          if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
            entries++;
        }
        closedir(dir);
      }
      roots += (roots.empty() ? "" : "  |  ") + root.substr(1) + " " +
               (entries < 0 ? std::string("-") : std::to_string(entries));
    }
    if (!roots.empty())
      about_source += "\nRoot files: " + roots;
  }
  lv_obj_t *where = text_label(panel, about_source, COLOR_MUTED);
  set_font(where, small_font());
  lv_obj_set_width(where, inner);
  lv_label_set_long_mode(where, LV_LABEL_LONG_WRAP);
  lv_obj_align_to(where, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
  lv_obj_update_layout(panel);
  int32_t y = lv_obj_get_y2(where) + 20;

  std::vector<std::pair<int, std::string>> buttons;
  buttons.emplace_back(DRAWER_REFRESH, LV_SYMBOL_REFRESH "  Refresh");
  if (source != nullptr) {
    for (size_t i = 0; i < source->actions.size(); i++)
      buttons.emplace_back(static_cast<int>(i), source->actions[i].first);
  }
  for (const auto &item : buttons) {
    lv_obj_t *button = plain_box(panel);
    lv_obj_set_size(button, inner, 52);
    lv_obj_set_pos(button, 0, y);
    y += 62;
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x0EA5E9), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x0284C7), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(button, 3, LV_STATE_FOCUSED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_t *label = text_label(button, item.second, 0xFFFFFF);
    one_line(label, inner - 16);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, store_drawer_event_cb_, LV_EVENT_ALL, new DrawerContext{this, item.first});  // NOLINT
    this->drawer_buttons_.push_back(button);
    this->drawer_actions_.push_back(item.first);
  }
  lv_obj_t *hint = text_label(panel, "Select: open / close", COLOR_MUTED);
  set_font(hint, small_font());
  lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  // The tab: a strip down the left edge, all that shows while parked. Created
  // last so it is on top of the panel's edge.
  lv_obj_t *strip = plain_box(drawer);
  lv_obj_set_size(strip, DRAWER_TAB, lv_pct(100));
  lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(strip, LV_OBJ_FLAG_CLICK_FOCUSABLE);
  lv_obj_add_event_cb(strip, store_drawer_event_cb_, LV_EVENT_ALL, new DrawerContext{this, DRAWER_TOGGLE});  // NOLINT
  lv_obj_t *pill = plain_box(strip);
  lv_obj_set_size(pill, DRAWER_TAB, 96);
  lv_obj_align(pill, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_radius(pill, 12, 0);
  lv_obj_set_style_bg_color(pill, lv_color_hex(0x1E293B), 0);
  lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(pill, 1, 0);
  lv_obj_set_style_border_color(pill, lv_color_hex(COLOR_ACCENT), 0);
  lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *arrow = text_label(pill, this->side_menu_open_ ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT, COLOR_ACCENT);
  lv_obj_center(arrow);

  if (this->side_menu_open_)
    this->focus_drawer_button_(this->drawer_focus_);
}

void PappLoader::focus_drawer_button_(uint8_t index) {
  if (this->drawer_buttons_.empty())
    return;
  index = std::min<uint8_t>(index, static_cast<uint8_t>(this->drawer_buttons_.size() - 1));
  for (lv_obj_t *button : this->drawer_buttons_)
    lv_obj_remove_state(button, LV_STATE_FOCUSED);
  lv_obj_add_state(this->drawer_buttons_[index], LV_STATE_FOCUSED);
  this->drawer_focus_ = index;
}

void PappLoader::run_drawer_action_(int action) {
  if (action == DRAWER_REFRESH) {
    this->refresh_catalog();
    return;
  }
  if (this->catalogs_.empty())
    return;
  const auto &actions = this->catalogs_[this->catalog_index_].actions;
  if (action >= 0 && static_cast<size_t>(action) < actions.size() && actions[action].second != nullptr)
    actions[action].second->trigger();
}

#endif  // PAPP_LOADER_USE_LVGL

void PappLoader::set_side_menu(bool open) {
  this->side_menu_open_ = open;
#ifdef PAPP_LOADER_USE_LVGL
  if (this->drawer_ == nullptr)
    return;
  slide_drawer(this->drawer_, drawer_x(this->drawer_, open));
  // The tab strip is the drawer's last child: strip -> pill -> arrow.
  lv_obj_t *strip = lv_obj_get_child(this->drawer_, -1);
  lv_obj_t *pill = strip != nullptr ? lv_obj_get_child(strip, 0) : nullptr;
  lv_obj_t *arrow = pill != nullptr ? lv_obj_get_child(pill, 0) : nullptr;
  if (arrow != nullptr)
    lv_label_set_text(arrow, open ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT);
  if (open) {
    this->focus_drawer_button_(0);
  } else {
    for (lv_obj_t *button : this->drawer_buttons_)
      lv_obj_remove_state(button, LV_STATE_FOCUSED);
  }
#endif
}

}  // namespace papp_loader
}  // namespace esphome
