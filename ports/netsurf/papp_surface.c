// libnsfb surface "papp": NetSurf's framebuffer frontend drawing straight
// into the loader's RGB565 canvas (the whole panel when the loader offers
// it, else 800x480), with the loader's USB keyboard,
// USB mouse, touch panel and gamepad as its input.
//
// Display: libnsfb's 16 bpp plotters write into the canvas the loader
// returns from display_get_framebuffer(). update() only marks it dirty;
// input(), which NetSurf's main loop calls between redraws, pushes the canvas
// to the panel with display_flush() at most every FRAME_US, and every
// REFRESH_US even when nothing changed. The mouse pointer
// is drawn into the canvas like libnsfb's SDL surface does (claim() lifts it
// before a redraw, update() puts it back); touch input hides it.
//
// Input becomes nsfb events:
//   keyboard  the loader queues keys as taps: printable keys as ASCII (sent
//             as that keycode; patches/0005 makes fbtk map them 1:1), others
//             as its own numbers (mapped below). Ctrl and Alt arrive as their
//             own taps and apply to the next key (Ctrl-L, Ctrl-+/-/0, Alt+Left).
//   arrows    only exist as the loader's held D-pad, which W/A/S/D also press:
//             a direction is an arrow key (with repeat) unless its letter was
//             typed around then (as the Tulip port does).
//   mouse     relative moves, left/right buttons; the middle button held
//             turns movement into scrolling.
//   touch     a tap is a click where the finger went down, on the release or
//             once the finger has been still for TOUCH_HOLD_CLICK_US; a drag
//             in the page scrolls it (a wheel event carrying the distance,
//             patches/0003); on the scroll bars it drags them; the toolbar
//             only takes taps. Every touch is logged (down, click, up).
//   gamepad   A click, B back, Y reload, Select URL bar, Start Enter, L/R page
//             up/down, while no USB keyboard is in use (the loader also
//             reports some keyboard keys and mouse buttons as these buttons).
//   quit      Menu (Escape on a keyboard) held 3 s, Menu + X, or the loader's
//             close control: NSFB_CONTROL_QUIT, and NetSurf shuts down.
#include "papp_port.h"

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_plot_util.h"
#include "libnsfb_cursor.h"
#include "libnsfb_event.h"

#include "cursor.h"
#include "nsfb.h"
#include "plot.h"
#include "surface.h"

#include "utils/nsoption.h"

// The loader's canvas (papp_main.c): 1024x600 on a full panel, else 800x480;
// its framebuffer's stride is its width.
#define FB_W papp_canvas_w
#define FB_H papp_canvas_h
#define FRAME_US 33000        // flush the canvas at most ~30 times a second
#define DEVICE_POLL_US 8000   // touch, mouse and gamepad at most this often

// ── Event queue ─────────────────────────────────────────────────────────────

#define QUEUE_LEN 128
static nsfb_event_t s_queue[QUEUE_LEN];
static unsigned s_head, s_tail;

static void push(const nsfb_event_t *ev)
{
    if (s_head - s_tail < QUEUE_LEN) {
        s_queue[s_head++ % QUEUE_LEN] = *ev;
    }
}

static bool pop(nsfb_event_t *ev)
{
    if (s_head == s_tail) {
        return false;
    }
    *ev = s_queue[s_tail++ % QUEUE_LEN];
    return true;
}

static void push_key(bool down, int code)
{
    nsfb_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
    ev.value.keycode = (enum nsfb_key_code_e)code;
    push(&ev);
}

static void push_tap(int code)
{
    push_key(true, code);
    push_key(false, code);
}

static void push_move(bool absolute, int x, int y)
{
    nsfb_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = absolute ? NSFB_EVENT_MOVE_ABSOLUTE : NSFB_EVENT_MOVE_RELATIVE;
    ev.value.vector.x = x;
    ev.value.vector.y = y;
    push(&ev);
}

// A wheel event that carries its distance: vector.y (down positive) and
// vector.z (right positive) next to the keycode, which shares the union with
// vector.x. The patched browser window click handler scrolls by exactly that.
static void push_scroll(int dx, int dy)
{
    nsfb_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = NSFB_EVENT_KEY_DOWN;
    ev.value.keycode = dy < 0 ? NSFB_KEY_MOUSE_4 : NSFB_KEY_MOUSE_5;
    ev.value.vector.y = dy;
    ev.value.vector.z = dx;
    push(&ev);
}

// ── Display ─────────────────────────────────────────────────────────────────

