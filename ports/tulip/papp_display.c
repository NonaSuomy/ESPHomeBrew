// Display and input for the Tulip PAPP (replaces tulip/shared/desktop/
// unix_display.c, which drove SDL).
//
// Tulip's display engine (tulip/shared/display.c) composites its background,
// sprites and text frame buffer (TFB) one 12-pixel band at a time into an
// RGB332 bounce buffer (display_bounce_empty), exactly as Tulip's RGB panel
// driver and SDL window do. This port builds Tulip at 800x480 (see
// patches/0001-display-800x480.patch), the loader's canvas, so each band is
// converted to RGB565 straight into the loader's framebuffer and shown 1:1.
//
// The same task polls the loader's input every frame and feeds Tulip the way
// its desktop build does: keys to send_key_to_micropython() (REPL, editor,
// keyboard callbacks), touch to send_touch_to_micropython() and LVGL, held
// gamepad buttons as HID scan codes in last_scan[] (tulip.keys(), joyk()).
#include "papp_port.h"

#include <string.h>

#include "display.h"
#include "keyscan.h"
#include "lvgl.h"

#define FRAME_US (1000000 / 30)
#define CANVAS_W 800
#define CANVAS_H 480

#if H_RES != CANVAS_W || V_RES != CANVAS_H
#error "Tulip must be built at 800x480 for the PAPP canvas (patches/0001-display-800x480.patch)"
#endif

static uint16_t s_lut[256];          // RGB332 -> RGB565
static uint8_t *s_band = NULL;       // one FONT_HEIGHT-row RGB332 band
static void *s_task = NULL;
static volatile int s_stop = 0;
static volatile int s_stopped = 1;

// ── Keyboard ────────────────────────────────────────────────────────────────
// The loader queues USB keyboard keys as taps (down+up at once): printable
// keys as ASCII, others as the numbers below (papp_loader.cpp
// enqueue_keyboard_text). It has no events for the arrow keys; those, WASD
// and a few others only show up as held gamepad inputs (see poll_gamepad).
enum {
    LK_BACKSPACE = 127,
    LK_TAB = 9,
    LK_ENTER = 13,
    LK_ESCAPE = 27,
    LK_ALT = 132,
    LK_CTRL = 133,
    LK_SHIFT = 134,
    LK_F1 = 135,   // .. F12 = 146
    LK_INSERT = 147,
    LK_DELETE = 148,
    LK_PAGE_DOWN = 149,
    LK_PAGE_UP = 150,
    LK_HOME = 151,
    LK_END = 152,
};

// Tulip's own key codes for keys that are not ASCII (keyscan.c scan_ascii).
enum {
    TK_DOWN = 258,
    TK_UP = 259,
    TK_LEFT = 260,
    TK_RIGHT = 261,
    TK_DELETE = 262,
    TK_CTRL_TAB = 263,
    TK_PAGE_UP = 25,
    TK_PAGE_DOWN = 22,
};

// Ctrl arrives as its own tap, before the letter: it applies to the next key
// pressed within CTRL_STICKY_US (Ctrl-C interrupts, Ctrl-X leaves the editor,
// Ctrl-Q quits a UI app, Ctrl-Tab switches apps).
#define CTRL_STICKY_US 1500000
static int64_t s_ctrl_at = 0;

// When W/A/S/D was last typed: those letters also press gamepad directions,
// which must not turn into arrow keys while typing.
static int64_t s_letter_at[4];  // indexed like the directions below

enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT, DIR_COUNT };
static const int s_dir_input[DIR_COUNT] = {PAPP_INPUT_UP, PAPP_INPUT_DOWN, PAPP_INPUT_LEFT, PAPP_INPUT_RIGHT};
static const int s_dir_key[DIR_COUNT] = {TK_UP, TK_DOWN, TK_LEFT, TK_RIGHT};
static const char s_dir_letter[DIR_COUNT] = {'w', 's', 'a', 'd'};

