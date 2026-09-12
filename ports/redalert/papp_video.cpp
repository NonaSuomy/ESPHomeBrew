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
        uint16_t* fb = papp_svc->display_get_framebuffer();
        if (fb == nullptr) {
            return;
        }
        const long long start = papp_time_us();
        const int ox = (CANVAS_W - w) / 2 > 0 ? (CANVAS_W - w) / 2 : 0;
        const int oy = (CANVAS_H - h) / 2 > 0 ? (CANVAS_H - h) / 2 : 0;
        const int cw = w < CANVAS_W ? w : CANVAS_W;
        const int ch = h < CANVAS_H ? h : CANVAS_H;
        // Both buffers are in PSRAM: move 4 pixels per 32-bit read and two
        // 32-bit writes instead of byte/halfword accesses (the byte loop took
        // ~16 ms per 640x400 frame).
        const bool words = ((w | cw | ox) & 3) == 0 && (((uintptr_t)pixels | (uintptr_t)fb) & 3) == 0;
        for (int y = 0; y < ch; y++) {
            const unsigned char* src = pixels + y * w;
            uint16_t* dst = fb + (oy + y) * CANVAS_W + ox;
            int x = 0;
            if (words) {
                const uint32_t* __restrict s4 = reinterpret_cast<const uint32_t*>(src);
                uint32_t* __restrict d2 = reinterpret_cast<uint32_t*>(dst);
                for (; x < cw; x += 4) {
                    const uint32_t p = *s4++;
                    d2[0] = s_palette[p & 0xFF] | (uint32_t)s_palette[(p >> 8) & 0xFF] << 16;
                    d2[1] = s_palette[(p >> 16) & 0xFF] | (uint32_t)s_palette[p >> 24] << 16;
                    d2 += 2;
                }
            }
            for (; x < cw; x++) {
                dst[x] = s_palette[src[x]];
            }
        }
        // Software cursor, unless the game has hidden it.
        if (s_cursor.image != nullptr && !Get_Mouse_State()) {
            const int cx = (int)papp_mouse_x - s_cursor.hotx;
            const int cy = (int)papp_mouse_y - s_cursor.hoty;
            for (int y = 0; y < s_cursor.h; y++) {
                const int py = cy + y;
                if (py < 0 || py >= ch) {
                    continue;
                }
                for (int x = 0; x < s_cursor.w; x++) {
                    const int px = cx + x;
                    const unsigned char c = s_cursor.image[y * s_cursor.w + x];
                    if (c != 0 && px >= 0 && px < cw) {
                        fb[(oy + py) * CANVAS_W + ox + px] = s_palette[c];
                    }
                }
            }
        }
        const long long converted = papp_time_us();
        papp_svc->display_flush();
        Log_Rate(start, converted, papp_time_us());
    }

    // Every 5 s: frames presented per second and where a frame's time goes.
    static void Log_Rate(long long start, long long converted, long long flushed)
    {
        static long long window = 0, convert_us = 0, flush_us = 0;
        static int frames = 0;
        if (window == 0) {
            window = start;
        }
        frames++;
        convert_us += converted - start;
        flush_us += flushed - converted;
        if (flushed - window >= 5000000) {
            const long long elapsed = flushed - window;
            papp_svc->log_printf("RA: %d.%d fps, per frame: convert %lld us, flush %lld us\n",
                                 (int)(frames * 10000000LL / elapsed / 10), (int)(frames * 10000000LL / elapsed % 10),
                                 convert_us / frames, flush_us / frames);
            window = flushed;
            frames = 0;
            convert_us = flush_us = 0;
        }
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
