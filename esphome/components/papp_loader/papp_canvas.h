#pragma once

// The PAPP canvas: the part of the panel an app draws.
//
// An app gets the ABI v1 canvas of 800x480 unless it asks for another size
// (psram_app.h display_get_size / display_set_canvas). The canvas is centred
// on the panel; the loader's close control sits beside it when there is room,
// else over its top-right corner.
//
// Everything here is plain C++ with no ESP-IDF dependency, so it can be unit
// tested on a host (tests/cpp/test_papp_canvas.cpp).

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace papp_loader {
namespace canvas {

// What apps that never ask for a size draw (and what every app starts with).
static constexpr int LEGACY_WIDTH = 800;
static constexpr int LEGACY_HEIGHT = 480;
// The panel this loader was written for, assumed when the display reports no
// usable size.
static constexpr int FALLBACK_PANEL_WIDTH = 1024;
static constexpr int FALLBACK_PANEL_HEIGHT = 600;
// The smallest and largest canvas an app may ask for (the largest is also
// capped at the panel).
static constexpr int MIN_WIDTH = 320;
static constexpr int MIN_HEIGHT = 240;
static constexpr int MAX_SIDE = 4096;

// The on-screen close control: a square button, placed GAP pixels right of the
// canvas and MARGIN below its top when that fits on the panel, else MARGIN
// from the panel's top-right corner (over the canvas).
static constexpr int CLOSE_SIZE = 58;
static constexpr int CLOSE_GAP = 12;
static constexpr int CLOSE_MARGIN = 10;
static constexpr int CLOSE_HIT_PADDING = 10;

// Framebuffer transfers (cache write-back, PPA output) work in whole cache
// lines. 128 covers the ESP32-P4's 64- and 128-byte line settings.
static constexpr size_t CACHE_LINE = 128;

inline size_t align_bytes(size_t bytes) { return (bytes + CACHE_LINE - 1) / CACHE_LINE * CACHE_LINE; }

// `bytes` rounded up to whole cache lines, but never past the allocation.
inline size_t sync_bytes(size_t bytes, size_t allocated) { return std::min(align_bytes(bytes), allocated); }

inline size_t frame_bytes(int width, int height) {
  return static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint16_t);
}

// A canvas an app may use on a panel whose largest canvas is max_w x max_h:
// even sizes (the canvas is centred, and the panel turned 180 degrees), from
// MIN_WIDTH x MIN_HEIGHT up.
inline bool size_ok(int width, int height, int max_w, int max_h) {
  return width >= MIN_WIDTH && height >= MIN_HEIGHT && width <= max_w && height <= max_h && width <= MAX_SIDE &&
         height <= MAX_SIDE && width % 2 == 0 && height % 2 == 0;
}

// "1024x600" -> 1024, 600. Digits, an 'x' (or 'X'), digits; nothing else.
inline bool parse_size(const std::string &text, int *width, int *height) {
  const size_t x = text.find_first_of("xX");
  if (x == std::string::npos || x == 0 || x + 1 >= text.size() || x > 5 || text.size() - x - 1 > 5)
    return false;
  int values[2] = {0, 0};
  const std::string parts[2] = {text.substr(0, x), text.substr(x + 1)};
  for (int i = 0; i < 2; i++) {
    for (char c : parts[i]) {
      if (!std::isdigit(static_cast<unsigned char>(c)))
        return false;
      values[i] = values[i] * 10 + (c - '0');
    }
  }
  if (values[0] <= 0 || values[1] <= 0)
    return false;
  if (width != nullptr)
    *width = values[0];
  if (height != nullptr)
    *height = values[1];
  return true;
}

inline std::string format_size(int width, int height) {
  char text[24];
  std::snprintf(text, sizeof(text), "%dx%d", width, height);
  return text;
}

// Where the canvas and the close control are on a panel. "Screen"
// coordinates are the touch panel's (landscape, top-left origin). The panel's
// raw draw path is turned 180 degrees, so a rectangle at screen (x, y) is
// drawn at raw (panel_w - x - w, panel_h - y - h).
struct Geometry {
  int panel_w{FALLBACK_PANEL_WIDTH};
  int panel_h{FALLBACK_PANEL_HEIGHT};
  int canvas_w{LEGACY_WIDTH};
  int canvas_h{LEGACY_HEIGHT};

  int x() const { return (this->panel_w - this->canvas_w) / 2; }
  int y() const { return (this->panel_h - this->canvas_h) / 2; }
  int raw_x() const { return this->panel_w - this->x() - this->canvas_w; }
  int raw_y() const { return this->panel_h - this->y() - this->canvas_h; }

  // True when the close control fits in the margin right of the canvas.
  bool close_beside() const { return this->panel_w - (this->x() + this->canvas_w) >= CLOSE_GAP + CLOSE_SIZE; }
  int close_x() const {
    return this->close_beside() ? this->x() + this->canvas_w + CLOSE_GAP : this->panel_w - CLOSE_SIZE - CLOSE_MARGIN;
  }
  int close_y() const { return this->close_beside() ? this->y() + CLOSE_MARGIN : CLOSE_MARGIN; }
  int close_raw_x() const { return this->panel_w - this->close_x() - CLOSE_SIZE; }
  int close_raw_y() const { return this->panel_h - this->close_y() - CLOSE_SIZE; }

  // A touch on the close control, with some padding around it.
  bool in_close(int sx, int sy) const {
    return sx >= this->close_x() - CLOSE_HIT_PADDING && sx < this->close_x() + CLOSE_SIZE + CLOSE_HIT_PADDING &&
           sy >= this->close_y() - CLOSE_HIT_PADDING && sy < this->close_y() + CLOSE_SIZE + CLOSE_HIT_PADDING;
  }

  // Screen -> canvas coordinates; false outside the canvas.
  bool to_canvas(int sx, int sy, int *cx, int *cy) const {
    const int dx = sx - this->x(), dy = sy - this->y();
    if (dx < 0 || dy < 0 || dx >= this->canvas_w || dy >= this->canvas_h)
      return false;
    if (cx != nullptr)
      *cx = dx;
    if (cy != nullptr)
      *cy = dy;
    return true;
  }
};

// The per-app settings key: the file name without folders, query, ".papp" and
// a "-<version>" suffix, so a store download (psram_tulip-0.1.0.papp), an
// installed copy (/sd/roms/papp/psram_tulip.papp) and the listing's name
// (psram_tulip) agree.
inline std::string app_key(const std::string &path) {
  std::string name = path.substr(0, path.find_first_of("?#"));
  const size_t slash = name.find_last_of('/');
  if (slash != std::string::npos)
    name = name.substr(slash + 1);
  if (name.size() > 5) {
    std::string ext = name.substr(name.size() - 5);
    for (char &c : ext)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".papp")
      name.resize(name.size() - 5);
  }
  // A version is digits in dot-separated groups: 0.1.0, 2, 10.4.
  const size_t dash = name.rfind('-');
  if (dash != std::string::npos && dash > 0 && dash + 1 < name.size()) {
    bool version = true;
    bool digit = false;
    for (size_t i = dash + 1; i < name.size() && version; i++) {
      const char c = name[i];
      if (std::isdigit(static_cast<unsigned char>(c))) {
        digit = true;
      } else if (c == '.' && digit) {
        digit = false;
      } else {
        version = false;
      }
    }
    if (version && digit)
      name.resize(dash);
  }
  return name;
}