// LVGL's keypad (text areas, focus) reads its keys from this ring in the
// MicroPython task (lvgl_keyboard_read below); the display task fills it.
// Same mapping as Tulip's T-Deck keyboard (keycode_to_ctrl_key).
#define LV_KEYS 64
static uint32_t s_lv_keys[LV_KEYS];
static volatile unsigned s_lv_head = 0, s_lv_tail = 0;

static void lv_key_push(uint32_t key)
{
    if (s_lv_head - s_lv_tail < LV_KEYS) {
        s_lv_keys[s_lv_head % LV_KEYS] = key;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        s_lv_head++;
    }
}

static void send_key(int c)
{
    const uint32_t ctrl = keycode_to_ctrl_key((uint16_t)c);
    if (ctrl != '\0') {
        lv_key_push(ctrl);
    } else if (c >= 32 && c < 127) {
        lv_key_push((uint32_t)c);
    }
    send_key_to_micropython((uint16_t)c);
}

static void poll_keyboard(int64_t now)
{
    if (papp_svc->input_keyboard_read == NULL) {
        return;
    }
    papp_keyboard_event_t ev;
    while (papp_svc->input_keyboard_read(&ev)) {
        if (!ev.down) {
            continue;
        }
        int key = ev.key;
        const int ctrl = s_ctrl_at != 0 && now - s_ctrl_at < CTRL_STICKY_US;
        switch (key) {
        case LK_CTRL:
            s_ctrl_at = now;
            continue;
        case LK_ALT:
        case LK_SHIFT:
        case LK_INSERT:
        case LK_HOME:
        case LK_END:
            continue;  // Tulip has no use for these (scan_ascii ignores them too)
        case LK_BACKSPACE:
            key = 8;
            break;
        case LK_DELETE:
            key = TK_DELETE;
            break;
        case LK_PAGE_UP:
            key = TK_PAGE_UP;
            break;
        case LK_PAGE_DOWN:
            key = TK_PAGE_DOWN;
            break;
        case LK_TAB:
            key = ctrl ? TK_CTRL_TAB : LK_TAB;
            break;
        default:
            if (key >= LK_F1 && key < LK_F1 + 12) {
                continue;
            }
            if (key > 255) {
                continue;
            }
            for (int d = 0; d < DIR_COUNT; d++) {
                if (key == s_dir_letter[d] || key == s_dir_letter[d] - 32) {
                    s_letter_at[d] = now;
                }
            }
            if (ctrl && ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') || key == '[' || key == '\\' ||
                         key == ']' || key == '^' || key == '_')) {
                key &= 0x1f;
            }
            break;
        }
        s_ctrl_at = 0;
        send_key(key);
    }
}

// ── Gamepad ─────────────────────────────────────────────────────────────────
// Directions become arrow keys (REPL history, cursor, editor) with key
// repeat, unless the same direction's WASD letter was typed around the time
// it went down. Every held button is also reported to Tulip as the HID scan
// code tulip_graphics.joyk() maps to its Joy bits.

#define ARROW_DECIDE_US 60000   // wait this long for the matching letter tap
#define ARROW_LETTER_US 250000  // a letter this close to the press means typing
#define REPEAT_DELAY_US 400000
#define REPEAT_EVERY_US 60000
#define MENU_QUIT_US 3000000

static int64_t s_dir_down_at[DIR_COUNT];   // 0: not held
static int s_dir_state[DIR_COUNT];         // 0 undecided, 1 arrow, 2 typing
static int64_t s_dir_next[DIR_COUNT];
static int64_t s_menu_since = 0;

static const struct {
    int input;
    uint8_t scan;
} PAD_SCANS[] = {
    {PAPP_INPUT_RIGHT, 0x4F}, {PAPP_INPUT_LEFT, 0x50}, {PAPP_INPUT_DOWN, 0x51}, {PAPP_INPUT_UP, 0x52},
    {PAPP_INPUT_A, 27},       {PAPP_INPUT_B, 29},      {PAPP_INPUT_X, 22},      {PAPP_INPUT_Y, 4},
    {PAPP_INPUT_START, 40},   {PAPP_INPUT_SELECT, 42}, {PAPP_INPUT_L, 20},      {PAPP_INPUT_R, 8},
};

