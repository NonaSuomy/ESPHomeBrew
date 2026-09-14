// Host tests for esphome/components/papp_loader/papp_handoff.h.
//
//   g++ -std=gnu++17 -Wall -Wextra -Werror -I esphome/components/papp_loader
//       tests/cpp/test_papp_handoff.cpp -o /tmp/test_papp_handoff && /tmp/test_papp_handoff

#include "papp_handoff.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace esphome::papp_loader::handoff;

static int failures = 0;

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                     \
    }                                                                 \
  } while (0)

static void test_classify() {
  CHECK(classify("psram_video") == Target::NAME);
  CHECK(classify("psram_netsurf") == Target::NAME);
  CHECK(classify("a") == Target::NAME);
  CHECK(classify("app-2.v1") == Target::NAME);
  CHECK(classify("") == Target::INVALID);
  CHECK(classify("-video") == Target::INVALID);
  CHECK(classify(".video") == Target::INVALID);
  CHECK(classify("a..b") == Target::INVALID);
  CHECK(classify("video player") == Target::INVALID);
  CHECK(classify("psram_video\n") == Target::INVALID);
  CHECK(classify("roms/psram_video") == Target::INVALID);
  CHECK(classify(std::string(64, 'a')) == Target::NAME);
  CHECK(classify(std::string(65, 'a')) == Target::INVALID);

  CHECK(classify("http://192.168.1.5:8080/psram_video.papp") == Target::URL);
  CHECK(classify("https://example.github.io/store/psram_video-0.1.0.papp") == Target::URL);
  CHECK(classify("HTTPS://example.com/X.PAPP?download=1") == Target::URL);
  CHECK(classify("https://example.com/psram_video.papp#top") == Target::URL);
  CHECK(classify("https://example.com/") == Target::INVALID);
  CHECK(classify("https://example.com") == Target::INVALID);
  CHECK(classify("https:///x.papp") == Target::INVALID);
  CHECK(classify("https://example.com/video.mp4") == Target::INVALID);
  CHECK(classify("https://example.com/.papp") == Target::INVALID);
  CHECK(classify("ftp://example.com/x.papp") == Target::INVALID);
  CHECK(classify("https://example.com/" + std::string(250, 'a') + ".papp") == Target::INVALID);  // too long

  CHECK(classify("/sd/roms/papp/psram_video.papp") == Target::PATH);
  CHECK(classify("/usb0/papp/psram_video-0.1.0.papp") == Target::PATH);
  CHECK(classify("/sd/roms/papp/") == Target::INVALID);
  CHECK(classify("/sd/../etc/x.papp") == Target::INVALID);
  CHECK(classify("/sd/./x.papp") == Target::INVALID);
  CHECK(classify("/sd/x.papp?y") == Target::INVALID);
  CHECK(classify("/sd/videos/clip.mp4") == Target::INVALID);
}

static void test_names_and_versions() {
  CHECK(papp_stem("https://h/apps/psram_video-0.1.0.papp?x=1") == "psram_video-0.1.0");
  CHECK(papp_stem("/sd/roms/papp/psram_video.papp") == "psram_video");
  CHECK(version_of("https://h/psram_video-0.1.0.papp") == "0.1.0");
  CHECK(version_of("/sd/roms/papp/psram_video.papp").empty());
  CHECK(version_of("/sd/roms/papp/psram-video.papp").empty());
  CHECK(version_of("/sd/x/psram_video-12.papp") == "12");

  CHECK(compare_versions("0.1.0", "0.1.0") == 0);
  CHECK(compare_versions("0.1.0", "0.1") == 0);
  CHECK(compare_versions("0.10.0", "0.9.9") == 1);
  CHECK(compare_versions("0.9.9", "0.10.0") == -1);
  CHECK(compare_versions("1", "0.99") == 1);
  CHECK(compare_versions("", "0.1") == -1);
  CHECK(compare_versions("0.1", "") == 1);
  CHECK(compare_versions("", "") == 0);
}

static void test_best_match() {
  const std::vector<std::string> catalog = {
      "https://h/store/psram_doom-0.1.1.papp",
      "https://h/store/psram_video-0.1.0.papp",
      "https://h/store/psram_video-0.10.0.papp",
      "https://h/store/psram_video-0.9.2.papp",
      "https://h/store/psram_videos-1.0.0.papp",
      "https://h/store/psram_video.json",
  };
  CHECK(best_match(catalog, "psram_video") == "https://h/store/psram_video-0.10.0.papp");
  CHECK(best_match(catalog, "psram_doom") == "https://h/store/psram_doom-0.1.1.papp");
  CHECK(best_match(catalog, "psram_videos") == "https://h/store/psram_videos-1.0.0.papp");
  CHECK(best_match(catalog, "psram_quake").empty());
  CHECK(best_match({}, "psram_video").empty());

  // A folder with an unversioned copy and a versioned one: the version wins.
  CHECK(best_match({"/sd/papp/psram_video.papp", "/sd/papp/psram_video-0.2.0.papp"}, "psram_video") ==
        "/sd/papp/psram_video-0.2.0.papp");
  CHECK(best_match({"/sd/papp/psram_video.papp"}, "psram_video") == "/sd/papp/psram_video.papp");
  // Case matters in app names, as in the store's listings.
  CHECK(best_match({"/sd/papp/PSRAM_VIDEO.papp"}, "psram_video").empty());
}

