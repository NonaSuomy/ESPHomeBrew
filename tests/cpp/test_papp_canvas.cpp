// Host tests for esphome/components/papp_loader/papp_canvas.h.
//
//   g++ -std=gnu++17 -Wall -Wextra -Werror -I esphome/components/papp_loader
//       tests/cpp/test_papp_canvas.cpp -o /tmp/test_papp_canvas && /tmp/test_papp_canvas

#include "papp_canvas.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace esphome::papp_loader::canvas;

static int failures = 0;

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                     \
    }                                                                 \
  } while (0)

static Geometry make(int panel_w, int panel_h, int canvas_w, int canvas_h) {
  Geometry g;
  g.panel_w = panel_w;
  g.panel_h = panel_h;
  g.canvas_w = canvas_w;
  g.canvas_h = canvas_h;
  return g;
}

// The 800x480 canvas on the 1024x600 panel must land exactly where the loader
// always put it (the old compile-time constants).
static void test_legacy_layout() {
  const Geometry g;  // defaults: 800x480 on 1024x600
  CHECK(g.x() == 112 && g.y() == 60);
  CHECK(g.raw_x() == 112 && g.raw_y() == 60);
  CHECK(g.close_beside());
  CHECK(g.close_x() == 924 && g.close_y() == 70);        // LCD_X_OFFSET + 800 + 12, LCD_Y_OFFSET + 10
  CHECK(g.close_raw_x() == 42 && g.close_raw_y() == 472);  // 1024 - 924 - 58, 600 - 70 - 58
  CHECK(g.in_close(914, 60) && g.in_close(991, 137));
  CHECK(!g.in_close(913, 60) && !g.in_close(992, 100) && !g.in_close(950, 138));
  int x = -1, y = -1;
  CHECK(g.to_canvas(112, 60, &x, &y) && x == 0 && y == 0);
  CHECK(g.to_canvas(911, 539, &x, &y) && x == 799 && y == 479);
  CHECK(!g.to_canvas(111, 60, &x, &y) && !g.to_canvas(912, 60, &x, &y) && !g.to_canvas(500, 540, nullptr, nullptr));
  CHECK(g.to_canvas(500, 300, nullptr, nullptr));
}

static void test_full_panel_and_small_canvases() {
  const Geometry full = make(1024, 600, 1024, 600);
  CHECK(full.x() == 0 && full.y() == 0 && full.raw_x() == 0 && full.raw_y() == 0);
  CHECK(!full.close_beside());
  CHECK(full.close_x() == 1024 - 58 - 10 && full.close_y() == 10);  // over the canvas, top-right
  CHECK(full.close_raw_x() == 10 && full.close_raw_y() == 600 - 10 - 58);
  int x = 0, y = 0;
  CHECK(full.to_canvas(1023, 599, &x, &y) && x == 1023 && y == 599);
  CHECK(!full.to_canvas(1024, 0, &x, &y));

  const Geometry vga = make(1024, 600, 640, 480);
  CHECK(vga.x() == 192 && vga.y() == 60 && vga.raw_x() == 192 && vga.raw_y() == 60);
  CHECK(vga.close_beside() && vga.close_x() == 192 + 640 + 12 && vga.close_y() == 70);

  // Too little room beside the canvas: over its corner instead.
  const Geometry wide = make(1024, 600, 960, 540);
  CHECK(!wide.close_beside() && wide.close_x() == 956 && wide.close_y() == 10);

  // An odd panel: the raw corner is the true 180-degree counterpart.
  const Geometry odd = make(1025, 601, 800, 480);
  CHECK(odd.x() == 112 && odd.y() == 60 && odd.raw_x() == 113 && odd.raw_y() == 61);
  // Each close position stays on the panel.
  for (const Geometry &g : {Geometry(), full, vga, wide, odd, make(800, 480, 800, 480), make(1280, 720, 1280, 720)}) {
    CHECK(g.close_x() >= 0 && g.close_x() + CLOSE_SIZE <= g.panel_w);
    CHECK(g.close_y() >= 0 && g.close_y() + CLOSE_SIZE <= g.panel_h);
    CHECK(g.close_raw_x() >= 0 && g.close_raw_y() >= 0);
  }
}

static void test_sizes() {
  CHECK(size_ok(800, 480, 1024, 600));
  CHECK(size_ok(1024, 600, 1024, 600));
  CHECK(size_ok(320, 240, 1024, 600));
  CHECK(!size_ok(1026, 600, 1024, 600));
  CHECK(!size_ok(1024, 602, 1024, 600));
  CHECK(!size_ok(801, 480, 1024, 600));
  CHECK(!size_ok(800, 481, 1024, 600));
  CHECK(!size_ok(318, 240, 1024, 600));
  CHECK(!size_ok(320, 238, 1024, 600));
  CHECK(!size_ok(0, 0, 1024, 600));
  CHECK(!size_ok(-800, 480, 1024, 600));
  CHECK(!size_ok(1024, 600, 800, 480));  // buffers only for 800x480

  int w = 0, h = 0;
  CHECK(parse_size("1024x600", &w, &h) && w == 1024 && h == 600);
  CHECK(parse_size("640X480", &w, &h) && w == 640 && h == 480);
  CHECK(parse_size("800x480", nullptr, nullptr));
  for (const char *bad : {"", "x", "1024", "1024x", "x600", "1024x600x2", "1024 x 600", " 1024x600", "1024x600 ",
                          "-1x5", "0x480", "800x0", "123456x10", "10x123456", "axb", "1024,600"}) {
    CHECK(!parse_size(bad, &w, &h));
  }
  CHECK(format_size(1024, 600) == "1024x600");
}

