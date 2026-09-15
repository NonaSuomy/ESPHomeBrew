#pragma once

// Where an app's heap comes from (psram_app.h mem_alloc, mem_calloc,
// mem_realloc and mem_caps_alloc).
//
// ESPHome builds ESP-IDF with CONFIG_SPIRAM_USE_CAPS_ALLOC, so plain malloc()
// never returns PSRAM: every block comes from internal RAM. On the ESP32-P4
// all of that RAM is DMA-capable, and it is the RAM the I2S speaker, the SD
// card, Ethernet and lwIP need while an app runs. An app that fills it (Doom's
// PrBoom keeps every small lump it has read until malloc() fails) leaves the
// drivers nothing: the speaker cannot allocate its DMA buffers and the native
// API stops accepting connections.
//
// So an app's plain heap is PSRAM. Internal RAM is used only when PSRAM is
// full, and then only while INTERNAL_RESERVE bytes of it stay free. Blocks an
// app asks for with explicit caps (mem_caps_alloc with PAPP_MEM_CAP_SPIRAM,
// _INTERNAL or _DMA) come from where it asked, as before.
//
// Everything here is plain C++ with no ESP-IDF dependency, so it can be unit
// tested on a host (tests/cpp/test_papp_memory.cpp).

#include <cstddef>
#include <cstdint>
#include <limits>

namespace esphome {
namespace papp_loader {
namespace memory {

// ESP-IDF's MALLOC_CAP_* bits, which psram_app.h's PAPP_MEM_CAP_* share.
static constexpr uint32_t CAP_DMA = 1u << 2;
static constexpr uint32_t CAP_SPIRAM = 1u << 10;
static constexpr uint32_t CAP_INTERNAL = 1u << 11;
static constexpr uint32_t CAP_8BIT = 1u << 13;
// The placement bits an app may pass to mem_caps_alloc; others are ignored.
static constexpr uint32_t APP_CAPS = CAP_SPIRAM | CAP_INTERNAL | CAP_DMA;

// Internal RAM an app's plain heap never takes: enough for the speaker's I2S
// DMA buffers, lwIP, the native API and a TLS session.
static constexpr size_t INTERNAL_RESERVE = 64 * 1024;

// Whether `size` bytes may come from internal RAM that has `free_bytes` free,
// `largest_block` of it in one piece: the block must fit and at least
// `reserve` bytes must stay free afterwards.
inline bool internal_fits(size_t size, size_t free_bytes, size_t largest_block,
                          size_t reserve = INTERNAL_RESERVE) {
  if (size > largest_block || size > free_bytes)
    return false;
  return free_bytes - size >= reserve;
}

// n * size for calloc, or false when it overflows.
inline bool calloc_bytes(size_t n, size_t size, size_t *total) {
  if (size != 0 && n > std::numeric_limits<size_t>::max() / size)
    return false;
  *total = n * size;
  return true;
}

// One try of an allocation: these heap caps. A `guarded` try is internal RAM
// and is only made while internal_fits() says so.
struct Attempt {
  uint32_t caps;
  bool guarded;
};

// The tries in order; the first that succeeds is the block.
struct Plan {
  Attempt attempts[2];
  int count;
};

// mem_alloc, mem_calloc and mem_realloc: PSRAM, then internal RAM above the
// reserve.
inline Plan plain_plan() { return Plan{{{CAP_SPIRAM | CAP_8BIT, false}, {CAP_INTERNAL | CAP_8BIT, true}}, 2}; }

// mem_caps_alloc(size, caps): with no placement bits (0, or only bits the
// loader does not know, such as MALLOC_CAP_8BIT) it is a plain allocation;
// otherwise exactly the caps the app named, as 8-bit memory. SPIRAM together
// with INTERNAL names no heap and fails, as it always has.
inline Plan caps_plan(uint32_t caps) {
  const uint32_t placement = caps & APP_CAPS;
  if (placement == 0)
    return plain_plan();
  return Plan{{{placement | CAP_8BIT, false}, {0, false}}, 1};
}

}  // namespace memory
}  // namespace papp_loader
}  // namespace esphome
