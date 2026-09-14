// OpenLara platform layer for the PAPP loader (replaces main/openlara_esp32p4.cpp
// of alexkid77/openlara_esp32p4, which drove the ESP-IDF drivers itself).
//
// THIS IS THE ONLY FILE THAT INCLUDES game.h: OpenLara is a header-only
// engine, so the whole engine (and its globals such as contentDir) is
// compiled here.
//
//   video : the software renderer draws 320x240 RGB565 into GAPI::swColor,
//           one of two frames papp_video.cpp shows through the loader (2x).
//   audio : Sound::fill() tops up papp_audio.cpp's ring once per frame.
//   input : the loader's gamepad as OpenLara joystick 0 (the PAPP build's
//           default bindings are set in patches/0001-papp-platform.patch),
//           plus USB keyboard keys the loader only reports as taps.
//   files : game data, settings and saves in /sd/roms/openlara/.
#include "papp_port.h"

#include "game.h"

#include <string.h>

// ── Engine OS hooks ─────────────────────────────────────────────────────────

int osGetTimeMS()
{
    return int(papp_time_us() / 1000);
}

bool osJoyReady(int index)
{
    return index == 0;
}

void osJoyVibrate(int, float, float)
{
}

// Everything that touches the engine runs on the game task (Sound::fill too),
// so its mutex has nothing to guard.
static int s_mutex_token;

void *osMutexInit()
{
    return &s_mutex_token;
}

void osMutexFree(void *)
{
}

void osMutexLock(void *)
{
}

void osMutexUnlock(void *)
{
}

// ── Input ───────────────────────────────────────────────────────────────────

// Loader key numbers for keys that are not printable ASCII (papp_loader.cpp).
enum
{
    LK_ESCAPE = 27,
    LK_ALT = 132,
    LK_CTRL = 133,
    LK_SHIFT = 134,
    LK_F1 = 135, // .. LK_F12 = 146
    LK_F12 = 146,
};

// Each gamepad input as the OpenLara joystick button of the same name.
static const struct
{
    int input;
    JoyKey joy;
} PAD_MAP[] = {
    {PAPP_INPUT_UP, jkUp},
    {PAPP_INPUT_DOWN, jkDown},
    {PAPP_INPUT_LEFT, jkLeft},
    {PAPP_INPUT_RIGHT, jkRight},
    {PAPP_INPUT_A, jkA},
    {PAPP_INPUT_B, jkB},
    {PAPP_INPUT_X, jkX},
    {PAPP_INPUT_Y, jkY},
    {PAPP_INPUT_L, jkLB},
    {PAPP_INPUT_R, jkRB},
    {PAPP_INPUT_START, jkStart},
    {PAPP_INPUT_SELECT, jkSelect},
};

static papp_gamepad_state_t s_pad;
static bool s_start_is_enter = false;
static long long s_menu_since = 0;

// The loader's USB keyboard path reports a key as a tap (down and up at
// once) and turns the held state of arrows, WASD, Space/Enter/Z, X, C, V/Tab,
// Q, E, Backspace and Escape into gamepad inputs. Only keys it has no gamepad
// input for are taken from the tap queue here, held down for TAP_US so the
// engine sees them: Escape (inventory), Ctrl (action), Alt (jump), Shift,
// the digits (5 quick save, 9 quick load) and F1-F11. F12 toggles the FPS
// counter, as in the original port. Letters are left out: the engine keeps
// debug keys on some of them (O, P, R, T).
static const long long TAP_US = 120000;
static long long s_key_release[ikMAX];

static InputKey key_for(int key)
{
    if (key >= '0' && key <= '9') {
        return InputKey(ik0 + (key - '0'));
    }
    if (key >= LK_F1 && key < LK_F12) {
        return InputKey(ikF1 + (key - LK_F1));
    }
    switch (key) {
    case LK_ESCAPE:
        return ikEscape;
    case LK_ALT:
        return ikAlt;
    case LK_CTRL:
        return ikCtrl;
    case LK_SHIFT:
        return ikShift;
    }
    return ikNone;
}