// The sizes the store's Screen setting offers (after "Default"), largest
// first, without repeats, each one that fits in max_w x max_h: the listing's
// own sizes when it names some, else the panel, 1024x600, 800x480 and 640x480.
inline std::vector<std::string> choices(int max_w, int max_h, const std::vector<std::string> &listed) {
  std::vector<std::pair<int, int>> sizes;
  if (listed.empty()) {
    sizes = {{max_w - max_w % 2, max_h - max_h % 2}, {1024, 600}, {800, 480}, {640, 480}};
  } else {
    for (const auto &text : listed) {
      int w = 0, h = 0;
      if (parse_size(text, &w, &h))
        sizes.emplace_back(w, h);
    }
  }
  std::stable_sort(sizes.begin(), sizes.end(), [](const std::pair<int, int> &a, const std::pair<int, int> &b) {
    return static_cast<long>(a.first) * a.second > static_cast<long>(b.first) * b.second;
  });
  std::vector<std::string> result;
  for (const auto &size : sizes) {
    if (!size_ok(size.first, size.second, max_w, max_h))
      continue;
    const std::string text = format_size(size.first, size.second);
    if (std::find(result.begin(), result.end(), text) == result.end())
      result.push_back(text);
  }
  return result;
}

}  // namespace canvas
}  // namespace papp_loader
}  // namespace esphome