static void test_copy_arg() {
  char buf[8];
  std::memset(buf, 'x', sizeof(buf));
  CHECK(copy_arg("", buf, sizeof(buf)) == 0 && buf[0] == '\0');
  CHECK(copy_arg("abc", buf, sizeof(buf)) == 3 && std::strcmp(buf, "abc") == 0);
  CHECK(copy_arg("0123456789", buf, sizeof(buf)) == 10 && std::strcmp(buf, "0123456") == 0);
  CHECK(copy_arg("abc", nullptr, 0) == 3);
  std::memset(buf, 'x', sizeof(buf));
  CHECK(copy_arg("abc", buf, 1) == 3 && buf[0] == '\0' && buf[1] == 'x');
  CHECK(copy_arg("abc", buf, 0) == 3 && buf[0] == '\0');  // len 0: buf untouched
}

static void test_chain_return() {
  Chain chain;
  Frame next;
  CHECK(!chain.pending() && chain.depth() == 0 && !chain.next(&next));

  // NetSurf opens the video player and wants its page back.
  CHECK(chain.request({"/sd/roms/papp/psram_netsurf.papp", "http://site/page.html"},
                      {"/sd/roms/papp/psram_video.papp", "http://site/clip.mp4"}, true));
  CHECK(chain.pending());
  CHECK(!chain.request({"a", ""}, {"b", ""}, false));  // one request at a time
  CHECK(chain.next(&next));
  CHECK(next.source == "/sd/roms/papp/psram_video.papp" && next.arg == "http://site/clip.mp4");
  CHECK(!chain.pending() && chain.depth() == 1);

  // The player quits: NetSurf again, with its page.
  CHECK(chain.next(&next));
  CHECK(next.source == "/sd/roms/papp/psram_netsurf.papp" && next.arg == "http://site/page.html");
  CHECK(chain.depth() == 0);
  // NetSurf quits: back to the menu.
  CHECK(!chain.next(&next));
}

static void test_chain_no_return_and_depth() {
  Chain chain;
  Frame next;
  // Without return_after the caller is not remembered.
  CHECK(chain.request({"A", "a"}, {"B", "b"}, false));
  CHECK(chain.next(&next) && next.source == "B" && next.arg == "b");
  CHECK(chain.depth() == 0 && !chain.next(&next));

  // A -> B -> C, each coming back; a third level is refused.
  CHECK(chain.request({"A", "a"}, {"B", "b"}, true));
  CHECK(chain.next(&next) && next.source == "B");
  CHECK(chain.request({"B", "b2"}, {"C", "c"}, true));
  CHECK(chain.next(&next) && next.source == "C" && chain.depth() == 2);
  CHECK(!chain.request({"C", ""}, {"D", ""}, true));
  // Handing over without coming back still works at full depth: D replaces C.
  CHECK(chain.request({"C", ""}, {"D", "d"}, false));
  CHECK(chain.next(&next) && next.source == "D" && chain.depth() == 2);
  // D quits: B with its resume argument, then A.
  CHECK(chain.next(&next) && next.source == "B" && next.arg == "b2");
  CHECK(chain.next(&next) && next.source == "A" && next.arg == "a");
  CHECK(!chain.next(&next));
}

static void test_chain_failed_launch_and_reset() {
  Chain chain;
  Frame next;
  // The target fails to load: the loader asks for the next launch at once,
  // which is the caller again.
  CHECK(chain.request({"A", "page"}, {"missing", "x"}, true));
  CHECK(chain.next(&next) && next.source == "missing");
  CHECK(chain.next(&next) && next.source == "A" && next.arg == "page");
  CHECK(!chain.next(&next));

  // A menu launch forgets everything.
  CHECK(chain.request({"A", ""}, {"B", ""}, true));
  CHECK(chain.next(&next));
  CHECK(chain.request({"B", ""}, {"C", ""}, true));
  chain.reset();
  CHECK(!chain.pending() && chain.depth() == 0 && !chain.next(&next));
}

int main() {
  test_classify();
  test_names_and_versions();
  test_best_match();
  test_copy_arg();
  test_chain_return();
  test_chain_no_return_and_depth();
  test_chain_failed_launch_and_reset();
  if (failures == 0)
    std::printf("papp_handoff.h: all tests passed\n");
  return failures == 0 ? 0 : 1;
}
