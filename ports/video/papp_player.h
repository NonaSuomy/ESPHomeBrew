// psram_video: the player (papp_player.c) and the input helper (papp_main.c).
#pragma once

#include "papp_port.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Input ─────────────────────────────────────────────────────────────────
// One poll per UI frame: gamepad edges, keyboard taps, touch taps and drags,
// and the ways to leave the app.

#define PAPP_MAX_KEYS 16

typedef struct {
    papp_gamepad_state_t pad, prev;
    int64_t repeat_at[PAPP_INPUT_MAX];  // next auto-repeat of a held button
    int keys[PAPP_MAX_KEYS];            // keyboard presses this poll (ASCII, Quake codes)
    int nkeys;
    // Touch (canvas coordinates).
    bool touching;
    int x, y;                // where the finger is
    int start_x, start_y;    // where it came down
    bool moved;              // dragged more than a few pixels
    bool tap;                // lifted without moving: a tap at tap_x, tap_y
    int tap_x, tap_y;
    int drag_dy;             // vertical movement since the last poll
    // Leaving: Menu held 3 s, Menu + X (the loader's close control), L3 held.
    int64_t menu_since, l3_since;
    bool quit_app;
} papp_input_t;

void papp_input_reset(papp_input_t *in);
void papp_input_poll(papp_input_t *in);
// True on the poll a button went down, and every `repeat_ms` while it stays
// down after the first 400 ms (0: no repeat).
bool papp_button(papp_input_t *in, int button, int repeat_ms);
bool papp_key(const papp_input_t *in, int key);

// ── Player ────────────────────────────────────────────────────────────────

enum papp_play_result {
    PLAY_ENDED,     // the end of the file
    PLAY_BACK,      // B, Esc or the Back button: leave the player
    PLAY_QUIT_APP,  // Menu held, the loader's close control: leave the app
    PLAY_FAILED,    // could not open or play it (the error was shown)
};

// Plays `location` (a /sd path, file:// or http(s):// URL) on the canvas
// fb (canvas_w x canvas_h) until the user leaves or it ends.
enum papp_play_result papp_play(const char *location, int canvas_w, int canvas_h, papp_input_t *in);

// Shows a message on the canvas (and the log) for up to `ms`, or until a
// button or tap. Returns false when the user asked to leave the app.
bool papp_message(const char *title, const char *text, int canvas_w, int canvas_h, int ms, papp_input_t *in);

#ifdef __cplusplus
}
#endif