static bool s_live = false;   // the canvas is NetSurf's (between initialise and finalise)
static bool s_dirty = false;
static int64_t s_last_flush = 0;
static bool s_cursor_visible = true;  // off after touch input, on after mouse movement

// The canvas goes to the panel when NetSurf drew into it, and every
// REFRESH_US even when nothing changed. NetSurf can sit idle for a long time
// (waiting for the network, say); if anything else painted the panel meanwhile
// (the loader's own LVGL layer is black), the page must come back by itself.
#define REFRESH_US 500000

static void flush(bool force)
{
    if (!s_live) {
        return;
    }
    const int64_t now = papp_time_us();
    if (s_dirty ? (!force && now - s_last_flush < FRAME_US) : (!force && now - s_last_flush < REFRESH_US)) {
        return;
    }
    papp_svc->display_flush();
    s_dirty = false;
    s_last_flush = now;
}

static int papp_defaults(nsfb_t *nsfb)
{
    nsfb->width = FB_W;
    nsfb->height = FB_H;
    nsfb->format = NSFB_FMT_RGB565;
    select_plotters(nsfb);
    return 0;
}

// Only RGB565, at most the loader's canvas.
static int papp_set_geometry(nsfb_t *nsfb, int width, int height, enum nsfb_format_e format)
{
    (void)format;
    nsfb->width = (width > 0 && width <= FB_W) ? width : FB_W;
    nsfb->height = (height > 0 && height <= FB_H) ? height : FB_H;
    nsfb->format = NSFB_FMT_RGB565;
    select_plotters(nsfb);
    if (nsfb->ptr != NULL) {
        nsfb->linelen = FB_W * 2;
    }
    return 0;
}

static int papp_initialise(nsfb_t *nsfb)
{
    uint16_t *canvas = papp_svc->display_get_framebuffer != NULL ? papp_svc->display_get_framebuffer() : NULL;
    if (canvas == NULL) {
        papp_svc->log_printf("NETSURF: the loader has no framebuffer\n");
        return -1;
    }
    nsfb->ptr = (uint8_t *)canvas;
    nsfb->linelen = FB_W * 2;
    memset(canvas, 0, FB_W * FB_H * 2);
    s_live = true;
    s_dirty = true;
    flush(true);
    return 0;
}

static int papp_finalise(nsfb_t *nsfb)
{
    if (nsfb->ptr != NULL) {
        memset(nsfb->ptr, 0, FB_W * FB_H * 2);
        s_dirty = true;
        flush(true);
    }
    s_live = false;
    nsfb->ptr = NULL;
    return 0;
}

static int papp_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct nsfb_cursor_s *cursor = nsfb->cursor;
    if (cursor != NULL && cursor->plotted && nsfb_plot_bbox_intersect(box, &cursor->loc)) {
        nsfb_cursor_clear(nsfb, cursor);
    }
    return 0;
}

static int papp_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    (void)box;
    struct nsfb_cursor_s *cursor = nsfb->cursor;
    if (cursor != NULL && !cursor->plotted && s_cursor_visible) {
        nsfb_cursor_plot(nsfb, cursor);
    }
    s_dirty = true;
    return 0;
}

static int papp_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
    if (cursor == NULL) {
        return 0;
    }
    if (cursor->plotted) {
        nsfb_cursor_clear(nsfb, cursor);
        s_dirty = true;
    }
    if (s_cursor_visible) {
        nsfb_cursor_plot(nsfb, cursor);
        s_dirty = true;
    }
    return 0;
}

static void show_cursor(nsfb_t *nsfb, bool visible)
{
    if (s_cursor_visible == visible) {
        return;
    }
    s_cursor_visible = visible;
    if (nsfb->cursor != NULL) {
        if (!visible && nsfb->cursor->plotted) {
            nsfb_cursor_clear(nsfb, nsfb->cursor);
            s_dirty = true;
        } else if (visible && !nsfb->cursor->plotted) {
            nsfb_cursor_plot(nsfb, nsfb->cursor);
            s_dirty = true;
        }
    }
}

// ── Keyboard ────────────────────────────────────────────────────────────────
// The loader's numbers for keys that are not ASCII (papp_loader.cpp
// enqueue_keyboard_text).
enum {
    LK_TAB = 9,
    LK_ENTER = 13,
    LK_ESCAPE = 27,
    LK_BACKSPACE = 127,
    LK_ALT = 132,
    LK_CTRL = 133,
    LK_SHIFT = 134,
    LK_F1 = 135,  // .. F12 = 146
    LK_INSERT = 147,
    LK_DELETE = 148,
    LK_PAGE_DOWN = 149,
    LK_PAGE_UP = 150,
    LK_HOME = 151,
    LK_END = 152,
};