static void poll_keyboard(long long now)
{
    if (papp_svc->input_keyboard_read != nullptr) {
        papp_keyboard_event_t event;
        while (papp_svc->input_keyboard_read(&event)) {
            if (!event.down) {
                continue; // released after TAP_US instead
            }
            if (event.key == LK_F12) {
                UI::showFPS = !UI::showFPS;
                continue;
            }
            const InputKey key = key_for(event.key);
            if (key != ikNone) {
                Input::setDown(key, true);
                s_key_release[key] = now + TAP_US;
            }
        }
    }
    for (int key = 0; key < ikMAX; key++) {
        if (s_key_release[key] != 0 && now >= s_key_release[key]) {
            s_key_release[key] = 0;
            Input::setDown(InputKey(key), false);
        }
    }
}

// Returns false when the player asked to leave: the loader's close control
// (MENU + X, or its L3 service), or MENU held for 3 s.
static bool poll_input(long long now)
{
    poll_keyboard(now);

    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    if (papp_svc->input_gamepad_read != nullptr) {
        papp_svc->input_gamepad_read(&pad);
    }
    if (papp_svc->input_l3_read != nullptr && papp_svc->input_l3_read()) {
        return false;
    }
    if (pad.values[PAPP_INPUT_MENU] && pad.values[PAPP_INPUT_X]) {
        return false;
    }
    if (pad.values[PAPP_INPUT_MENU]) {
        if (s_menu_since == 0) {
            s_menu_since = now;
        } else if (now - s_menu_since >= 3000000) {
            return false;
        }
    } else {
        s_menu_since = 0;
    }

    // The loader reports the keyboard's Enter as START and A together. Treat
    // that as A alone (select/action, as Enter is on a PC) instead of also
    // opening or closing the inventory.
    const bool a_pressed = pad.values[PAPP_INPUT_A] && !s_pad.values[PAPP_INPUT_A];
    const bool start_pressed = pad.values[PAPP_INPUT_START] && !s_pad.values[PAPP_INPUT_START];
    if (start_pressed && a_pressed) {
        s_start_is_enter = true;
    } else if (!pad.values[PAPP_INPUT_START]) {
        s_start_is_enter = false;
    }
    if (s_start_is_enter) {
        pad.values[PAPP_INPUT_START] = 0;
    }

    for (const auto &m : PAD_MAP) {
        Input::setJoyDown(0, m.joy, pad.values[m.input] != 0);
    }
    // Temporary diagnostics: which buttons arrive, and what the inventory sees.
    static unsigned s_logged_mask = 0;
    unsigned mask = 0;
    for (int i = 0; i < PAPP_INPUT_MAX && i < 16; i++) {
        mask |= (pad.values[i] != 0 ? 1u : 0u) << i;
    }
    if (mask != s_logged_mask) {
        s_logged_mask = mask;
        papp_svc->log_printf("OL: pad %04x (up/right/down/left=bits 0-3, a=6, b=7, start=5) joyIndex=%d lastState=%d\n",
                             mask, int(Core::settings.controls[0].joyIndex), int(Input::lastState[0]));
    }
    s_pad = pad;
    return true;
}

// ── Health and oxygen bars ──────────────────────────────────────────────────
// With the software renderer the bar textures (CommonTex CTEX_HEALTH/OXYGEN)
// are never built, so Level::renderUI draws nothing for them; the original
// port overlays them itself. Drawn here straight into the 320x240 frame with
// the engine's own bar colours (level.h CommonTexData, 0xAABBGGRR) at
// Level::renderUI's positions (640x480 UI units, halved).

static const uint32 HEALTH_ROWS[5] = {0xFF2C5D71, 0xFF5E81AE, 0xFF2C5D71, 0xFF1B4557, 0xFF16304F};
static const uint32 OXYGEN_ROWS[5] = {0xFF647464, 0xFFA47848, 0xFF647464, 0xFF4C504C, 0xFF303030};

