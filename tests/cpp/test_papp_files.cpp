// Host tests for esphome/components/papp_loader/papp_files.h.
//
//   g++ -std=gnu++17 -Wall -Wextra -Werror -I esphome/components/papp_loader
//       tests/cpp/test_papp_files.cpp -o /tmp/test_papp_files && /tmp/test_papp_files

#include "papp_files.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace esphome::papp_loader::files;

static int failures = 0;

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                     \
    }                                                                 \
  } while (0)

static const std::vector<std::string> ROOTS{"/sd", "/usb0"};

static bool ok(const char *path, std::string *clean = nullptr, std::string *root = nullptr) {
  return clean_app_path(path, ROOTS, clean, root);
}

// What the file services accept: the card and the data roots, nothing that
// could climb out of them.
static void test_app_paths() {
  std::string clean, root;
  CHECK(ok("/sd/roms/x.zip", &clean, &root) && clean == "/sd/roms/x.zip" && root == "/sd");
  CHECK(ok("/usb0/a/b/", &clean, &root) && clean == "/usb0/a/b" && root == "/usb0");
  CHECK(ok("/sd", &clean, &root) && clean == "/sd" && root == "/sd");
  CHECK(ok("/sd/", &clean) && clean == "/sd");
  CHECK(ok("/sd/My Games/Tomb Raider (1996)/DATA", &clean));
  CHECK(ok("/sd/.hidden"));  // a dot file is a name, not "."

  CHECK(!ok(nullptr));
  CHECK(!ok(""));
  CHECK(!ok("sd/x"));
  CHECK(!ok("/"));
  CHECK(!ok("/sdcard/x"));  // the VFS path, not the app's
  CHECK(!ok("/sdx/x"));
  CHECK(!ok("/usb1/x"));
  CHECK(!ok("/spiffs/x"));
  CHECK(!ok("/sd/../etc/passwd"));
  CHECK(!ok("/sd/roms/.."));
  CHECK(!ok("/sd/roms/../../x"));
  CHECK(!ok("/sd/./x"));
  CHECK(!ok("/sd//x"));
  CHECK(!ok("/sd/a\\..\\b"));
  CHECK(!ok("/sd/0:/x"));
  CHECK(!ok("/sd/a\nb"));
  CHECK(!ok("/sd/..hidden/../x"));
  CHECK(ok("/sd/..hidden"));  // only a whole ".." part climbs
  CHECK(!ok(("/sd/" + std::string(300, 'x')).c_str()));

  // Nested roots: the shortest (the mount point) is the root.
  const std::vector<std::string> nested{"/sd/games", "/sd", "/usb0/"};
  CHECK(clean_app_path("/sd/games/x", nested, &clean, &root) && root == "/sd");
  CHECK(!clean_app_path("/usb0/x", nested, &clean, &root));  // "/usb0/" is not a usable root
}

static void test_glob() {
  CHECK(glob_match("*.PHD", "LEVEL1.PHD"));
  CHECK(glob_match("*.phd", "level1.PHD"));
  CHECK(glob_match("pak?.pak", "PAK0.PAK"));
  CHECK(!glob_match("pak?.pak", "pak10.pak"));
  CHECK(glob_match("*", ""));
  CHECK(glob_match("*", "anything"));
  CHECK(glob_match("a*b*c", "aXXbYYc"));
  CHECK(glob_match("a*b*c", "abc"));
  CHECK(!glob_match("a*b*c", "aXXbYY"));
  CHECK(glob_match("*.tar.gz", "x.y.tar.gz"));
  CHECK(!glob_match("*.tar.gz", "x.tar.gzz"));
  CHECK(glob_match("doom1.wad", "DOOM1.WAD"));
  CHECK(!glob_match("doom1.wad", "doom1.wa"));
  CHECK(!glob_match("", "x"));
  CHECK(glob_match("", ""));
}

static void test_required() {
  Required r;
  CHECK(parse_required("/sd/roms/doom/doom1.wad", &r) && r.folder == "roms/doom" && r.name == "doom1.wad" &&
        !r.is_dir && !r.is_glob && r.relative() == "roms/doom/doom1.wad");
  CHECK(parse_required("/sd/roms/openlara/FMV/", &r) && r.folder == "roms/openlara" && r.name == "FMV" && r.is_dir &&
        !r.is_glob);
  CHECK(parse_required("/sd/roms/openlara/DATA/*.PHD", &r) && r.folder == "roms/openlara/DATA" && r.name == "*.PHD" &&
        r.is_glob && !r.is_dir);
  CHECK(parse_required("/sd/boot.cfg", &r) && r.folder.empty() && r.name == "boot.cfg" && r.relative() == "boot.cfg");
  CHECK(parse_required("/sd/roms/*/", &r) && r.is_dir && r.is_glob && r.folder == "roms");
  CHECK(parse_required("/sd/x", nullptr));

  CHECK(!parse_required("", &r));
  CHECK(!parse_required("/sd/", &r));
  CHECK(!parse_required("/sd//", &r));
  CHECK(!parse_required("/usb0/x", &r));
  CHECK(!parse_required("roms/x", &r));
  CHECK(!parse_required("/sd/../x", &r));
  CHECK(!parse_required("/sd/a/../x", &r));
  CHECK(!parse_required("/sd/a//x", &r));
  CHECK(!parse_required("/sd/*/x.wad", &r));  // patterns only in the last part
  CHECK(!parse_required("/sd/a\\x", &r));
  CHECK(!parse_required("/sd/c:x", &r));
}

static void test_parent_folders() {
  CHECK((parent_folders("roms/doom/doom1.wad") == std::vector<std::string>{"roms/doom"}));
  CHECK((parent_folders("roms/quake/id1/pak0.pak") == std::vector<std::string>{"roms/quake/id1", "roms/quake"}));
  CHECK(parent_folders("roms/x.wad").empty());  // never a top-level folder
  CHECK(parent_folders("x.wad").empty());
}

int main() {
  test_app_paths();
  test_glob();
  test_required();
  test_parent_folders();
  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("papp_files.h: all checks passed\n");
  return 0;
}