// Keys the patched frontend (patches/0003) turns into browser actions.
#define KEY_BACK NSFB_KEY_F13
#define KEY_FORWARD NSFB_KEY_F14
#define KEY_RELOAD NSFB_KEY_F5
#define KEY_URL_BAR NSFB_KEY_F6

#define MODIFIER_STICKY_US 1500000
static int64_t s_ctrl_at = 0;
static int64_t s_alt_at = 0;
static int64_t s_key_at = 0;    // last keyboard event
static bool s_keyboard_seen = false;

enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT, DIR_COUNT };
static const int s_dir_input[DIR_COUNT] = {PAPP_INPUT_UP, PAPP_INPUT_DOWN, PAPP_INPUT_LEFT, PAPP_INPUT_RIGHT};
static const int s_dir_key[DIR_COUNT] = {NSFB_KEY_UP, NSFB_KEY_DOWN, NSFB_KEY_LEFT, NSFB_KEY_RIGHT};
static const char s_dir_letter[DIR_COUNT] = {'w', 's', 'a', 'd'};
static int64_t s_letter_at[DIR_COUNT];  // when W/A/S/D was last typed

static bool modifier_active(int64_t since, int64_t now)
{
    return since != 0 && now - since < MODIFIER_STICKY_US;
}

// One key for NetSurf, with Ctrl held around it when the sticky Ctrl applies.
static void send_key(int code, int64_t now)
{
    const bool ctrl = modifier_active(s_ctrl_at, now);
    const bool alt = modifier_active(s_alt_at, now);
    s_ctrl_at = 0;
    s_alt_at = 0;
    if (alt && code == NSFB_KEY_LEFT) {
        push_tap(KEY_BACK);
        return;
    }
    if (alt && code == NSFB_KEY_RIGHT) {
        push_tap(KEY_FORWARD);
        return;
    }
    if (ctrl) {
        if (code >= 'A' && code <= 'Z') {
            code += 'a' - 'A';
        } else if (code == '+') {
            code = NSFB_KEY_EQUALS;
        }
        push_key(true, NSFB_KEY_LCTRL);
        push_tap(code);
        push_key(false, NSFB_KEY_LCTRL);
        return;
    }
    push_tap(code);
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
        s_keyboard_seen = true;
        s_key_at = now;
        int key = ev.key;
        switch (key) {
        case LK_CTRL:
            s_ctrl_at = now;
            continue;
        case LK_ALT:
            s_alt_at = now;
            continue;
        case LK_SHIFT:
        case LK_INSERT:
            continue;  // printable keys already arrive shifted
        case LK_BACKSPACE:
            key = NSFB_KEY_BACKSPACE;
            break;
        case LK_TAB:
            key = NSFB_KEY_TAB;
            break;
        case LK_ENTER:
            key = NSFB_KEY_RETURN;
            break;
        case LK_ESCAPE:
            key = NSFB_KEY_ESCAPE;
            break;
        case LK_DELETE:
            key = NSFB_KEY_DELETE;
            break;
        case LK_PAGE_DOWN:
            key = NSFB_KEY_PAGEDOWN;
            break;
        case LK_PAGE_UP:
            key = NSFB_KEY_PAGEUP;
            break;
        case LK_HOME:
            key = NSFB_KEY_HOME;
            break;
        case LK_END:
            key = NSFB_KEY_END;
            break;
        default:
            if (key >= LK_F1 && key < LK_F1 + 12) {
                key = NSFB_KEY_F1 + (key - LK_F1);
                break;
            }
            if (key < 32 || key > 126) {
                continue;
            }
            for (int d = 0; d < DIR_COUNT; d++) {
                if (key == s_dir_letter[d] || key == s_dir_letter[d] - 32) {
                    s_letter_at[d] = now;
                }
            }
            break;
        }
        send_key(key, now);
    }
}

// ── Gamepad: directions, buttons, quitting ──────────────────────────────────

#define DECIDE_US 60000        // wait this long for a matching keyboard tap
#define TYPED_NEAR_US 250000   // a key this close to the press means typing
#define REPEAT_DELAY_US 400000
#define REPEAT_EVERY_US 60000
#define MENU_QUIT_US 3000000

