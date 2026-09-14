#pragma once

// App hand-off: one app opening another (psram_app.h app_open, app_get_arg,
// app_set_resume_arg) and the way back to it.
//
// A running app asks for a target (an app's name, a .papp URL or a /sd path)
// with an argument. The loader closes the caller the usual way, then starts
// the target with that argument. With return_after, the caller is started
// again once the target has quit (or failed to load), with the argument the
// caller asked to get back (its resume argument). The apps to go back to form
// a stack of at most MAX_DEPTH.
//
// Everything here is plain C++ with no ESP-IDF dependency, so it can be unit
// tested on a host (tests/cpp/test_papp_handoff.cpp).

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "papp_canvas.h"  // canvas::app_key

namespace esphome {
namespace papp_loader {
namespace handoff {

// Longest argument, in bytes without the terminating NUL (PAPP_APP_ARG_MAX - 1).
static constexpr size_t MAX_ARG = 2047;
// Longest target (a name, URL or path).
static constexpr size_t MAX_TARGET = 255;
// Longest app name.
static constexpr size_t MAX_NAME = 64;
// How many apps may wait to be returned to.
static constexpr size_t MAX_DEPTH = 2;

enum class Target { INVALID, NAME, URL, PATH };

inline std::string lower(std::string text) {
  for (char &c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// True when the path part (before any ?query or #fragment) ends in ".papp".
inline bool is_papp_file(const std::string &target) {
  const std::string path = lower(target.substr(0, target.find_first_of("?#")));
  return path.size() > 5 && path.compare(path.size() - 5, 5, ".papp") == 0 && path[path.size() - 6] != '/';
}

// An app name as the store uses it: psram_video. Letters, digits, '_', '-'
// and '.', not starting with '.' or '-'.
inline bool valid_name(const std::string &name) {
  if (name.empty() || name.size() > MAX_NAME || name[0] == '.' || name[0] == '-')
    return false;
  for (char c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.')
      return false;
  }
  return name.find("..") == std::string::npos;
}

// What kind of target app_open was given.
inline Target classify(const std::string &target) {
  if (target.empty() || target.size() > MAX_TARGET)
    return Target::INVALID;
  for (char c : target) {
    if (static_cast<unsigned char>(c) <= ' ' || c == 0x7f)
      return Target::INVALID;
  }
  const std::string low = lower(target);
  for (const char *scheme : {"http://", "https://"}) {
    const size_t n = std::strlen(scheme);
    if (low.compare(0, n, scheme) == 0) {
      const size_t host_end = target.find_first_of("/?#", n);
      if (host_end == n || host_end == std::string::npos)
        return Target::INVALID;  // no host, or no path to a .papp
      return is_papp_file(target) ? Target::URL : Target::INVALID;
    }
  }
  if (target[0] == '/') {
    if (target.find("/../") != std::string::npos || target.find("/./") != std::string::npos ||
        target.find_first_of("?#") != std::string::npos)
      return Target::INVALID;
    return is_papp_file(target) ? Target::PATH : Target::INVALID;
  }
  return valid_name(target) ? Target::NAME : Target::INVALID;
}

// The file name of a .papp without folders, query and ".papp":
// https://host/apps/psram_video-0.1.0.papp?x -> psram_video-0.1.0.
inline std::string papp_stem(const std::string &path) {
  std::string name = path.substr(0, path.find_first_of("?#"));
  const size_t slash = name.find_last_of('/');
  if (slash != std::string::npos)
    name = name.substr(slash + 1);
  if (is_papp_file(name))
    name.resize(name.size() - 5);
  return name;
}

// The version in a .papp's file name (psram_video-0.2.0.papp -> "0.2.0"), or
// "" when it has none (an installed copy, psram_video.papp).
inline std::string version_of(const std::string &path) {
  const std::string stem = papp_stem(path);
  const std::string key = canvas::app_key(path);
  if (stem.size() > key.size() + 1 && stem[key.size()] == '-')
    return stem.substr(key.size() + 1);
  return {};
}

// Compares dot-separated versions number by number: -1, 0 or 1. "" (no
// version) is lower than any version; "0.10" is higher than "0.9".
inline int compare_versions(const std::string &a, const std::string &b) {
  if (a.empty() || b.empty())
    return a.empty() == b.empty() ? 0 : (a.empty() ? -1 : 1);
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    unsigned long x = 0, y = 0;
    while (i < a.size() && a[i] != '.')
      x = x * 10 + static_cast<unsigned long>(a[i++] - '0');
    while (j < b.size() && b[j] != '.')
      y = y * 10 + static_cast<unsigned long>(b[j++] - '0');
    if (x != y)
      return x < y ? -1 : 1;
    i++;
    j++;
  }
  return 0;
}

// The .papp (a URL or path from a library source) that is app `name`: its
// file name is name.papp or name-<version>.papp. With several, the highest
// version; one without a version counts lowest. "" when none is.
inline std::string best_match(const std::vector<std::string> &candidates, const std::string &name) {
  std::string best;
  std::string best_version;
  for (const auto &candidate : candidates) {
    if (!is_papp_file(candidate) || canvas::app_key(candidate) != name)
      continue;
    const std::string version = version_of(candidate);
    if (best.empty() || compare_versions(version, best_version) > 0) {
      best = candidate;
      best_version = version;
    }
  }
  return best;
}

// app_get_arg's copy: `arg` into buf (NUL-terminated, cut to len - 1 bytes);
// returns the full length, like snprintf. buf may be NULL when len is 0.
inline int copy_arg(const std::string &arg, char *buf, int len) {
  if (buf != nullptr && len > 0) {
    const size_t n = std::min(arg.size(), static_cast<size_t>(len) - 1);
    std::memcpy(buf, arg.data(), n);
    buf[n] = '\0';
  }
  return static_cast<int>(arg.size());
}

// One app launch: its .papp (path or URL) and the argument it gets.
struct Frame {
  std::string source;
  std::string arg;
};

// The hand-off state of a loader: at most one request waiting for the
// running app to quit, and the apps to go back to (the newest last).
class Chain {
 public:
  // The running app `caller` (its source and resume argument) asks for
  // `target`. False when a request is already waiting, or when return_after
  // would make more than MAX_DEPTH apps wait.
  bool request(const Frame &caller, const Frame &target, bool return_after) {
    if (this->has_pending_ || (return_after && this->stack_.size() >= MAX_DEPTH))
      return false;
    this->pending_caller_ = caller;
    this->pending_target_ = target;
    this->pending_return_ = return_after;
    this->has_pending_ = true;
    return true;
  }

  bool pending() const { return this->has_pending_; }
  size_t depth() const { return this->stack_.size(); }

  // The running app has ended, or the last launch failed: what to start next.
  // The waiting request first (remembering its caller when it asked to come
  // back), else the newest app to go back to. False: none, back to the menu.
  bool next(Frame *out) {
    if (this->has_pending_) {
      this->has_pending_ = false;
      if (this->pending_return_)
        this->stack_.push_back(this->pending_caller_);
      *out = this->pending_target_;
      return true;
    }
    if (this->stack_.empty())
      return false;
    *out = this->stack_.back();
    this->stack_.pop_back();
    return true;
  }

  // A launch from the menu, a button or remotely starts over.
  void reset() {
    this->has_pending_ = false;
    this->stack_.clear();
  }

 private:
  bool has_pending_{false};
  bool pending_return_{false};
  Frame pending_caller_;
  Frame pending_target_;
  std::vector<Frame> stack_;
};

}  // namespace handoff
}  // namespace papp_loader
}  // namespace esphome
