// Video backend for the Red Alert PAPP (replaces common/video_sdl2.cpp).
//
// The game draws 8-bit paletted pages (640x400 by default). The visible page
// is converted through a 256-entry RGB565 table into the loader's 800x480
// canvas, centred, with the mouse cursor drawn on top, whenever the game
// presents a frame (Frame_Limiter -> Video_Render_Frame).
#include "papp_port.h"

#include "common/video.h"
#include "common/wwmouse.h"

#include <stdlib.h>
#include <string.h>

static const int CANVAS_W = 800;
static const int CANVAS_H = 480;

static uint16_t s_palette[256];
static int s_mode_w = 640;
static int s_mode_h = 400;

// The game's mouse position in page pixels; moved by papp_input.cpp.
float papp_mouse_x = 320.0f;
float papp_mouse_y = 200.0f;

static struct
{
    unsigned char* image;  // palette indices, 0 = transparent
    int w, h, hotx, hoty;
} s_cursor = {nullptr, 0, 0, 0, 0};

class SurfaceMonitorClassPAPP : public SurfaceMonitorClass
{
public:
    void Restore_Surfaces() override
    {
    }
    void Set_Surface_Focus(bool) override
    {
    }
    void Release() override
    {
    }
};

static SurfaceMonitorClassPAPP s_all_surfaces;
SurfaceMonitorClass& AllSurfaces = s_all_surfaces;

SurfaceMonitorClass::SurfaceMonitorClass()
{
    SurfacesRestored = false;
}

// ── Presenting ──────────────────────────────────────────────────────────────
// Converting a 640x400 page to RGB565 (~13 ms, PSRAM-bound) and the loader's
// flush (~22 ms) used to run on the game task for every frame. Present() now
// only copies the page, palette and cursor into a snapshot; a presenter task
// on core 1 converts and flushes it. When the presenter is still busy, the
// frame is dropped instead of making the game wait.

struct Snapshot
{
    unsigned char* pixels;
    size_t capacity;
    int w, h;
    uint16_t palette[256];
    unsigned char cursor[64 * 64];
    int cursor_w, cursor_h, cursor_x, cursor_y; // cursor_w == 0: no cursor
};

static Snapshot s_snap;
static volatile bool s_snap_ready = false;
static volatile bool s_presenter_quit = false;
static volatile bool s_presenter_done = true;
static void* s_presenter = nullptr;

// Game-side counters for the rate log (read by the presenter).
static volatile int s_offered = 0;
static volatile int s_dropped = 0;
static volatile long long s_copy_us = 0;
static long long s_wait_us = 0; // presenter: time asleep waiting for a frame

static void render(const Snapshot& f, uint16_t* fb)
{
    const int ox = (CANVAS_W - f.w) / 2 > 0 ? (CANVAS_W - f.w) / 2 : 0;
    const int oy = (CANVAS_H - f.h) / 2 > 0 ? (CANVAS_H - f.h) / 2 : 0;
    const int cw = f.w < CANVAS_W ? f.w : CANVAS_W;
    const int ch = f.h < CANVAS_H ? f.h : CANVAS_H;
    const uint16_t* pal = f.palette;
    // 4 pixels per 32-bit read, two RGB565 pairs per 32-bit write.
    const bool words = ((f.w | cw | ox) & 3) == 0 && (((uintptr_t)f.pixels | (uintptr_t)fb) & 3) == 0;
    for (int y = 0; y < ch; y++) {
        const unsigned char* src = f.pixels + y * f.w;
        uint16_t* dst = fb + (oy + y) * CANVAS_W + ox;
        int x = 0;
        if (words) {
            const uint32_t* __restrict s4 = reinterpret_cast<const uint32_t*>(src);
            uint32_t* __restrict d2 = reinterpret_cast<uint32_t*>(dst);
            for (; x < cw; x += 4) {
                const uint32_t p = *s4++;
                d2[0] = pal[p & 0xFF] | (uint32_t)pal[(p >> 8) & 0xFF] << 16;
                d2[1] = pal[(p >> 16) & 0xFF] | (uint32_t)pal[p >> 24] << 16;
                d2 += 2;
            }
        }
        for (; x < cw; x++) {
            dst[x] = pal[src[x]];
        }
    }
    for (int y = 0; y < f.cursor_h; y++) {
        const int py = f.cursor_y + y;
        if (py < 0 || py >= ch) {
            continue;
        }
        for (int x = 0; x < f.cursor_w; x++) {
            const int px = f.cursor_x + x;
            const unsigned char c = f.cursor[y * f.cursor_w + x];
            if (c != 0 && px >= 0 && px < cw) {
                fb[(oy + py) * CANVAS_W + ox + px] = pal[c];
            }
        }
    }
}