static void test_bytes() {
  CHECK(frame_bytes(800, 480) == 768000);
  CHECK(frame_bytes(1024, 600) == 1228800);
  CHECK(align_bytes(768000) == 768000);  // already whole 128-byte lines
  CHECK(align_bytes(1) == 128 && align_bytes(128) == 128 && align_bytes(129) == 256);
  CHECK(sync_bytes(768000, 1228800) == 768000);
  CHECK(sync_bytes(frame_bytes(642, 482), 1228800) == align_bytes(642 * 482 * 2));
  CHECK(sync_bytes(1228800 - 1, 1228800) == 1228800);
  CHECK(sync_bytes(2000000, 1228800) == 1228800);  // never past the buffer
}

static void test_app_keys() {
  CHECK(app_key("https://nonasuomy.github.io/papp-conversions/psram_tulip-0.1.0.papp") == "psram_tulip");
  CHECK(app_key("/sdcard/roms/papp/psram_tulip.papp") == "psram_tulip");
  CHECK(app_key("/sd/roms/papp/psram_tulip.PAPP") == "psram_tulip");
  CHECK(app_key("http://lan:8000/doom.papp?x=1#y") == "doom");
  CHECK(app_key("psram_doom-2.papp") == "psram_doom");
  CHECK(app_key("a-1-2.papp") == "a-1");  // like make_listing's split_stem
  CHECK(app_key("azip-1.2") == "azip");
  CHECK(app_key("ESP32_P4_PAPP.papp") == "ESP32_P4_PAPP");
  CHECK(app_key("half-life.papp") == "half-life");
  CHECK(app_key("game-1..2.papp") == "game-1..2");
  CHECK(app_key("game-1.papp.papp") == "game-1.papp");
  CHECK(app_key("game-.papp") == "game-");
  CHECK(app_key("-1.papp") == "-1");
  CHECK(app_key("x-1.0.") == "x-1.0.");
  CHECK(app_key(".papp") == ".papp");
  CHECK(app_key("") == "");
}

static void test_choices() {
  const std::vector<std::string> none;
  CHECK((choices(1024, 600, none) == std::vector<std::string>{"1024x600", "800x480", "640x480"}));
  CHECK((choices(1280, 720, none) == std::vector<std::string>{"1280x720", "1024x600", "800x480", "640x480"}));
  CHECK((choices(800, 480, none) == std::vector<std::string>{"800x480", "640x480"}));
  CHECK((choices(1025, 601, none) == std::vector<std::string>{"1024x600", "800x480", "640x480"}));
  // The listing's own sizes: those that fit, largest first, once each.
  CHECK((choices(1024, 600, {"640x480", "1280x720", "1024x600", "800x480", "800x480", "junk", "801x480"}) ==
         std::vector<std::string>{"1024x600", "800x480", "640x480"}));
  CHECK(choices(800, 480, {"1024x600"}).empty());
}

// The store's Screen button with and without a recommended size.
static void test_recommended() {
  const std::vector<std::string> none;
  const std::vector<std::string> listed{"800x480", "1024x600"};
  CHECK(recommended(1024, 600, listed, "800x480") == "800x480");
  CHECK(recommended(1024, 600, listed, "800X480") == "800x480");
  CHECK(recommended(1024, 600, listed, "640x480").empty());  // not one of its sizes
  CHECK(recommended(800, 480, {"1024x600"}, "1024x600").empty());  // does not fit
  CHECK(recommended(1024, 600, listed, "").empty());
  CHECK(recommended(1024, 600, listed, "junk").empty());
  CHECK(recommended(1024, 600, none, "960x540") == "960x540");  // any size: any that fits
  CHECK(recommended(1024, 600, none, "961x540").empty());
  CHECK(recommended(1024, 600, none, "1280x720").empty());

  CHECK((offered(1024, 600, listed, "800x480") == std::vector<std::string>{"1024x600", "800x480"}));
  CHECK((offered(1024, 600, none, "") == std::vector<std::string>{"1024x600", "800x480", "640x480"}));
  CHECK((offered(1024, 600, none, "960x540") ==
         std::vector<std::string>{"1024x600", "960x540", "800x480", "640x480"}));

  // No recommendation: Default first, as before.
  const std::vector<std::string> plain = screen_options(offered(1024, 600, listed, ""), "");
  CHECK((plain == std::vector<std::string>{"", "1024x600", "800x480"}));
  CHECK(next_screen_option(plain, "") == "1024x600");
  CHECK(next_screen_option(plain, "1024x600") == "800x480");
  CHECK(next_screen_option(plain, "800x480").empty());
  CHECK(next_screen_option(plain, "640x480").empty());  // no longer offered: back to Default

  // Recommended 800x480: "" (no setting) is 800x480, in its place; no Default.
  const std::vector<std::string> rec = screen_options(offered(1024, 600, listed, "800x480"), "800x480");
  CHECK((rec == std::vector<std::string>{"1024x600", ""}));
  CHECK(next_screen_option(rec, "") == "1024x600");
  CHECK(next_screen_option(rec, "1024x600").empty());
  CHECK(next_screen_option(rec, "1280x720").empty());
  CHECK((screen_options({"800x480"}, "800x480") == std::vector<std::string>{""}));
  CHECK(next_screen_option({""}, "").empty());
  CHECK(next_screen_option({}, "800x480").empty());
}

int main() {
  test_recommended();
  test_legacy_layout();
  test_full_panel_and_small_canvases();
  test_sizes();
  test_bytes();
  test_app_keys();
  test_choices();
  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("papp_canvas.h: all checks passed\n");
  return 0;
}
