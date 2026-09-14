// psram_video: entry point, input and the file list.
//
// app_entry runs on the loader's worker task. It takes the canvas the loader
// offers (the whole panel, 1024x600, when it can), then plays the argument it
// was launched with (app_get_arg: a path or URL, e.g. from NetSurf) and
// quits, or, launched from the store with none, shows the videos in
// /sd/videos/ and /sd/ to pick from.
#include "papp_player.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const app_services_t *papp_svc = NULL;

#define WHITE 0xFFFF
#define GREY RGB565(150, 150, 150)
#define ACCENT RGB565(56, 189, 248)
#define HEADER RGB565(17, 28, 51)
#define ROW_SEL RGB565(30, 60, 100)

// ── Input ─────────────────────────────────────────────────────────────────

void papp_input_reset(papp_input_t *in)
{
    memset(in, 0, sizeof(*in));
    // Buttons already down (the one that launched us) count as held, not new.
    if (papp_svc->input_gamepad_read != NULL) {
        papp_svc->input_gamepad_read(&in->pad);
        in->prev = in->pad;
    }
    papp_keyboard_event_t ev;
    while (papp_svc->input_keyboard_read != NULL && papp_svc->input_keyboard_read(&ev)) {
    }
}

void papp_input_poll(papp_input_t *in)
{
    const int64_t t = papp_time_us();
    in->prev = in->pad;
    memset(&in->pad, 0, sizeof(in->pad));
    if (papp_svc->input_gamepad_read != NULL) {
        papp_svc->input_gamepad_read(&in->pad);
    }
    in->nkeys = 0;
    papp_keyboard_event_t ev;
    while (papp_svc->input_keyboard_read != NULL && papp_svc->input_keyboard_read(&ev)) {
        if (ev.down && in->nkeys < PAPP_MAX_KEYS) {
            in->keys[in->nkeys++] = ev.key;
        }
    }

    // Touch: a tap is a touch lifted without having moved.
    int x = 0, y = 0;
    const bool down = papp_svc->touch_read != NULL && papp_svc->touch_read(&x, &y);
    in->tap = false;
    in->drag_dy = 0;
    if (down) {
        if (!in->touching) {
            in->start_x = x;
            in->start_y = y;
            in->moved = false;
        } else {
            in->drag_dy = y - in->y;
        }
        if (abs(x - in->start_x) > 12 || abs(y - in->start_y) > 12) {
            in->moved = true;
        }
        in->x = x;
        in->y = y;
    } else if (in->touching && !in->moved) {
        in->tap = true;
        in->tap_x = in->start_x;
        in->tap_y = in->start_y;
    }
    in->touching = down;

    // Leaving the app: Menu held 3 s, Menu + X together (what the loader's
    // close control holds), or L3 held half a second.
    const bool menu = in->pad.values[PAPP_INPUT_MENU] != 0;
    in->menu_since = menu ? (in->menu_since ? in->menu_since : t) : 0;
    const bool l3 = papp_svc->input_l3_read != NULL && papp_svc->input_l3_read();
    in->l3_since = l3 ? (in->l3_since ? in->l3_since : t) : 0;
    if ((menu && t - in->menu_since > 3000000) || (menu && in->pad.values[PAPP_INPUT_X]) ||
        (l3 && t - in->l3_since > 500000)) {
        in->quit_app = true;
    }
}

bool papp_button(papp_input_t *in, int button, int repeat_ms)
{
    const int64_t t = papp_time_us();
    const bool now = in->pad.values[button] != 0, before = in->prev.values[button] != 0;
    if (now && !before) {
        in->repeat_at[button] = t + 400000;
        return true;
    }
    if (now && repeat_ms > 0 && t >= in->repeat_at[button]) {
        in->repeat_at[button] = t + (int64_t)repeat_ms * 1000;
        return true;
    }
    return false;
}

bool papp_key(const papp_input_t *in, int key)
{
    for (int i = 0; i < in->nkeys; i++) {
        if (in->keys[i] == key) {
            return true;
        }
    }
    return false;
}

// ── The file list ─────────────────────────────────────────────────────────

#define LIST_BYTES 32768
#define MAX_ENTRIES 512

typedef struct {
    char dir[256];           // with a trailing '/'
    char *names;             // file_list_dir's output
    const char *entry[MAX_ENTRIES];
    int count;
    int sel, top;
    char typed[PAPP_APP_ARG_MAX];  // a URL being typed
    int typed_len;
    bool typing;
    char note[96];
} browser_t;