static void poll_gamepad(int64_t now)
{
    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    if (papp_svc->input_gamepad_read != NULL) {
        papp_svc->input_gamepad_read(&pad);
    }
    // Leaving: the loader's close controls, or Menu (Escape on a keyboard)
    // held for 3 s, like the other ports.
    if ((papp_svc->input_l3_read != NULL && papp_svc->input_l3_read()) ||
        (pad.values[PAPP_INPUT_MENU] && pad.values[PAPP_INPUT_X])) {
        papp_request_quit(0);
        return;
    }
    if (pad.values[PAPP_INPUT_MENU]) {
        if (s_menu_since == 0) {
            s_menu_since = now;
        } else if (now - s_menu_since >= MENU_QUIT_US) {
            papp_request_quit(0);
            return;
        }
    } else {
        s_menu_since = 0;
    }

    for (int d = 0; d < DIR_COUNT; d++) {
        if (!pad.values[s_dir_input[d]]) {
            // A tap shorter than the decision time: an arrow, unless typed.
            if (s_dir_down_at[d] != 0 && s_dir_state[d] == 0) {
                const int64_t since_letter = s_dir_down_at[d] - s_letter_at[d];
                if (s_letter_at[d] == 0 || since_letter >= ARROW_LETTER_US || since_letter <= -ARROW_LETTER_US) {
                    send_key(s_dir_key[d]);
                }
            }
            s_dir_down_at[d] = 0;
            continue;
        }
        if (s_dir_down_at[d] == 0) {
            s_dir_down_at[d] = now;
            s_dir_state[d] = 0;
        }
        if (s_dir_state[d] == 0 && now - s_dir_down_at[d] >= ARROW_DECIDE_US) {
            const int64_t since_letter = s_dir_down_at[d] - s_letter_at[d];
            const int typing = s_letter_at[d] != 0 && since_letter < ARROW_LETTER_US && since_letter > -ARROW_LETTER_US;
            s_dir_state[d] = typing ? 2 : 1;
            if (!typing) {
                send_key(s_dir_key[d]);
                s_dir_next[d] = now + REPEAT_DELAY_US;
            }
        } else if (s_dir_state[d] == 1 && now >= s_dir_next[d]) {
            send_key(s_dir_key[d]);
            s_dir_next[d] = now + REPEAT_EVERY_US;
        }
    }

    // tulip.keys(): up to six held scan codes in last_scan[2..7].
    int slot = 2;
    for (size_t i = 0; i < sizeof(PAD_SCANS) / sizeof(PAD_SCANS[0]) && slot < 8; i++) {
        if (pad.values[PAD_SCANS[i].input]) {
            last_scan[slot++] = PAD_SCANS[i].scan;
        }
    }
    while (slot < 8) {
        last_scan[slot++] = 0;
    }
}

// ── Touch ───────────────────────────────────────────────────────────────────
// The loader reports touches in its 800x480 canvas space: Tulip's own.

static int s_touching = 0;

static void poll_touch(void)
{
    if (papp_svc->touch_read == NULL) {
        return;
    }
    int x = 0, y = 0;
    const int down = papp_svc->touch_read(&x, &y);
    if (down) {
        last_touch_x[0] = (int16_t)x;
        last_touch_y[0] = (int16_t)y;
        send_touch_to_micropython((int16_t)x, (int16_t)y, 0);
        s_touching = 1;
    } else if (s_touching) {
        send_touch_to_micropython(last_touch_x[0], last_touch_y[0], 1);
        s_touching = 0;
    }
}

// LVGL's keypad device (display.c lvgl_input_kb_read_cb), as Tulip Desktop's
// SDL version: each key is reported pressed, then released on the next read.
void lvgl_keyboard_read(lv_indev_t *indev_drv, lv_indev_data_t *data)
{
    (void)indev_drv;
    static bool release_next = false;
    if (release_next) {
        release_next = false;
        data->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = s_lv_head != s_lv_tail;
    } else if (s_lv_head != s_lv_tail) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        data->key = s_lv_keys[s_lv_tail % LV_KEYS];
        s_lv_tail++;
        data->state = LV_INDEV_STATE_PRESSED;
        data->continue_reading = true;
        release_next = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = false;
    }
}

