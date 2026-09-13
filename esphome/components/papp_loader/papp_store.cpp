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
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <new>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esphome/components/json/json_util.h"
#include "esphome/core/log.h"
#include "mbedtls/base64.h"

#ifdef PAPP_LOADER_USE_LVGL
#include "PNGdec.h"
#endif

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

enum DetailAction : uint8_t { ACTION_STREAM, ACTION_INSTALL, ACTION_LAUNCH, ACTION_BACK };

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

static bool parse_app_info(const std::string &text, PappLoader::AppInfo *info) {
  return json::parse_json(text, [info](JsonObject root) -> bool {
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
    for (JsonVariant line : root["controls"].as<JsonArray>()) {
      const char *value = line | "";
      if (*value != '\0')
        info->controls.emplace_back(value);
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
    const char *icon = root["icon"]["base64"] | "";
    const size_t icon_len = std::strlen(icon);
    if (icon_len > 0) {
      info->icon_png.resize(icon_len * 3 / 4 + 4);
      size_t decoded = 0;
      if (mbedtls_base64_decode(info->icon_png.data(), info->icon_png.size(), &decoded,
                                reinterpret_cast<const unsigned char *>(icon), icon_len) == 0) {
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

// ── Listings ────────────────────────────────────────────────────────────────

void PappLoader::start_info_fetch_() {
  this->catalog_generation_++;
  this->app_info_.assign(this->catalog_entries_.size(), AppInfo{});
  if (!this->store_ui_ || this->info_loading_ || this->catalog_entries_.empty())
    return;  // a running fetch notices the new generation and starts again
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
  for (size_t i = 0; i < urls.size(); i++) {
    const std::string sidecar = sidecar_for(urls[i]);
    if (sidecar.empty())
      continue;
    std::string text;
    const esp_err_t err = is_network_url(sidecar.c_str()) ? fetch_http_text(sidecar.c_str(), &text, INFO_MAX_BYTES)
                                                          : read_small_file(sidecar, &text, INFO_MAX_BYTES);
    if (err == ESP_OK && parse_app_info(text, &infos[i])) {
      infos[i].sidecar = std::move(text);
      found++;
    }
  }
  ESP_LOGI(TAG, "App info: %u of %u app(s) have a listing", static_cast<unsigned>(found),
           static_cast<unsigned>(urls.size()));
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
  if (this->info_generation_ != this->catalog_generation_) {
    // The catalog changed while this ran: fetch the current one.
    this->catalog_generation_--;
    this->start_info_fetch_();
    return;
  }
  this->app_info_ = std::move(this->info_result_);
#ifdef PAPP_LOADER_USE_LVGL
  this->catalog_ui_pending_ = true;
#endif
}

void PappLoader::scan_installed_() {
  this->installed_.clear();
  const std::string dir = runtime_path(this->install_dir_.c_str());
  DIR *folder = opendir(dir.c_str());
  if (folder == nullptr)
    return;
  while (dirent *entry = readdir(folder)) {
    const std::string file = entry->d_name;
    if (file.size() < 6 || file.compare(file.size() - 5, 5, ".json") != 0)
      continue;
    std::string text;
    AppInfo info;
    if (read_small_file(this->install_dir_ + "/" + file, &text, INFO_MAX_BYTES) == ESP_OK &&
        parse_app_info(text, &info) && !info.name.empty())
      this->installed_.emplace_back(info.name, info.version);
  }
  closedir(folder);
}

std::string PappLoader::installed_version_(const std::string &name) const {
  for (const auto &item : this->installed_) {
    if (item.first == name)
      return item.second;
  }
  return {};
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
  this->install_info_ = info;
  this->install_url_ = this->catalog_entries_[index].second;
  this->install_done_ = false;
  if (xTaskCreatePinnedToCore(&PappLoader::papp_install_task_entry_, "papp_install", 12288, this, 4,
                              &this->install_task_handle_, 0) != pdPASS) {
    this->install_task_handle_ = nullptr;
    this->set_progress_(false, 0, 0, "%s", "Could not start the install");
  }
}

void PappLoader::papp_install_task_entry_(void *arg) {
  auto *self = static_cast<PappLoader *>(arg);
  const AppInfo &info = self->install_info_;
  const std::string &url = self->install_url_;
  esp_err_t err = ESP_OK;
  // install_dir is a storage root and a folder on it: /sd/roms/papp -> /sd + roms/papp.
  const std::string &dir = self->install_dir_;
  const size_t cut = dir.find('/', 1);
  const std::string root = runtime_path((cut == std::string::npos ? dir : dir.substr(0, cut)).c_str());
  const std::string folder = cut == std::string::npos ? std::string() : dir.substr(cut + 1) + "/";
  const std::string target = folder + info.name + ".papp";
  if (!make_parent_dirs(root, target)) {
    self->set_progress_(false, 0, 0, "Cannot write to %.40s - is the card in?", self->install_dir_.c_str());
    err = ESP_ERR_NOT_FOUND;
  }
  if (err == ESP_OK) {
    data::DataFile file;
    file.size = info.size;
    file.sha256 = info.sha256;
    file.target = target;
    file.url = url;
    err = self->download_data_file_(file, root + "/" + target, 0, info.size, 1, 1);
    if (err != ESP_OK)
      self->set_progress_(false, 0, 0, "Install failed: %s", esp_err_to_name(err));
  }
  if (err == ESP_OK && is_network_url(url.c_str()))
    err = self->sync_app_data_(url);  // reports its own progress and errors
  if (err == ESP_OK) {
    // The listing next to the installed copy marks it installed (and at which version).
    const std::string listing = root + "/" + folder + info.name + ".json";
    FILE *out = std::fopen(listing.c_str(), "wb");
    if (out != nullptr) {
      std::fwrite(info.sidecar.data(), 1, info.sidecar.size(), out);
      std::fclose(out);
    }
    self->set_progress_(false, 0, 0, "Installed %.60s", info.title.empty() ? info.name.c_str() : info.title.c_str());
  }
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
  ESP_LOGI(TAG, "Install finished: %s", esp_err_to_name(this->install_result_));
#ifdef PAPP_LOADER_USE_LVGL
  // Rebuilt with the new badges; an open detail page reopens with Launch.
  this->catalog_ui_pending_ = true;
#endif
}

#ifdef PAPP_LOADER_USE_LVGL

// ── Icons ───────────────────────────────────────────────────────────────────

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

// Decodes a PNG and box-filters it down (or nearest-up) to side x side RGB565.
bool PappLoader::decode_icon_(const std::vector<uint8_t> &png_bytes, uint16_t side, AppIcon *out) {
  static PNG *png = nullptr;
  if (png == nullptr) {
    void *memory = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr)
      return false;
    png = new (memory) PNG();
  }
  if (png_bytes.empty() ||
      png->openRAM(const_cast<uint8_t *>(png_bytes.data()), static_cast<int>(png_bytes.size()), icon_draw) != PNG_SUCCESS)
    return false;
  const uint16_t w = png->getWidth(), h = png->getHeight();
  if (w == 0 || h == 0 || w > 512 || h > 512) {
    png->close();
    return false;
  }
  const size_t cells = static_cast<size_t>(side) * side;
  IconDecode d{};
  d.png = png;
  d.src_w = w;
  d.src_h = h;
  d.side = side;
  d.row = static_cast<uint16_t *>(heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
  d.acc = static_cast<uint32_t *>(heap_caps_calloc(cells * 3, sizeof(uint32_t), MALLOC_CAP_SPIRAM));
  d.counts = static_cast<uint16_t *>(heap_caps_calloc(cells, sizeof(uint16_t), MALLOC_CAP_SPIRAM));
  d.out = static_cast<uint16_t *>(heap_caps_malloc(cells * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
  bool ok = d.row && d.acc && d.counts && d.out;
  if (ok)
    ok = png->decode(&d, 0) == PNG_SUCCESS;  // the decode state rides in pUser
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
    out->pixels = d.out;
    std::memset(&out->dsc, 0, sizeof(out->dsc));
    out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    out->dsc.header.w = side;
    out->dsc.header.h = side;
    out->dsc.header.stride = side * sizeof(uint16_t);
    out->dsc.data_size = cells * sizeof(uint16_t);
    out->dsc.data = reinterpret_cast<const uint8_t *>(d.out);
    d.out = nullptr;
  }
  heap_caps_free(d.row);
  heap_caps_free(d.acc);
  heap_caps_free(d.counts);
  heap_caps_free(d.out);
  return ok;
}

void PappLoader::release_icon_(AppIcon *icon) {
  // Raw RGB565 is drawn straight from these pixels (nothing decoded into
  // LVGL's image cache), so they can simply be freed once no object shows them.
  if (icon->pixels != nullptr)
    heap_caps_free(icon->pixels);
  *icon = AppIcon{};
}

void PappLoader::free_icons_() {
  for (auto &icon : this->tile_icons_)
    release_icon_(&icon);
  this->tile_icons_.clear();
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
  this->scan_installed_();
  if (this->catalog_entries_.empty())
    return;

  lv_obj_update_layout(this->catalog_container_);
  int32_t width = lv_obj_get_content_width(this->catalog_container_);
  if (width <= 0)
    width = 960;
  this->grid_columns_ = static_cast<uint16_t>(std::max<int32_t>(1, (width + TILE_GAP) / (TILE_W + TILE_GAP)));
  const uint16_t cols = this->grid_columns_;
  const size_t count = this->catalog_entries_.size();
  const int32_t rows = static_cast<int32_t>((count + cols - 1) / cols);
  const int32_t left = std::max<int32_t>(0, (width - (cols * TILE_W + (cols - 1) * TILE_GAP)) / 2);

  lv_obj_t *grid = plain_box(this->catalog_container_);
  lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
  lv_obj_set_size(grid, width, rows * (TILE_H + TILE_GAP));

  this->tile_icons_.resize(count);
  for (size_t i = 0; i < count; i++) {
    const AppInfo *info = i < this->app_info_.size() && this->app_info_[i].has_info ? &this->app_info_[i] : nullptr;
    const std::string title = display_title(info, this->catalog_entries_[i].first);
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
    if (info != nullptr && this->decode_icon_(info->icon_png, TILE_ICON, &this->tile_icons_[i])) {
      lv_obj_set_style_bg_image_src(icon, &this->tile_icons_[i].dsc, 0);
      lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    } else {
      letter_tile(icon, title);
    }

    lv_obj_t *name = text_label(tile, title, COLOR_TEXT);
    one_line(name, TILE_W - 16);
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
  if (index < 0 || static_cast<size_t>(index) >= this->catalog_entries_.size())
    return;
  this->close_detail_();
  this->detail_index_ = index;
  this->detail_url_ = this->catalog_entries_[index].second;
  const AppInfo *info =
      static_cast<size_t>(index) < this->app_info_.size() && this->app_info_[index].has_info ? &this->app_info_[index]
                                                                                             : nullptr;
  const std::string title = display_title(info, this->catalog_entries_[index].first);
  const std::string url = this->catalog_entries_[index].second;
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

  const int32_t margin = 40;
  lv_obj_t *icon = plain_box(panel);
  lv_obj_set_size(icon, DETAIL_ICON, DETAIL_ICON);
  lv_obj_set_pos(icon, margin, margin);
  lv_obj_set_style_radius(icon, 36, 0);
  lv_obj_set_style_clip_corner(icon, true, 0);
  if (info != nullptr && this->decode_icon_(info->icon_png, DETAIL_ICON, &this->detail_icon_)) {
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
      buttons.emplace_back(ACTION_INSTALL, LV_SYMBOL_REFRESH "  Update to v" + info->version);
  } else if (remote) {
    buttons.emplace_back(ACTION_STREAM, LV_SYMBOL_PLAY "  Stream");
    if (info != nullptr && info->size > 0 && info->sha256.size() == 64)
      buttons.emplace_back(ACTION_INSTALL, LV_SYMBOL_DOWNLOAD "  Install");
  } else {
    buttons.emplace_back(ACTION_LAUNCH, LV_SYMBOL_PLAY "  Launch");
  }
  buttons.emplace_back(ACTION_BACK, LV_SYMBOL_CLOSE "  Back");
  this->detail_buttons_.clear();
  this->detail_actions_.clear();
  int32_t y = margin + DETAIL_ICON + 24;
  for (size_t i = 0; i < buttons.size(); i++) {
    lv_obj_t *button = plain_box(panel);
    lv_obj_set_size(button, DETAIL_ICON, 52);
    lv_obj_set_pos(button, margin, y);
    y += 64;
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
  one_line(name, text_w);

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
      this->close_detail_();
      this->request_launch_url(url);
      break;
    }
    case ACTION_LAUNCH: {
      std::string path = this->catalog_entries_[index].second;
      if (info != nullptr && !info->name.empty() && !this->installed_version_(info->name).empty())
        path = this->install_dir_ + "/" + info->name + ".papp";
      this->close_detail_();
      this->request_launch(path);
      break;
    }
    case ACTION_INSTALL:
      this->start_install_(index);
      break;
    case ACTION_BACK:
    default:
      this->close_detail_();
      break;
  }
}

// Returns true when the store view handled the input.
bool PappLoader::handle_store_controls_(uint8_t newly_pressed, bool a_pressed, bool b_pressed, bool l_pressed,
                                        bool r_pressed, bool select_pressed) {
  const bool up = newly_pressed & (1U << 0), right = newly_pressed & (1U << 1), down = newly_pressed & (1U << 2),
             left = newly_pressed & (1U << 3);
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
  if (!this->store_ui_ || this->catalog_container_ == nullptr)
    return;
  lv_obj_t *screen = lv_obj_get_screen(this->catalog_container_);
  lv_obj_t *drawer = plain_box(screen);
  lv_obj_add_flag(drawer, LV_OBJ_FLAG_FLOATING);  // not moved by scrolling or layouts
  lv_obj_set_size(drawer, DRAWER_W, lv_obj_get_height(screen));
  lv_obj_set_y(drawer, -lv_obj_get_style_pad_top(screen, LV_PART_MAIN));
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
  lv_obj_t *where = text_label(panel, (source != nullptr ? source->url : this->catalog_url_) + "\n" + count, COLOR_MUTED);
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
