#pragma once

// Paths for the loader's file services and the store's file checks.
//
// - clean_app_path: which paths an app may create, delete, rename or stat
//   (psram_app.h file_mkdir / file_remove / file_rename / file_stat).
// - parse_required: a listing's "requires" path (/sd/roms/x/FILE, a folder
//   ending in '/', or a '*' / '?' pattern in the last part), which the store
//   looks for under every data root.
// - glob_match: those patterns, without regard to case (FAT ignores it too).
//
// Everything here is plain C++ with no ESP-IDF dependency, so it can be unit
// tested on a host (tests/cpp/test_papp_files.cpp).

#include <cstddef>
#include <string>
#include <vector>

namespace esphome {
namespace papp_loader {
namespace files {

static constexpr size_t MAX_PATH = 255;

// One '/'-separated part of a path an app or a listing may use: not empty,
// not "." or "..", and no backslash (FatFs also splits on it), colon (a
// FatFs drive prefix) or control character.
inline bool safe_segment(const std::string &segment) {
  if (segment.empty() || segment == "." || segment == "..")
    return false;
  for (char c : segment) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F || c == '\\' || c == ':')
      return false;
  }
  return true;
}

// Checks an app's path for the file services and returns it without a
// trailing '/' in *clean. It must be one of `roots` (/sd, /usb0, ...) or lie
// under one, with safe parts only. *root gets that root (the shortest one
// when roots nest, as that is the storage's mount point).
inline bool clean_app_path(const char *path, const std::vector<std::string> &roots, std::string *clean,
                           std::string *root = nullptr) {
  if (path == nullptr || path[0] != '/')
    return false;
  std::string value(path);
  if (value.size() > MAX_PATH)
    return false;
  while (value.size() > 1 && value.back() == '/')
    value.pop_back();
  size_t start = 1;
  while (start <= value.size()) {
    size_t end = value.find('/', start);
    if (end == std::string::npos)
      end = value.size();
    if (!safe_segment(value.substr(start, end - start)))
      return false;
    start = end + 1;
  }
  const std::string *match = nullptr;
  for (const auto &candidate : roots) {
    if (candidate.size() < 2 || candidate[0] != '/' || candidate.back() == '/')
      continue;
    if ((value == candidate || value.compare(0, candidate.size() + 1, candidate + "/") == 0) &&
        (match == nullptr || candidate.size() < match->size()))
      match = &candidate;
  }
  if (match == nullptr)
    return false;
  if (clean != nullptr)
    *clean = value;
  if (root != nullptr)
    *root = *match;
  return true;
}

// '*' (any run of characters) and '?' (one character), ignoring case.
inline bool glob_match(const char *pattern, const char *name) {
  const char *star = nullptr;  // the last '*' seen, and where its run ends in name
  const char *resume = nullptr;
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
  while (*name != '\0') {
    if (*pattern == '*') {
      star = pattern++;
      resume = name;
    } else if (*pattern == '?' || (*pattern != '\0' && lower(*pattern) == lower(*name))) {
      pattern++;
      name++;
    } else if (star != nullptr) {
      pattern = star + 1;
      name = ++resume;
    } else {
      return false;
    }
  }
  while (*pattern == '*')
    pattern++;
  return *pattern == '\0';
}

inline bool has_glob(const std::string &text) { return text.find_first_of("*?") != std::string::npos; }

// A listing's "requires" path, as an app sees it: /sd/<relative path>, where
// a trailing '/' names a folder and the last part may be a pattern
// ("/sd/roms/quake/id1/*.pak": at least one match).
struct Required {
  std::string folder;   // relative folder under a data root ("" = the root itself)
  std::string name;     // the last part: a file or folder name, or a pattern
  bool is_dir{false};   // must be a folder
  bool is_glob{false};  // name is a pattern
  // The relative path under a data root, e.g. roms/doom/doom1.wad.
  std::string relative() const { return this->folder.empty() ? this->name : this->folder + "/" + this->name; }
};

inline bool parse_required(const std::string &path, Required *out) {
  static const std::string PREFIX = "/sd/";
  if (path.size() <= PREFIX.size() || path.size() > MAX_PATH || path.compare(0, PREFIX.size(), PREFIX) != 0)
    return false;
  std::string rest = path.substr(PREFIX.size());
  Required result;
  if (rest.back() == '/') {
    result.is_dir = true;
    rest.pop_back();
  }
  if (rest.empty())
    return false;
  const size_t slash = rest.rfind('/');
  result.folder = slash == std::string::npos ? std::string() : rest.substr(0, slash);
  result.name = slash == std::string::npos ? rest : rest.substr(slash + 1);
  if (!safe_segment(result.name))
    return false;
  size_t start = 0;
  while (!result.folder.empty() && start <= result.folder.size()) {
    size_t end = result.folder.find('/', start);
    if (end == std::string::npos)
      end = result.folder.size();
    const std::string segment = result.folder.substr(start, end - start);
    if (!safe_segment(segment) || has_glob(segment))  // patterns only in the last part
      return false;
    start = end + 1;
  }
  result.is_glob = has_glob(result.name);
  if (out != nullptr)
    *out = result;
  return true;
}

// The folders to try removing after deleting the file at `relative` (under a
// data root): its parent, then that one's parent, and so on, but never the
// first folder (roms in roms/doom/doom1.wad), which holds other apps too.
inline std::vector<std::string> parent_folders(const std::string &relative) {
  std::vector<std::string> folders;
  std::string folder = relative;
  for (;;) {
    const size_t slash = folder.rfind('/');
    if (slash == std::string::npos)
      break;
    folder.resize(slash);
    if (folder.find('/') == std::string::npos)
      break;  // a top-level folder
    folders.push_back(folder);
  }
  return folders;
}

}  // namespace files
}  // namespace papp_loader
}  // namespace esphome
