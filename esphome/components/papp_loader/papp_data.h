#pragma once

// App data lists: the files a store app needs on the card.
//
// The store publishes <name>-<version>.files next to <name>-<version>.papp:
//
//   # papp-data 1
//   # comment lines start with '#'
//   <size> <sha256> <target path under the data root> <http(s) URL>
//
// Everything here is plain C++ with no ESP-IDF dependency, so it can be unit
// tested on a host (tests/cpp/test_papp_data.cpp).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace esphome {
namespace papp_loader {
namespace data {

static constexpr const char *LIST_HEADER = "# papp-data 1";
static constexpr size_t MAX_FILES = 64;
static constexpr uint64_t MAX_FILE_SIZE = 1024ULL * 1024 * 1024;

struct DataFile {
  uint32_t size{0};
  std::string sha256;  // 64 lowercase hex digits
  std::string target;  // relative path under the data root
  std::string url;
};

// A target is a relative path of [A-Za-z0-9._-] segments separated by '/',
// with no empty, "." or ".." segment, so it can never leave the data root.
inline bool safe_target(const std::string &target) {
  if (target.empty() || target.size() > 200)
    return false;
  size_t start = 0;
  while (start <= target.size()) {
    size_t end = target.find('/', start);
    if (end == std::string::npos)
      end = target.size();
    const std::string segment = target.substr(start, end - start);
    if (segment.empty() || segment == "." || segment == "..")
      return false;
    for (char c : segment) {
      const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                      c == '_' || c == '-';
      if (!ok)
        return false;
    }
    start = end + 1;
  }
  return true;
}

inline bool is_sha256_hex(const std::string &value) {
  if (value.size() != 64)
    return false;
  for (char c : value) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

// ".../psram_doom-0.1.0.papp" -> ".../psram_doom-0.1.0.files"; empty when the
// URL does not name a .papp file. A query or fragment is dropped.
inline std::string list_url_for(const std::string &papp_url) {
  std::string url = papp_url.substr(0, papp_url.find_first_of("?#"));
  if (url.size() < 5)
    return {};
  std::string ext = url.substr(url.size() - 5);
  for (char &c : ext)
    c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  if (ext != ".papp")
    return {};
  return url.substr(0, url.size() - 5) + ".files";
}

// Parses a data list. On failure returns false and says why in *error.
inline bool parse_list(const std::string &text, std::vector<DataFile> *out, std::string *error) {
  out->clear();
  size_t pos = 0;
  size_t line_no = 0;
  bool header_seen = false;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos)
      end = text.size();
    std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    line_no++;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (!header_seen) {
      if (line != LIST_HEADER) {
        *error = "not a papp-data 1 list";
        return false;
      }
      header_seen = true;
      continue;
    }
    if (line.empty() || line[0] == '#')
      continue;

    std::vector<std::string> fields;
    size_t f = 0;
    while (f < line.size()) {
      size_t sp = line.find(' ', f);
      if (sp == std::string::npos)
        sp = line.size();
      if (sp > f)
        fields.push_back(line.substr(f, sp - f));
      f = sp + 1;
    }
    char where[32];
    std::snprintf(where, sizeof(where), "line %u: ", static_cast<unsigned>(line_no));
    if (fields.size() != 4) {
      *error = std::string(where) + "expected <size> <sha256> <target> <url>";
      return false;
    }
    uint64_t size = 0;
    for (char c : fields[0]) {
      if (c < '0' || c > '9' || size > MAX_FILE_SIZE) {
        *error = std::string(where) + "bad size";
        return false;
      }
      size = size * 10 + static_cast<uint64_t>(c - '0');
    }
    if (size == 0 || size > MAX_FILE_SIZE) {
      *error = std::string(where) + "bad size";
      return false;
    }
    if (!is_sha256_hex(fields[1])) {
      *error = std::string(where) + "bad sha256";
      return false;
    }
    if (!safe_target(fields[2])) {
      *error = std::string(where) + "unsafe target path";
      return false;
    }
    if (fields[3].rfind("https://", 0) != 0 && fields[3].rfind("http://", 0) != 0) {
      *error = std::string(where) + "URL must be http(s)";
      return false;
    }
    for (const auto &existing : *out) {
      if (existing.target == fields[2]) {
        *error = std::string(where) + "target listed twice";
        return false;
      }
    }
    if (out->size() >= MAX_FILES) {
      *error = "too many files";
      return false;
    }
    out->push_back(DataFile{static_cast<uint32_t>(size), fields[1], fields[2], fields[3]});
  }
  if (!header_seen) {
    *error = "empty list";
    return false;
  }
  return true;
}

// Small self-contained SHA-256 (FIPS 180-4), so the check does not depend on
// which mbedTLS/PSA API the ESP-IDF version exposes.
class Sha256 {
 public:
  Sha256() { this->reset(); }

  void reset() {
    static const uint32_t INIT[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(this->h_, INIT, sizeof(INIT));
    this->fill_ = 0;
    this->total_ = 0;
  }

  void update(const void *data, size_t length) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    this->total_ += length;
    if (this->fill_ != 0) {
      const size_t take = length < 64 - this->fill_ ? length : 64 - this->fill_;
      std::memcpy(this->buffer_ + this->fill_, bytes, take);
      this->fill_ += take;
      bytes += take;
      length -= take;
      if (this->fill_ < 64)
        return;
      this->block_(this->buffer_);
      this->fill_ = 0;
    }
    while (length >= 64) {
      this->block_(bytes);
      bytes += 64;
      length -= 64;
    }
    std::memcpy(this->buffer_, bytes, length);
    this->fill_ = length;
  }

  // Finishes the hash and returns it as 64 lowercase hex digits.
  std::string hex() {
    const uint64_t bits = this->total_ * 8;
    uint8_t pad[72] = {0x80};
    const size_t pad_len = (this->fill_ < 56 ? 56 : 120) - this->fill_;
    this->update(pad, pad_len);
    uint8_t length_be[8];
    for (int i = 0; i < 8; i++)
      length_be[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    this->update(length_be, 8);
    static const char *const DIGITS = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 8; i++) {
      for (int j = 0; j < 8; j++)
        out[i * 8 + j] = DIGITS[(this->h_[i] >> (28 - 4 * j)) & 0xF];
    }
    return out;
  }

 private:
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

  void block_(const uint8_t *p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      w[i] = (static_cast<uint32_t>(p[4 * i]) << 24) | (static_cast<uint32_t>(p[4 * i + 1]) << 16) |
             (static_cast<uint32_t>(p[4 * i + 2]) << 8) | static_cast<uint32_t>(p[4 * i + 3]);
    }
    for (int i = 16; i < 64; i++) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = this->h_[0], b = this->h_[1], c = this->h_[2], d = this->h_[3];
    uint32_t e = this->h_[4], f = this->h_[5], g = this->h_[6], h = this->h_[7];
    for (int i = 0; i < 64; i++) {
      const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    this->h_[0] += a;
    this->h_[1] += b;
    this->h_[2] += c;
    this->h_[3] += d;
    this->h_[4] += e;
    this->h_[5] += f;
    this->h_[6] += g;
    this->h_[7] += h;
  }

  uint32_t h_[8];
  uint8_t buffer_[64];
  size_t fill_{0};
  uint64_t total_{0};
};

}  // namespace data
}  // namespace papp_loader
}  // namespace esphome