static inline uint16_t rgb565(uint32 abgr)
{
    const uint32 r = abgr & 0xFF, g = (abgr >> 8) & 0xFF, b = (abgr >> 16) & 0xFF;
    return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color, bool half)
{
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= PAPP_OL_HEIGHT) {
            continue;
        }
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= PAPP_OL_WIDTH) {
                continue;
            }
            uint16_t &p = fb[j * PAPP_OL_WIDTH + i];
            // 50% towards the colour: halve each channel of both and add.
            p = half ? uint16_t(((p >> 1) & 0x7BEF) + ((color >> 1) & 0x7BEF)) : color;
        }
    }
}

static void draw_bar(uint16_t *fb, int x, int y, float value, const uint32 *rows)
{
    const int w = 90, h = 5;
    fill_rect(fb, x - 1, y - 1, w + 2, 1, rgb565(0xFF4C504C), false); // frame, top
    fill_rect(fb, x - 1, y - 1, 1, h + 2, rgb565(0xFF4C504C), false); // left
    fill_rect(fb, x - 1, y + h, w + 2, 1, rgb565(0xFF748474), false); // bottom
    fill_rect(fb, x + w, y - 1, 1, h + 2, rgb565(0xFF748474), false); // right
    fill_rect(fb, x, y, w, h, 0x0000, true);                          // background
    const int filled = int(w * clamp(value, 0.0f, 1.0f) + 0.5f);
    for (int row = 0; row < h; row++) {
        fill_rect(fb, x, y + row, filled, 1, rgb565(rows[row]), false);
    }
}

static void draw_bars(uint16_t *fb)
{
    Level *level = Game::level;
    if (level == nullptr || inventory == nullptr || level->level.isTitle() || level->level.isCutsceneLevel()) {
        return;
    }
    if (inventory->titleTimer > 1.0f || inventory->active || inventory->video != nullptr) {
        return;
    }
    Lara *lara = level->players[0];
    if (lara == nullptr || (lara->camera != nullptr && lara->camera->spectator)) {
        return;
    }
    const bool blink = (osGetTimeMS() % 1000) < 500;
    float health = lara->health / LARA_MAX_HEALTH;
    float oxygen = lara->oxygen / LARA_MAX_OXYGEN;
    if (blink) {
        if (health <= 0.2f) {
            health = 0.0f;
        }
        if (oxygen <= 0.2f) {
            oxygen = 0.0f;
        }
    }
    const int x = (640 - 32 - 180) / 2;
    int y = 32 / 2;
    if (!lara->dozy && (lara->stand == Lara::STAND_ONWATER || lara->stand == Character::STAND_UNDERWATER)) {
        draw_bar(fb, x, y, oxygen, OXYGEN_ROWS);
        y += 16 / 2;
    }
    if ((lara->wpnReady() && !lara->emptyHands()) || lara->damageTime > 0.0f || health <= 0.2f) {
        draw_bar(fb, x, y, health, HEALTH_ROWS);
    }
}

// ── Game data ───────────────────────────────────────────────────────────────

// The TR1 files in any of the layouts the engine reads: the PC CD's DATA
// folder, the bare .PHD files, or OpenLara's own level/1/ tree.
static bool data_present()
{
    static const char *const probes[] = {"DATA/GYM.PHD",     "GYM.PHD",       "level/1/GYM.PHD",
                                         "DATA/TITLE.PHD",   "TITLE.PHD",     "level/1/TITLE.PHD",
                                         "DATA/LEVEL1.PHD",  "LEVEL1.PHD",    "level/1/LEVEL1.PHD"};
    for (const char *name : probes) {
        if (Stream::existsContent(name)) {
            return true;
        }
    }
    return false;
}

// Where the data may be: our folder first, then the places alexkid77's
// ESP32-P4 build looked (its /sdcard is the loader's /sd).
static const char *find_data_dir()
{
    static const char *const dirs[] = {PAPP_OL_DATA_DIR, "/sd/OpenLara/", "/sd/DATA/", "/sd/data/", "/sd/"};
    for (const char *dir : dirs) {
        strcpy(contentDir, dir);
        if (data_present()) {
            return dir;
        }
    }
    strcpy(contentDir, PAPP_OL_DATA_DIR);
    return nullptr;
}

