#pragma once

// Helpers shared by papp_loader.cpp and papp_store.cpp. Not part of the
// component's public API.

#include <cstddef>
#include <string>

#include "esp_err.h"

namespace esphome {
namespace papp_loader {

// Most the loader reads for an HTML catalog page.
static constexpr size_t MAX_CATALOG_SIZE = 64 * 1024;

// /sd/... becomes the VFS mount /sdcard/...; other paths are unchanged.
std::string runtime_path(const char *path);
bool is_network_url(const char *value);
// GET a small text resource over HTTP(S), at most max_bytes. ESP_ERR_NOT_FOUND
// for a 404, so callers can treat a missing optional file as normal.
esp_err_t fetch_http_text(const char *url, std::string *out, size_t max_bytes = MAX_CATALOG_SIZE);
// Creates the folders between `root` and the file `target`.
bool make_parent_dirs(const std::string &root, const std::string &target);

}  // namespace papp_loader
}  // namespace esphome
