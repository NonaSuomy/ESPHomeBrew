// Video for the OpenLara PAPP (replaces the ESP32-P4 port's PPA code).
//
// OpenLara's software renderer draws 320x240 RGB565 into GAPI::swColor. The
// loader scales such a frame 2x to 640x480, centred on its 800x480 canvas,
// with the P4's PPA (display_write_frame_custom). Its flush costs ~20 ms, so
// a presenter task on core 1 shows frame N while the game draws frame N+1
// into the other buffer. The first buffer is the loader's internal-RAM
// emulator buffer (faster for the renderer than PSRAM) when it has one.
#include "papp_port.h"

#include <string.h>

static uint16_t *s_frames[2] = {nullptr, nullptr};
static bool s_owned[2] = {false, false};   // allocated here (else the loader's)
static int s_back = 0;                     // the buffer the game draws into
static volatile int s_show = -1;           // the buffer the presenter shows next (-1: none)
static volatile bool s_quit = false;
static volatile bool s_done = true;
static void *s_task = nullptr;

static const size_t FRAME_BYTES = PAPP_OL_WIDTH * PAPP_OL_HEIGHT * sizeof(uint16_t);

static void show(const uint16_t *frame)
{
    papp_svc->display_write_frame_custom(frame, PAPP_OL_WIDTH, PAPP_OL_HEIGHT, 2.0f, false);
}

// Every 60 s: frames shown per second and the average time a flush takes.
static void log_rate(long long flush_us)
{
    static long long window = 0, total = 0;
    static int shown = 0;
    const long long now = papp_time_us();
    if (window == 0) {
        window = now;
    }
    shown++;
    total += flush_us;
    if (now - window >= 60000000) {
        papp_svc->log_printf("OL: %d fps shown, flush %lld us\n", (int)(shown * 1000000LL / (now - window)),
                             total / shown);
        window = now;
        shown = 0;
        total = 0;
    }
}

static void presenter_task(void *)
{
    while (!s_quit) {
        const int index = s_show;
        if (index < 0) {
            // delay_ms() below one tick (10 ms) is only a yield: sleep a tick.
            papp_svc->delay_ms(10);
            continue;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const long long start = papp_time_us();
        show(s_frames[index]);
        log_rate(papp_time_us() - start);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        s_show = -1;
    }
    s_done = true;
    for (;;) {
        papp_svc->delay_ms(1000); // deleted by papp_video_shutdown
    }
}

extern "C" int papp_video_init(void)
{
    uint16_t *emu = papp_svc->display_get_emu_buffer != nullptr ? papp_svc->display_get_emu_buffer() : nullptr;
    for (int i = 0; i < 2; i++) {
        if (i == 0 && emu != nullptr) {
            s_frames[i] = emu;  // the loader's 320x240 buffer
            continue;
        }
        s_frames[i] = static_cast<uint16_t *>(papp_alloc_raw(FRAME_BYTES, 1));
        if (s_frames[i] == nullptr) {
            s_frames[i] = static_cast<uint16_t *>(papp_alloc_raw(FRAME_BYTES, 0));
        }
        if (s_frames[i] == nullptr) {
            papp_svc->log_printf("OL: no memory for the frame buffers\n");
            return -1;
        }
        s_owned[i] = true;
    }
    for (int i = 0; i < 2; i++) {
        memset(s_frames[i], 0, FRAME_BYTES);
    }
    papp_svc->display_clear(0x0000);
    s_back = 0;
    s_show = -1;
    s_quit = false;
    s_done = false;
    // Core 1, priority 3: above ESPHome's loop, below the audio task. It
    // sleeps between frames, so the loop still gets its turn.
    if (papp_svc->task_create(presenter_task, "ol_present", 8 * 1024, nullptr, 3, &s_task, 1) != 0) {
        s_task = nullptr;
        s_done = true;
        papp_svc->log_printf("OL: no presenter task, presenting on the game task\n");
    }
    return 0;
}

extern "C" uint16_t *papp_video_back(void)
{
    return s_frames[s_back];
}

extern "C" void papp_video_present(void)
{
    if (s_task == nullptr) {
        show(s_frames[s_back]);
        return;
    }
    // Wait for the presenter to finish the other buffer (the previous frame):
    // the game draws into it next.
    while (s_show >= 0) {
        papp_svc->delay_ms(1);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s_show = s_back;
    s_back ^= 1;
}

extern "C" void papp_video_shutdown(void)
{
    if (s_task != nullptr) {
        s_quit = true;
        for (int i = 0; i < 200 && !s_done; i++) {
            papp_svc->delay_ms(5);
        }
        papp_svc->task_delete(s_task);
        s_task = nullptr;
    }
    for (int i = 0; i < 2; i++) {
        if (s_owned[i]) {
            papp_svc->mem_free(s_frames[i]);
        }
        s_frames[i] = nullptr;
        s_owned[i] = false;
    }
}