// Every 5 s: frames shown and offered per second, and where the time goes.
static void log_rate(long long convert_us, long long flush_us)
{
    static long long window = 0, total_convert = 0, total_flush = 0;
    static int shown = 0, offered_at_start = 0, dropped_at_start = 0;
    static long long copy_at_start = 0;
    const long long now = papp_time_us();
    if (window == 0) {
        window = now;
        offered_at_start = s_offered;
        dropped_at_start = s_dropped;
        copy_at_start = s_copy_us;
    }
    shown++;
    total_convert += convert_us;
    total_flush += flush_us;
    if (now - window >= 5000000) {
        const long long elapsed = now - window;
        const int offered = s_offered - offered_at_start;
        const long long copy = s_copy_us - copy_at_start;
        papp_svc->log_printf("RA: %d fps shown, %d fps from the game (%d dropped); per frame: copy %lld us, "
                             "convert %lld us, flush %lld us; presenter waited %lld ms\n",
                             (int)(shown * 1000000LL / elapsed), (int)(offered * 1000000LL / elapsed),
                             s_dropped - dropped_at_start, offered > 0 ? copy / offered : 0,
                             total_convert / shown, total_flush / shown, s_wait_us / 1000);
        s_wait_us = 0;
        window = now;
        shown = 0;
        total_convert = total_flush = 0;
        offered_at_start = s_offered;
        dropped_at_start = s_dropped;
        copy_at_start = s_copy_us;
    }
}

static void present_snapshot()
{
    uint16_t* fb = papp_svc->display_get_framebuffer();
    if (fb == nullptr) {
        return;
    }
    const long long start = papp_time_us();
    render(s_snap, fb);
    const long long converted = papp_time_us();
    papp_svc->display_flush();
    log_rate(converted - start, papp_time_us() - converted);
}