static const char *const k_media[] = {".mp4", ".m4v", ".mov", ".3gp", ".mkv", ".webm", ".avi", ".ts", ".mts",
                                      ".m2ts", ".mp3", ".ogg", ".ogv", ".oga", ".opus", ".wav", ".m4a", NULL};

static bool is_media(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }
    for (int i = 0; k_media[i] != NULL; i++) {
        if (strcasecmp(dot, k_media[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_dir(const char *name)
{
    const size_t n = strlen(name);
    return n > 0 && name[n - 1] == '/';
}

static int by_name(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    if (strcmp(x, "../") == 0) {
        return -1;
    }
    if (strcmp(y, "../") == 0) {
        return 1;
    }
    if (is_dir(x) != is_dir(y)) {
        return is_dir(x) ? -1 : 1;
    }
    return strcasecmp(x, y);
}

static bool load_dir(browser_t *b, const char *dir)
{
    if (papp_svc->file_list_dir == NULL) {
        snprintf(b->note, sizeof(b->note), "This loader cannot list folders; type a URL or path");
        b->count = 0;
        return false;
    }
    const int n = papp_svc->file_list_dir(dir, b->names, LIST_BYTES);
    if (n < 0) {
        return false;
    }
    snprintf(b->dir, sizeof(b->dir), "%s", dir);
    const size_t len = strlen(b->dir);
    if (len == 0 || b->dir[len - 1] != '/') {
        strncat(b->dir, "/", sizeof(b->dir) - len - 1);
    }
    b->count = 0;
    if (strcmp(b->dir, "/sd/") != 0) {
        b->entry[b->count++] = "../";
    }
    const char *p = b->names;
    for (int i = 0; i < n && b->count < MAX_ENTRIES; i++, p += strlen(p) + 1) {
        if (p[0] == '.') {
            continue;  // hidden files and folders
        }
        if (is_dir(p) || is_media(p)) {
            b->entry[b->count++] = p;
        }
    }
    qsort(b->entry, (size_t)b->count, sizeof(b->entry[0]), by_name);
    b->sel = 0;
    b->top = 0;
    snprintf(b->note, sizeof(b->note), "%s", b->count == 0 ? "No videos here" : "");
    return true;
}

static void parent_dir(browser_t *b)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", b->dir);
    size_t len = strlen(dir);
    if (len > 1 && dir[len - 1] == '/') {
        dir[--len] = '\0';
    }
    char *slash = strrchr(dir, '/');
    if (slash != NULL && slash != dir) {
        slash[1] = '\0';
        if (strlen(dir) < 4) {
            snprintf(dir, sizeof(dir), "/sd/");
        }
        load_dir(b, dir);
    }
}

typedef struct {
    int row_h, list_y, rows;
} list_geom_t;

static list_geom_t geom(int ch)
{
    list_geom_t g;
    g.row_h = 44;
    g.list_y = 64;
    g.rows = (ch - g.list_y - 48) / g.row_h;
    return g;
}

static void draw_browser(browser_t *b, int cw, int ch)
{
    papp_canvas_t c = {papp_svc->display_get_framebuffer(), cw, ch, cw};
    if (c.px == NULL) {
        return;
    }
    const list_geom_t g = geom(ch);
    papp_fill(&c, 0, 0, cw, ch, RGB565(8, 17, 31));
    papp_fill(&c, 0, 0, cw, 52, HEADER);
    int x = papp_text(&c, 16, 10, "Video Player", 2, WHITE);
    papp_text_fit(&c, x + 24, 10, b->typing ? "Open a URL or path" : b->dir, 2, GREY, cw - x - 40);

    if (b->typing) {
        papp_fill(&c, 16, g.list_y + 8, cw - 32, 48, RGB565(30, 41, 59));
        char shown[128];
        const char *t = b->typed;
        const int fits = (cw - 64) / 16 - 1;
        if (b->typed_len > fits) {
            t += b->typed_len - fits;  // the end of a long URL
        }
        snprintf(shown, sizeof(shown), "%s_", t);
        papp_text(&c, 28, g.list_y + 16, shown, 2, WHITE);
        papp_text(&c, 16, g.list_y + 80, "Enter: play   Esc: cancel   Backspace: delete", 2, GREY);
    } else {
        if (b->sel < b->top) {
            b->top = b->sel;
        }
        if (b->sel >= b->top + g.rows) {
            b->top = b->sel - g.rows + 1;
        }
        for (int r = 0; r < g.rows && b->top + r < b->count; r++) {
            const int i = b->top + r;
            const int y = g.list_y + r * g.row_h;
            if (i == b->sel) {
                papp_fill(&c, 8, y, cw - 16, g.row_h - 4, ROW_SEL);
            }
            const char *name = b->entry[i];
            const bool dir = is_dir(name);
            papp_text(&c, 20, y + 10, strcmp(name, "../") == 0 ? "<" : (dir ? ">" : " "), 2, ACCENT);
            papp_text_fit(&c, 52, y + 10, strcmp(name, "../") == 0 ? "(up)" : name, 2, dir ? ACCENT : WHITE,
                          cw - 72);
        }
        if (b->count > g.rows) {
            // A scroll bar at the right edge.
            const int track = g.rows * g.row_h;
            const int h = track * g.rows / b->count;
            const int y = g.list_y + (track - h) * b->top / (b->count - g.rows);
            papp_fill(&c, cw - 6, y, 4, h > 8 ? h : 8, GREY);
        }
        if (b->note[0] != '\0') {
            papp_text(&c, 20, g.list_y + 12, b->note, 2, GREY);
        }
    }
    papp_fill(&c, 0, ch - 40, cw, 40, HEADER);
    papp_text_fit(&c, 16, ch - 30,
                  b->typing ? "Type the address" : "Tap or A: play   B/Esc: quit   Keyboard: type a URL", 2, GREY,
                  cw - 32);
    papp_svc->display_flush();
}

// The chosen video in out; false when the user leaves the app.
static bool browse(browser_t *b, char *out, size_t out_len, int cw, int ch, papp_input_t *in)
{
    bool dirty = true;
    const list_geom_t g = geom(ch);
    int drag_acc = 0;
    for (;;) {
        if (dirty) {
            draw_browser(b, cw, ch);
            dirty = false;
        }
        papp_sleep_ms(20);
        papp_input_poll(in);
        if (in->quit_app) {
            return false;
        }
        if (b->typing) {
            for (int i = 0; i < in->nkeys; i++) {
                const int k = in->keys[i];
                if (k == 27) {
                    b->typing = false;
                } else if (k == 13 && b->typed_len > 0) {
                    b->typing = false;
                    snprintf(out, out_len, "%s", b->typed);
                    return true;
                } else if ((k == 127 || k == 8) && b->typed_len > 0) {
                    b->typed[--b->typed_len] = '\0';
                } else if (k >= 32 && k < 127 && b->typed_len < (int)sizeof(b->typed) - 1) {
                    b->typed[b->typed_len++] = (char)k;
                    b->typed[b->typed_len] = '\0';
                }
                dirty = true;
            }
            continue;  // (letters also press gamepad buttons: those are ignored while typing)
        }
        if (papp_button(in, PAPP_INPUT_B, 0) || papp_key(in, 27)) {
            return false;
        }
        // Keyboard: letters and '/' start typing an address; Backspace goes up.
        for (int i = 0; i < in->nkeys; i++) {
            const int k = in->keys[i];
            if (k == 127 || k == 8) {
                parent_dir(b);
                dirty = true;
            } else if (k > 32 && k < 127 && (isalpha(k) || k == '/') && k != 'x' && k != 'c' && k != 'v' &&
                       k != 'q' && k != 'e' && k != 'w' && k != 'a' && k != 's' && k != 'd' && k != 'z') {
                // (Letters that double as gamepad buttons start nothing.)
                b->typing = true;
                b->typed_len = 0;
                b->typed[0] = (char)k;
                b->typed[1] = '\0';
                b->typed_len = 1;
                dirty = true;
            }
        }
        if (b->typing) {
            continue;
        }
        if (papp_button(in, PAPP_INPUT_UP, 150) && b->sel > 0) {
            b->sel--;
            dirty = true;
        }
        if (papp_button(in, PAPP_INPUT_DOWN, 150) && b->sel + 1 < b->count) {
            b->sel++;
            dirty = true;
        }
        if (papp_button(in, PAPP_INPUT_L, 0) || papp_button(in, PAPP_INPUT_LEFT, 0)) {
            b->sel = b->sel > g.rows ? b->sel - g.rows : 0;
            dirty = true;
        }
        if (papp_button(in, PAPP_INPUT_R, 0) || papp_button(in, PAPP_INPUT_RIGHT, 0)) {
            b->sel = b->sel + g.rows < b->count ? b->sel + g.rows : (b->count > 0 ? b->count - 1 : 0);
            dirty = true;
        }
        // Touch: drag to scroll, tap a row.
        if (in->touching && in->moved) {
            drag_acc += in->drag_dy;
            while (drag_acc <= -g.row_h && b->top + g.rows < b->count) {
                b->top++;
                drag_acc += g.row_h;
                dirty = true;
            }
            while (drag_acc >= g.row_h && b->top > 0) {
                b->top--;
                drag_acc -= g.row_h;
                dirty = true;
            }
            if (b->sel < b->top) {
                b->sel = b->top;
            }
            if (b->sel >= b->top + g.rows) {
                b->sel = b->top + g.rows - 1;
            }
        } else if (!in->touching) {
            drag_acc = 0;
        }
        bool open = papp_button(in, PAPP_INPUT_A, 0) || papp_button(in, PAPP_INPUT_START, 0) || papp_key(in, 13);
        if (in->tap && in->tap_y >= g.list_y) {
            const int r = (in->tap_y - g.list_y) / g.row_h;
            if (r < g.rows && b->top + r < b->count) {
                b->sel = b->top + r;
                open = true;
            }
        }
        if (open && b->count > 0) {
            const char *name = b->entry[b->sel];
            if (strcmp(name, "../") == 0) {
                parent_dir(b);
            } else if (is_dir(name)) {
                char dir[256];
                snprintf(dir, sizeof(dir), "%s%s", b->dir, name);
                if (!load_dir(b, dir)) {
                    snprintf(b->note, sizeof(b->note), "Cannot open that folder");
                }
            } else {
                snprintf(out, out_len, "%s%s", b->dir, name);
                return true;
            }
            dirty = true;
        }
    }
}

// ── Entry ─────────────────────────────────────────────────────────────────

__attribute__((section(".text.entry"), used)) int app_entry(const app_services_t *svc)
{
    papp_svc = svc;
    papp_syscalls_init();
    svc->log_printf("VIDEO: psram_video starting (ABI %u)\n", (unsigned)svc->abi_version);

    // The whole panel when the loader offers it, else 800x480.
    int cw = 800, ch = 480;
    if (svc->display_get_size != NULL && svc->display_set_canvas != NULL) {
        svc->display_get_size(&cw, &ch);
        if (cw < 320 || ch < 240 || svc->display_set_canvas(cw, ch) != 0) {
            cw = 800;
            ch = 480;
        }
    }
    papp_log("canvas %dx%d", cw, ch);

    static papp_input_t in;
    papp_input_reset(&in);

    static char arg[PAPP_APP_ARG_MAX];
    const int len = svc->app_get_arg != NULL ? svc->app_get_arg(arg, sizeof(arg)) : 0;
    if (len > 0 && len < (int)sizeof(arg)) {
        // Opened by another app (NetSurf): play that and go back to it.
        papp_play(arg, cw, ch, &in);
    } else {
        browser_t *b = calloc(1, sizeof(*b));
        char *names = malloc(LIST_BYTES);
        if (b != NULL && names != NULL) {
            b->names = names;
            if (!load_dir(b, "/sd/videos/") && !load_dir(b, "/sd/")) {
                snprintf(b->dir, sizeof(b->dir), "/sd/");
            }
            static char chosen[PAPP_APP_ARG_MAX];
            while (!papp_fatal_hit && browse(b, chosen, sizeof(chosen), cw, ch, &in)) {
                papp_input_reset(&in);  // the button that chose it is not a new press
                if (papp_play(chosen, cw, ch, &in) == PLAY_QUIT_APP) {
                    break;
                }
                papp_input_reset(&in);
            }
        }
        free(names);
        free(b);
    }

    papp_log("quitting%s", papp_fatal_hit ? " after an error" : "");
    papp_free_all_memory();
    papp_syscalls_deinit();
    svc->log_printf("VIDEO: bye\n");
    return papp_fatal_hit ? papp_fatal_code : 0;
}