// No TR1 data: a blue checkerboard until a button is pressed (or 30 s).
static void show_missing_data()
{
    papp_svc->log_printf("OL: no Tomb Raider 1 data (GYM.PHD, TITLE.PHD or LEVEL1.PHD, bare, in DATA/ or in "
                         "level/1/) in %s, /sd/OpenLara/, /sd/DATA/, /sd/data/ or /sd/. Copy the game's DATA "
                         "folder to %s, optionally FMV/ and music\n",
                         PAPP_OL_DATA_DIR, PAPP_OL_DATA_DIR);
    for (int frame = 0; frame < 2; frame++) {
        uint16_t *fb = papp_video_back();
        for (int y = 0; y < PAPP_OL_HEIGHT; y++) {
            for (int x = 0; x < PAPP_OL_WIDTH; x++) {
                fb[y * PAPP_OL_WIDTH + x] = (((x / 16) ^ (y / 16)) & 1) ? 0x001F : 0x0000;
            }
        }
        papp_video_present();
    }
    const long long start = papp_time_us();
    memset(&s_pad, 0, sizeof(s_pad));
    while (papp_time_us() - start < 30000000) {
        papp_gamepad_state_t pad;
        memset(&pad, 0, sizeof(pad));
        if (papp_svc->input_gamepad_read != nullptr) {
            papp_svc->input_gamepad_read(&pad);
        }
        bool any = papp_svc->input_l3_read != nullptr && papp_svc->input_l3_read();
        for (int i = 0; i < PAPP_INPUT_MAX; i++) {
            any = any || pad.values[i] != 0;
        }
        if (any) {
            break;
        }
        papp_svc->delay_ms(50);
    }
}

// ── Game loop ───────────────────────────────────────────────────────────────

static void pump_audio()
{
    static Sound::Frame mix[1024];
    int wanted = papp_audio_wanted();
    while (wanted > 0) {
        const int n = wanted < 1024 ? wanted : 1024;
        Sound::fill(mix, n);
        papp_audio_write(reinterpret_cast<const int16_t *>(mix), n);
        wanted -= n;
    }
}

extern "C" int papp_openlara_run(void)
{
    if (papp_video_init() != 0) {
        return -1;
    }

    const char *data_dir = find_data_dir();  // sets contentDir
    if (data_dir == nullptr) {
        show_missing_data();
        return -1;
    }
    strcpy(cacheDir, data_dir);  // "settings"
    strcpy(saveDir, data_dir);   // "savegame.dat"

    Core::width = PAPP_OL_WIDTH;
    Core::height = PAPP_OL_HEIGHT;
    GAPI::swColor = papp_video_back();
    GAPI::resize();

    papp_svc->log_printf("OL: starting the engine, data in %s\n", contentDir);
    Game::init((const char *)NULL);
    if (Core::isQuit) {
        papp_svc->log_printf("OL: the engine could not load its first level\n");
        return -1;
    }
    papp_audio_init(); // after Game::init: Sound is set up and the settings are read

    long long last_log = 0;
    int frames = 0;
    while (!Core::isQuit) {
        const long long now = papp_time_us();
        if (!poll_input(now)) {
            papp_svc->log_printf("OL: close requested\n");
            break;
        }
        GAPI::swColor = papp_video_back();
        if (Game::update()) {
            Game::render();
            draw_bars(papp_video_back());
            papp_video_present();
            frames++;
        }
        pump_audio();
        if (now - last_log >= 60000000) {
            if (last_log != 0) {
                papp_svc->log_printf("OL: %d fps\n", (int)(frames * 1000000LL / (now - last_log)));
            }
            last_log = now;
            frames = 0;
        }
        papp_svc->delay_ms(1); // a yield
        papp_yield_maybe();
    }
    // Game::deinit() is skipped: papp_main.cpp returns the whole heap and
    // every open file to the loader.
    return 0;
}