static void presenter_task(void*)
{
    while (!s_presenter_quit) {
        if (!s_snap_ready) {
            // delay_ms() below one tick (10 ms) is only a yield: sleep a tick.
            const long long t = papp_time_us();
            papp_svc->delay_ms(10);
            s_wait_us += papp_time_us() - t;
            continue;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        present_snapshot();
        __atomic_thread_fence(__ATOMIC_RELEASE);
        s_snap_ready = false;
    }
    s_presenter_done = true;
    for (;;) {
        papp_svc->delay_ms(1000); // deleted by papp_video_shutdown
    }
}

extern "C" void papp_video_shutdown(void)
{
    if (s_presenter == nullptr) {
        return;
    }
    s_presenter_quit = true;
    for (int i = 0; i < 200 && !s_presenter_done; i++) {
        papp_svc->delay_ms(5);
    }
    papp_svc->task_delete(s_presenter);
    s_presenter = nullptr;
}

// Called by the game for every frame it presents.
static void papp_present(const unsigned char* pixels, int w, int h)
{
    if (s_presenter == nullptr && s_presenter_done) {
        s_presenter_quit = false;
        s_presenter_done = false;
        // Priority 1 like ESPHome's loop on core 1, so the two share the core
        // and touch/network keep running while frames are converted.
        if (papp_svc->task_create(presenter_task, "ra_present", 8 * 1024, nullptr, 1, &s_presenter, 1) != 0) {
            s_presenter = nullptr;
            papp_svc->log_printf("RA: no presenter task, presenting on the game task\n");
        }
    }
    s_offered++;
    if (s_presenter != nullptr && s_snap_ready) {
        s_dropped++; // the presenter is still showing the previous frame
        return;
    }
    const long long start = papp_time_us();
    const size_t bytes = (size_t)w * h;
    if (s_snap.capacity < bytes) {
        free(s_snap.pixels);
        s_snap.pixels = static_cast<unsigned char*>(malloc(bytes));
        s_snap.capacity = s_snap.pixels != nullptr ? bytes : 0;
        if (s_snap.pixels == nullptr) {
            return;
        }
    }
    memcpy(s_snap.pixels, pixels, bytes);
    s_snap.w = w;
    s_snap.h = h;
    memcpy(s_snap.palette, s_palette, sizeof(s_palette));
    s_snap.cursor_w = s_snap.cursor_h = 0;
    if (s_cursor.image != nullptr && !Get_Mouse_State() && s_cursor.w * s_cursor.h <= (int)sizeof(s_snap.cursor)) {
        memcpy(s_snap.cursor, s_cursor.image, s_cursor.w * s_cursor.h);
        s_snap.cursor_w = s_cursor.w;
        s_snap.cursor_h = s_cursor.h;
        s_snap.cursor_x = (int)papp_mouse_x - s_cursor.hotx;
        s_snap.cursor_y = (int)papp_mouse_y - s_cursor.hoty;
    }
    s_copy_us += papp_time_us() - start;
    if (s_presenter == nullptr) {
        present_snapshot();
        return;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s_snap_ready = true;
}

class VideoSurfacePAPP;
static VideoSurfacePAPP* s_front = nullptr;

class VideoSurfacePAPP : public VideoSurface
{
public:
    VideoSurfacePAPP(int w, int h, GBC_Enum flags)
        : w(w)
        , h(h)
        , pixels(static_cast<unsigned char*>(calloc(w * h, 1)))
    {
        if (flags & GBC_VISIBLE) {
            s_front = this;
        }
    }

    ~VideoSurfacePAPP() override
    {
        if (s_front == this) {
            s_front = nullptr;
        }
        free(pixels);
    }

    void* GetData() const override
    {
        return pixels;
    }
    int GetPitch() const override
    {
        return w;
    }
    bool IsAllocated() const override
    {
        return false;
    }
    void AddAttachedSurface(VideoSurface*) override
    {
    }
    bool IsReadyToBlit() override
    {
        return true;
    }
    bool LockWait() override
    {
        return true;
    }
    bool Unlock() override
    {
        return true;
    }

    void Blt(const Rect& dst, VideoSurface* source, const Rect& src, bool mask) override
    {
        VideoSurfacePAPP* from = static_cast<VideoSurfacePAPP*>(source);
        const int bw = src.Width < dst.Width ? src.Width : dst.Width;
        const int bh = src.Height < dst.Height ? src.Height : dst.Height;
        for (int y = 0; y < bh; y++) {
            const int sy = src.Y + y;
            const int dy = dst.Y + y;
            if (sy < 0 || sy >= from->h || dy < 0 || dy >= h) {
                continue;
            }
            for (int x = 0; x < bw; x++) {
                const int sx = src.X + x;
                const int dx = dst.X + x;
                if (sx < 0 || sx >= from->w || dx < 0 || dx >= w) {
                    continue;
                }
                const unsigned char c = from->pixels[sy * from->w + sx];
                if (!mask || c != 0) {
                    pixels[dy * w + dx] = c;
                }
            }
        }
    }

    // Same inclusive bounds as the SDL backend (Width/Height + 1).
    void FillRect(const Rect& rect, unsigned char color) override
    {
        for (int y = rect.Y; y <= rect.Y + rect.Height && y < h; y++) {
            if (y < 0) {
                continue;
            }
            int x0 = rect.X < 0 ? 0 : rect.X;
            int x1 = rect.X + rect.Width;
            if (x1 >= w) {
                x1 = w - 1;
            }
            if (x1 >= x0) {
                memset(pixels + y * w + x0, color, x1 - x0 + 1);
            }
        }
    }

    void Present()
    {
        papp_present(pixels, w, h);
    }

private:
    int w;
    int h;
    unsigned char* pixels;
};

void Video_Render_Frame()
{
    if (s_front != nullptr) {
        s_front->Present();
    }
}

Video::Video()
{
}

Video::~Video()
{
}

Video& Video::Shared()
{
    static Video video;
    return video;
}

VideoSurface* Video::CreateSurface(int w, int h, GBC_Enum flags)
{
    return new VideoSurfacePAPP(w, h, flags);
}

bool Set_Video_Mode(int w, int h, int bits_per_pixel)
{
    papp_svc->log_printf("RA: video mode %dx%d %d bpp\n", w, h, bits_per_pixel);
    s_mode_w = w;
    s_mode_h = h;
    papp_mouse_x = w / 2.0f;
    papp_mouse_y = h / 2.0f;
    papp_svc->display_clear(0x0000);
    return w <= CANVAS_W && h <= CANVAS_H;
}

void papp_video_mode_size(int* w, int* h)
{
    *w = s_mode_w;
    *h = s_mode_h;
}

bool Is_Video_Fullscreen()
{
    return true;
}

void Toggle_Video_Fullscreen()
{
}

void Reset_Video_Mode()
{
}

void Get_Video_Scale(float& x, float& y)
{
    x = 1.0f;
    y = 1.0f;
}

void Set_Video_Cursor_Clip(bool)
{
}

void Move_Video_Mouse(float xrel, float yrel)
{
    papp_mouse_x += xrel;
    papp_mouse_y += yrel;
    if (papp_mouse_x < 0) {
        papp_mouse_x = 0;
    } else if (papp_mouse_x > s_mode_w - 1) {
        papp_mouse_x = (float)(s_mode_w - 1);
    }
    if (papp_mouse_y < 0) {
        papp_mouse_y = 0;
    } else if (papp_mouse_y > s_mode_h - 1) {
        papp_mouse_y = (float)(s_mode_h - 1);
    }
}

void Get_Video_Mouse(int& x, int& y)
{
    x = (int)papp_mouse_x;
    y = (int)papp_mouse_y;
}

void Set_Video_Cursor(void* cursor, int w, int h, int hotx, int hoty)
{
    free(s_cursor.image);
    s_cursor.image = nullptr;
    if (cursor != nullptr && w > 0 && h > 0) {
        s_cursor.image = static_cast<unsigned char*>(malloc(w * h));
        if (s_cursor.image != nullptr) {
            memcpy(s_cursor.image, cursor, w * h);
        }
    }
    s_cursor.w = w;
    s_cursor.h = h;
    s_cursor.hotx = hotx;
    s_cursor.hoty = hoty;
}

unsigned int Get_Free_Video_Memory()
{
    return 1000000000;
}

// No blitter: the game keeps its hidden page in plain memory.
unsigned Get_Video_Hardware_Capabilities()
{
    return 0;
}

void Wait_Vert_Blank()
{
}

void Wait_Blit()
{
}

// 256 RGB triplets of 6-bit VGA values -> RGB565 (red in the high bits).
void Set_DD_Palette(void* palette)
{
    const unsigned char* rgb = static_cast<const unsigned char*>(palette);
    for (int i = 0; i < 256; i++) {
        const unsigned r = (rgb[i * 3 + 0] & 0x3F) << 2;
        const unsigned g = (rgb[i * 3 + 1] & 0x3F) << 2;
        const unsigned b = (rgb[i * 3 + 2] & 0x3F) << 2;
        s_palette[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}