static int64_t s_dir_down_at[DIR_COUNT];  // 0: not held
static int s_dir_state[DIR_COUNT];        // 0 undecided, 1 arrow, 2 typing
static int64_t s_dir_next[DIR_COUNT];
static int64_t s_menu_since = 0;

// Buttons: pressed after DECIDE_US unless a keyboard key explains the press.
// The loader also reports keyboard keys (Space, Enter, X, V, Q, E, Backspace)
// and the mouse's left/right buttons as these buttons, so once a USB
// keyboard has been used the buttons are left alone, and A/B once the mouse
// has clicked.
enum { BTN_A, BTN_B, BTN_Y, BTN_START, BTN_SELECT, BTN_L, BTN_R, BTN_COUNT };
static const int s_btn_input[BTN_COUNT] = {PAPP_INPUT_A, PAPP_INPUT_B, PAPP_INPUT_Y, PAPP_INPUT_START,
                                           PAPP_INPUT_SELECT, PAPP_INPUT_L, PAPP_INPUT_R};
static int64_t s_btn_down_at[BTN_COUNT];
static int s_btn_state[BTN_COUNT];  // 0 undecided, 1 acted on, 2 ignored
static int s_mouse_buttons = 0;
static bool s_mouse_clicked = false;

static bool typed_near(int64_t at)
{
    return s_key_at != 0 && s_key_at - at < TYPED_NEAR_US && at - s_key_at < TYPED_NEAR_US;
}

static void button_action(int b, bool down)
{
    switch (b) {
    case BTN_A:
        push_key(down, NSFB_KEY_MOUSE_1);  // a click at the pointer, held as long as A
        break;
    case BTN_B:
        if (down) {
            push_tap(KEY_BACK);
        }
        break;
    case BTN_Y:
        if (down) {
            push_tap(KEY_RELOAD);
        }
        break;
    case BTN_START:
        if (down) {
            push_tap(NSFB_KEY_RETURN);
        }
        break;
    case BTN_SELECT:
        if (down) {
            push_tap(KEY_URL_BAR);
        }
        break;
    case BTN_L:
        if (down) {
            push_tap(NSFB_KEY_PAGEUP);
        }
        break;
    case BTN_R:
        if (down) {
            push_tap(NSFB_KEY_PAGEDOWN);
        }
        break;
    default:
        break;
    }
}

static bool button_from_mouse(int b)
{
    return (b == BTN_A || b == BTN_B) && (s_mouse_clicked || (s_mouse_buttons & 3) != 0);
}

static void poll_gamepad(int64_t now)
{
    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    if (papp_svc->input_gamepad_read != NULL) {
        papp_svc->input_gamepad_read(&pad);
    }

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
                if (s_letter_at[d] == 0 || since_letter >= TYPED_NEAR_US || since_letter <= -TYPED_NEAR_US) {
                    send_key(s_dir_key[d], now);
                }
            }
            s_dir_down_at[d] = 0;
            continue;
        }
        if (s_dir_down_at[d] == 0) {
            s_dir_down_at[d] = now;
            s_dir_state[d] = 0;
        }
        if (s_dir_state[d] == 0 && now - s_dir_down_at[d] >= DECIDE_US) {
            const int64_t since_letter = s_dir_down_at[d] - s_letter_at[d];
            const bool typing = s_letter_at[d] != 0 && since_letter < TYPED_NEAR_US && since_letter > -TYPED_NEAR_US;
            s_dir_state[d] = typing ? 2 : 1;
            if (!typing) {
                send_key(s_dir_key[d], now);
                s_dir_next[d] = now + REPEAT_DELAY_US;
            }
        } else if (s_dir_state[d] == 1 && now >= s_dir_next[d]) {
            send_key(s_dir_key[d], now);
            s_dir_next[d] = now + REPEAT_EVERY_US;
        }
    }

    for (int b = 0; b < BTN_COUNT; b++) {
        const bool held = pad.values[s_btn_input[b]] != 0;
        if (held && s_btn_down_at[b] == 0) {
            s_btn_down_at[b] = now;
            s_btn_state[b] = 0;
        }
        if (s_btn_down_at[b] == 0) {
            continue;
        }
        const bool decide = s_btn_state[b] == 0 && (!held || now - s_btn_down_at[b] >= DECIDE_US);
        if (decide) {
            const bool ignore = s_keyboard_seen || typed_near(s_btn_down_at[b]) || button_from_mouse(b);
            s_btn_state[b] = ignore ? 2 : 1;
            if (!ignore) {
                button_action(b, true);
            }
        }
        if (!held) {
            if (s_btn_state[b] == 1) {
                button_action(b, false);
            }
            s_btn_down_at[b] = 0;
        }
    }
}

