// Host tests for esphome/components/papp_loader/papp_memory.h.
//
//   g++ -std=gnu++17 -Wall -Wextra -Werror -I esphome/components/papp_loader
//       tests/cpp/test_papp_memory.cpp -o /tmp/test_papp_memory && /tmp/test_papp_memory

#include "papp_memory.h"

#include <cstdio>
#include <initializer_list>
#include <limits>

using namespace esphome::papp_loader::memory;

static int failures = 0;

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                     \
    }                                                                 \
  } while (0)

// psram_app.h's PAPP_MEM_CAP_* values, as apps pass them.
static constexpr uint32_t APP_SPIRAM = 1u << 10;
static constexpr uint32_t APP_INTERNAL = 1u << 11;
static constexpr uint32_t APP_DMA = 1u << 2;

static bool same(const Attempt &a, uint32_t caps, bool guarded) { return a.caps == caps && a.guarded == guarded; }

// The app's plain heap: PSRAM first, internal RAM only as a guarded second try.
static void test_plain_plan() {
  const Plan plan = plain_plan();
  CHECK(plan.count == 2);
  CHECK(same(plan.attempts[0], CAP_SPIRAM | CAP_8BIT, false));
  CHECK(same(plan.attempts[1], CAP_INTERNAL | CAP_8BIT, true));
  // Never an unguarded internal try: that is what starved the drivers.
  for (int i = 0; i < plan.count; i++)
    CHECK(!((plan.attempts[i].caps & CAP_INTERNAL) != 0 && !plan.attempts[i].guarded));
}

// mem_caps_alloc: named caps as before, no placement caps like mem_alloc.
static void test_caps_plan() {
  Plan plan = caps_plan(APP_SPIRAM);
  CHECK(plan.count == 1 && same(plan.attempts[0], CAP_SPIRAM | CAP_8BIT, false));

  plan = caps_plan(APP_SPIRAM | APP_DMA);  // frame buffers (touchtest, LVGL)
  CHECK(plan.count == 1 && same(plan.attempts[0], CAP_SPIRAM | CAP_DMA | CAP_8BIT, false));

  plan = caps_plan(APP_INTERNAL);  // NetSurf's and Tulip's locks
  CHECK(plan.count == 1 && same(plan.attempts[0], CAP_INTERNAL | CAP_8BIT, false));

  plan = caps_plan(APP_INTERNAL | APP_DMA);  // OpenLara's internal frame
  CHECK(plan.count == 1 && same(plan.attempts[0], CAP_INTERNAL | CAP_DMA | CAP_8BIT, false));

  plan = caps_plan(APP_DMA);
  CHECK(plan.count == 1 && same(plan.attempts[0], CAP_DMA | CAP_8BIT, false));

  // Both heaps at once names none, and still fails as it always did.
  plan = caps_plan(APP_SPIRAM | APP_INTERNAL);
  CHECK(plan.count == 1 && same(plan.attempts[0], CAP_SPIRAM | CAP_INTERNAL | CAP_8BIT, false));

  // No placement: 0, or only bits the loader does not map (Doom's compat
  // header passes MALLOC_CAP_8BIT) -> the plain heap.
  for (uint32_t caps : {0u, CAP_8BIT, 1u << 20}) {
    plan = caps_plan(caps);
    CHECK(plan.count == 2 && same(plan.attempts[0], CAP_SPIRAM | CAP_8BIT, false) &&
          same(plan.attempts[1], CAP_INTERNAL | CAP_8BIT, true));
  }
  // Unknown bits are dropped from a named placement.
  plan = caps_plan(APP_SPIRAM | (1u << 20));
  CHECK(plan.count == 1 && same(plan.attempts[0], CAP_SPIRAM | CAP_8BIT, false));
}

// Internal RAM keeps its reserve.
static void test_internal_fits() {
  const size_t kb = 1024;
  CHECK(INTERNAL_RESERVE == 64 * kb);
  CHECK(internal_fits(100, 200 * kb, 100 * kb));
  CHECK(internal_fits(136 * kb, 200 * kb, 150 * kb));    // exactly the reserve left
  CHECK(!internal_fits(136 * kb + 1, 200 * kb, 150 * kb));
  CHECK(!internal_fits(1, 64 * kb, 64 * kb));            // nothing above the reserve
  CHECK(!internal_fits(0, 63 * kb, 63 * kb));            // already below it
  CHECK(internal_fits(0, 64 * kb, 0));
  CHECK(!internal_fits(8 * kb, 300 * kb, 4 * kb));       // fragmented: no piece that big
  CHECK(!internal_fits(10, 5, 100));                     // bad numbers: never
  CHECK(!internal_fits(std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max(),
                       std::numeric_limits<size_t>::max()));
  // Another reserve.
  CHECK(internal_fits(10, 20, 20, 10));
  CHECK(!internal_fits(11, 20, 20, 10));
  CHECK(internal_fits(20, 20, 20, 0));
}

static void test_calloc_bytes() {
  size_t total = 1;
  CHECK(calloc_bytes(4, 256, &total) && total == 1024);
  CHECK(calloc_bytes(0, 256, &total) && total == 0);
  CHECK(calloc_bytes(256, 0, &total) && total == 0);
  const size_t max = std::numeric_limits<size_t>::max();
  CHECK(calloc_bytes(max, 1, &total) && total == max);
  total = 7;
  CHECK(!calloc_bytes(max / 2 + 1, 2, &total) && total == 7);
  CHECK(!calloc_bytes(max, max, &total));
}

int main() {
  test_plain_plan();
  test_caps_plan();
  test_internal_fits();
  test_calloc_bytes();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("papp_memory.h: all tests passed\n");
  return 0;
}
