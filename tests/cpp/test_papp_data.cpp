// Host tests for esphome/components/papp_loader/papp_data.h.
//
//   g++ -std=gnu++17 -Wall -Wextra -Werror -I esphome/components/papp_loader
//       tests/cpp/test_papp_data.cpp -o /tmp/test_papp_data && /tmp/test_papp_data

#include "papp_data.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace esphome::papp_loader::data;

static int failures = 0;

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                     \
    }                                                                 \
  } while (0)

static std::string sha(const std::string &text) {
  Sha256 h;
  h.update(text.data(), text.size());
  return h.hex();
}

static void test_sha256() {
  CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  const std::string million(1000000, 'a');
  CHECK(sha(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // Same result however the input is split (covers every buffer offset).
  for (size_t step : {1u, 3u, 55u, 56u, 63u, 64u, 65u, 1000u, 16384u}) {
    Sha256 h;
    for (size_t i = 0; i < million.size(); i += step)
      h.update(million.data() + i, std::min(step, million.size() - i));
    CHECK(h.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  }
  for (size_t n = 50; n < 140; n++) {  // lengths around the padding boundaries
    Sha256 one, split;
    const std::string s(n, 'x');
    one.update(s.data(), n);
    split.update(s.data(), n / 2);
    split.update(s.data() + n / 2, n - n / 2);
    CHECK(one.hex() == split.hex());
  }
}

static void test_targets() {
  CHECK(safe_target("roms/doom/doom1.wad"));
  CHECK(safe_target("roms/quake/id1/pak0.pak"));
  CHECK(safe_target("a"));
  CHECK(!safe_target(""));
  CHECK(!safe_target("/roms/doom.wad"));
  CHECK(!safe_target("roms/../etc"));
  CHECK(!safe_target("roms/./x"));
  CHECK(!safe_target("roms//x"));
  CHECK(!safe_target("roms/x/"));
  CHECK(!safe_target("roms\\x"));
  CHECK(!safe_target("roms/a b"));
  CHECK(!safe_target(".."));
}

static void test_list_url() {
  CHECK(list_url_for("https://h/p/psram_doom-0.1.0.papp") == "https://h/p/psram_doom-0.1.0.files");
  CHECK(list_url_for("https://h/p/App.PAPP?x=1") == "https://h/p/App.files");
  CHECK(list_url_for("https://h/p/readme.txt").empty());
  CHECK(list_url_for("").empty());
}

static const char *const SHA_A = "1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771";

static void test_parse() {
  std::vector<DataFile> files;
  std::string error;
  const std::string good = std::string("# papp-data 1\r\n# psram_doom 0.1.0: shareware\n\n4196020 ") + SHA_A +
                           " roms/doom/doom1.wad https://h/data/psram_doom/roms/doom/doom1.wad\n";
  CHECK(parse_list(good, &files, &error));
  CHECK(files.size() == 1);
  CHECK(files[0].size == 4196020);
  CHECK(files[0].target == "roms/doom/doom1.wad");
  CHECK(files[0].url == "https://h/data/psram_doom/roms/doom/doom1.wad");

  const std::string line = std::string("4 ") + SHA_A + " roms/x https://h/x\n";
  const std::vector<std::string> bad = {
      "",
      "papp-data 1\n",
      std::string("# papp-data 2\n") + line,
      "# papp-data 1\n4 " + std::string(SHA_A) + " roms/x\n",
      "# papp-data 1\n0 " + std::string(SHA_A) + " roms/x https://h/x\n",
      "# papp-data 1\n99999999999 " + std::string(SHA_A) + " roms/x https://h/x\n",
      "# papp-data 1\n4x " + std::string(SHA_A) + " roms/x https://h/x\n",
      "# papp-data 1\n4 ABC roms/x https://h/x\n",
      "# papp-data 1\n4 " + std::string(SHA_A) + " ../x https://h/x\n",
      "# papp-data 1\n4 " + std::string(SHA_A) + " roms/x ftp://h/x\n",
      "# papp-data 1\n" + line + line,
  };
  for (const auto &text : bad) {
    error.clear();
    CHECK(!parse_list(text, &files, &error));
    CHECK(!error.empty());
  }
}

int main() {
  test_sha256();
  test_targets();
  test_list_url();
  test_parse();
  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("papp_data: all checks passed\n");
  return 0;
}