// ── Mouse ───────────────────────────────────────────────────────────────────

static void poll_mouse(nsfb_t *nsfb)
{
    if (papp_svc->input_mouse_read == NULL) {
        return;
    }
    int dx = 0, dy = 0, buttons = 0;
    if (!papp_svc->input_mouse_read(&dx, &dy, &buttons)) {
        return;
    }
    const int changed = buttons ^ s_mouse_buttons;
    if (buttons & 3) {
        s_mouse_clicked = true;
    }
    if (buttons & 4) {
        // Middle button held: scroll instead of moving the pointer.
        if (dx != 0 || dy != 0) {
            push_scroll(dx * 3, dy * 3);
        }
    } else if (dx != 0 || dy != 0) {
        show_cursor(nsfb, true);
        push_move(false, dx, dy);
    }
    if (changed & 1) {
        show_cursor(nsfb, true);
        push_key((buttons & 1) != 0, NSFB_KEY_MOUSE_1);
    }
    if (changed & 2) {
        show_cursor(nsfb, true);
        push_key((buttons & 2) != 0, NSFB_KEY_MOUSE_3);
    }
    s_mouse_buttons = buttons;
}

// ── Touch ───────────────────────────────────────────────────────────────────

#define TOUCH_SLOP 12             // pixels a finger may wander before a tap becomes a scroll
#define TOUCH_HOLD_CLICK_US 700000  // a finger held still this long clicks without waiting for the release

enum { TOUCH_PAGE, TOUCH_DRAG, TOUCH_TAP };
static const char *const s_touch_mode_name[] = {"page", "scroll bar", "toolbar"};

static struct {
    bool down;
    int mode;
    int x0, y0;  // where the finger went down
    int x, y;    // where it was last seen
    int64_t since;
    bool scrolling;
    bool clicked;            // the click went out while the finger was still down
    bool long_logged;
    int scroll_x, scroll_y;  // distance not yet sent
    int scrolled_x, scrolled_y;
} s_touch;

// A tap: the pointer to where the finger went down, then a click there.
static void touch_click(const char *why)
{
    push_move(true, s_touch.x0, s_touch.y0);
    push_tap(NSFB_KEY_MOUSE_1);
    s_touch.clicked = true;
    papp_svc->log_printf("NETSURF: touch %s: click at (%d,%d)\n", why, s_touch.x0, s_touch.y0);
}

// The frontend's layout: toolbar on top, vertical scroll bar on the right,
// status line and horizontal scroll bar at the bottom.
static int touch_mode(int x, int y)
{
    const int toolbar = nsoption_int(fb_toolbar_size);
    const int furniture = nsoption_int(fb_furniture_size);
    if (y < toolbar) {
        return TOUCH_TAP;
    }
    if (x >= FB_W - furniture || y >= FB_H - furniture) {
        return TOUCH_DRAG;
    }
    return TOUCH_PAGE;
}