// ── Frames ──────────────────────────────────────────────────────────────────

static void draw_frame(void)
{
    uint16_t *fb = papp_svc->display_get_framebuffer();
    if (fb == NULL) {
        return;
    }
    for (int y = 0; y < V_RES; y += FONT_HEIGHT) {
        display_bounce_empty(s_band, y * H_RES, H_RES * FONT_HEIGHT, NULL);
        for (int row = 0; row < FONT_HEIGHT; row++) {
            const uint8_t *src = s_band + row * H_RES;
            uint16_t *dst = fb + (y + row) * CANVAS_W;
            for (int x = 0; x < H_RES; x++) {
                dst[x] = s_lut[src[x]];
            }
        }
    }
    papp_svc->display_flush();
}

static void display_task(void *arg)
{
    (void)arg;
    int64_t next = papp_time_us();
    int64_t fps_window = next;
    int frames = 0;
    while (!s_stop) {
        const int64_t now = papp_time_us();
        if (papp_mp_ready) {
            poll_keyboard(now);
            poll_gamepad(now);
            poll_touch();
        }
        draw_frame();
        if (papp_mp_ready) {
            display_frame_done_generic();  // scrolling, frame callbacks, LVGL
        }
        frames++;
        if (now - fps_window >= 1000000) {
            reported_fps = (float)frames * 1000000.0f / (float)(now - fps_window);
            fps_window = now;
            frames = 0;
        }
        // Always sleep at least one tick so ESPHome's loop on this core runs.
        next += FRAME_US;
        int64_t wait = next - papp_time_us();
        if (wait < 10000) {
            wait = 10000;
            next = papp_time_us() + wait;
        }
        papp_svc->delay_ms((int)(wait / 1000));
    }
    s_stopped = 1;
    for (;;) {
        papp_svc->delay_ms(1000);  // deleted by papp_display_stop
    }
}

// ── Display hooks Tulip's shared code expects from its platform ─────────────

void unix_display_set_clock(uint8_t mhz)
{
    PIXEL_CLOCK_MHZ = mhz;  // the loader's panel has a fixed timing
}

void unix_display_timings(uint16_t t0, uint16_t t1, uint16_t t2, uint16_t t3)
{
    (void)t0;
    (void)t1;
    (void)t2;
    (void)t3;
}

int papp_display_start(void)
{
    display_init();
    for (int i = 0; i < 256; i++) {
        uint8_t r, g, b;
        unpack_rgb_332_repeat((uint8_t)i, &r, &g, &b);
        s_lut[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    s_band = (uint8_t *)papp_alloc_raw(H_RES * FONT_HEIGHT, 1);
    if (s_band == NULL) {
        s_band = (uint8_t *)papp_alloc_raw(H_RES * FONT_HEIGHT, 0);
    }
    if (s_band == NULL || papp_svc->display_get_framebuffer == NULL || papp_svc->display_get_framebuffer() == NULL) {
        papp_svc->log_printf("TULIP: no display (framebuffer %p)\n", (void *)s_band);
        return -1;
    }
    papp_svc->display_clear(0x0000);
    s_stop = 0;
    s_stopped = 0;
    // Core 1 (ESPHome's loop and the loader's display flush live there too),
    // above the loop's priority; it sleeps between frames.
    if (papp_svc->task_create(display_task, "tulip_disp", 16 * 1024, NULL, 4, &s_task, 1) != 0) {
        s_task = NULL;
        s_stopped = 1;
        papp_svc->log_printf("TULIP: could not create the display task\n");
        return -1;
    }
    papp_svc->log_printf("TULIP: display %dx%d RGB332 -> RGB565, %d fps target\n", H_RES, V_RES, 1000000 / FRAME_US);
    return 0;
}

void papp_display_stop(void)
{
    if (s_task != NULL) {
        s_stop = 1;
        for (int i = 0; i < 100 && !s_stopped; i++) {
            papp_svc->delay_ms(10);
        }
        papp_svc->task_delete(s_task);
        s_task = NULL;
    }
    papp_free_raw(s_band);
    s_band = NULL;
}