static void poll_touch(nsfb_t *nsfb)
{
    if (papp_svc->touch_read == NULL) {
        return;
    }
    int x = 0, y = 0;
    const bool down = papp_svc->touch_read(&x, &y) != 0;
    if (down) {
        x = x < 0 ? 0 : (x >= FB_W ? FB_W - 1 : x);
        y = y < 0 ? 0 : (y >= FB_H ? FB_H - 1 : y);
    }
    const int64_t now = papp_time_us();
    if (down && !s_touch.down) {
        memset(&s_touch, 0, sizeof(s_touch));
        s_touch.down = true;
        s_touch.mode = touch_mode(x, y);
        s_touch.x0 = s_touch.x = x;
        s_touch.y0 = s_touch.y = y;
        s_touch.since = now;
        papp_svc->log_printf("NETSURF: touch down at (%d,%d), %s\n", x, y, s_touch_mode_name[s_touch.mode]);
        show_cursor(nsfb, false);
        push_move(true, x, y);
        if (s_touch.mode == TOUCH_DRAG) {
            push_key(true, NSFB_KEY_MOUSE_1);
        }
    } else if (down) {
        int dx = x - s_touch.x, dy = y - s_touch.y;
        if (s_touch.mode == TOUCH_DRAG) {
            if (dx != 0 || dy != 0) {
                push_move(true, x, y);
            }
        } else if (!s_touch.clicked) {
            if (s_touch.mode == TOUCH_PAGE && !s_touch.scrolling &&
                (abs(x - s_touch.x0) > TOUCH_SLOP || abs(y - s_touch.y0) > TOUCH_SLOP)) {
                s_touch.scrolling = true;
                dx = x - s_touch.x0;
                dy = y - s_touch.y0;
            }
            if (s_touch.scrolling) {
                // Finger up: the page moves up, i.e. scrolls down.
                s_touch.scroll_x -= dx;
                s_touch.scroll_y -= dy;
            } else if (now - s_touch.since >= TOUCH_HOLD_CLICK_US) {
                // Held still: click now rather than waiting for a release
                // that the touch panel may report late or not at all.
                touch_click("held");
            }
        }
        s_touch.x = x;
        s_touch.y = y;
        if (!s_touch.long_logged && now - s_touch.since > 5000000) {
            s_touch.long_logged = true;  // a panel that never reports the release shows up here
            papp_svc->log_printf("NETSURF: touch still down after 5 s at (%d,%d)\n", x, y);
        }
    } else if (s_touch.down) {
        s_touch.down = false;
        if (s_touch.mode == TOUCH_DRAG) {
            push_key(false, NSFB_KEY_MOUSE_1);
            papp_svc->log_printf("NETSURF: touch up: scroll bar released\n");
        } else if (s_touch.scrolling) {
            papp_svc->log_printf("NETSURF: touch up: scrolled by (%d,%d)\n", s_touch.scrolled_x, s_touch.scrolled_y);
        } else if (!s_touch.clicked) {
            touch_click("tap");
        } else {
            papp_svc->log_printf("NETSURF: touch up\n");
        }
    }
    // One scroll event per poll carries everything the finger moved since.
    if (s_touch.scroll_x != 0 || s_touch.scroll_y != 0) {
        push_scroll(s_touch.scroll_x, s_touch.scroll_y);
        s_touch.scrolled_x += s_touch.scroll_x;
        s_touch.scrolled_y += s_touch.scroll_y;
        s_touch.scroll_x = 0;
        s_touch.scroll_y = 0;
    }
}

// ── Input ───────────────────────────────────────────────────────────────────

static int64_t s_last_device_poll = 0;

static void poll_devices(nsfb_t *nsfb)
{
    const int64_t now = papp_time_us();
    poll_keyboard(now);
    if (now - s_last_device_poll < DEVICE_POLL_US) {
        return;
    }
    s_last_device_poll = now;
    poll_mouse(nsfb);
    poll_touch(nsfb);
    poll_gamepad(now);
}

static int64_t s_last_input_return = 0;

static bool input_done(bool got)
{
    papp_yield_if_due();
    s_last_input_return = papp_time_us();
    return got;
}

static bool papp_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    const int64_t start = papp_time_us();
    // NetSurf works between these calls (layout, redraw, fetch callbacks);
    // a long gap is worth a line in the log.
    if (s_last_input_return != 0 && start - s_last_input_return > 1500000) {
        papp_svc->log_printf("NETSURF: busy for %d ms without reading input\n",
                             (int)((start - s_last_input_return) / 1000));
    }
    const int64_t deadline = timeout < 0 ? INT64_MAX : start + (int64_t)timeout * 1000;
    for (;;) {
        if (papp_quit_requested()) {
            flush(true);
            event->type = NSFB_EVENT_CONTROL;
            event->value.controlcode = NSFB_CONTROL_QUIT;
            return input_done(true);
        }
        poll_devices(nsfb);
        const bool got = pop(event);
        flush(false);
        if (got) {
            return input_done(true);
        }
        if (papp_time_us() >= deadline) {
            return input_done(false);
        }
        // One scheduler tick; the next scheduled NetSurf job or input ends the wait.
        papp_sleep_ms(10);
    }
}

const nsfb_surface_rtns_t papp_rtns = {
    .defaults = papp_defaults,
    .initialise = papp_initialise,
    .finalise = papp_finalise,
    .input = papp_input,
    .claim = papp_claim,
    .update = papp_update,
    .cursor = papp_cursor,
    .geometry = papp_set_geometry,
};

// Registered at start-up (papp_main.c runs the constructors) under the
// otherwise unused Linux surface type; NetSurf is started with "-f papp".
NSFB_SURFACE_DEF(papp, NSFB_SURFACE_LINUX, &papp_rtns)
