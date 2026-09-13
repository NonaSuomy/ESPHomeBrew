// Keyboard, mouse, gamepad and touch for the Red Alert PAPP (replaces
// common/wwkeyboard_sdl2.cpp). Everything ends up as the game's own key and
// mouse messages (Win32 VK codes, as in the non-SDL build).
//
//   USB mouse    -> cursor motion, left/right/middle buttons
//   USB keyboard -> keys (the loader sends taps with shift already applied)
//   touch        -> cursor jumps to the finger, left button while touching
//   gamepad      -> d-pad = arrow keys (scroll), A = left click, B = right
//                   click, START = Enter, SELECT = Escape
//   on-screen X  -> quit (the loader reports it as MENU + X together)
#include "papp_port.h"

#include "common/wwkeyboard.h"

#include <string.h>

extern float papp_mouse_x;
extern float papp_mouse_y;
void Move_Video_Mouse(float xrel, float yrel);
void papp_video_mode_size(int* w, int* h);
void Process_Network(); // common/wsproto.cpp

// Loader key numbers for keys that are not printable ASCII (papp_loader.cpp).
enum
{
    LK_TAB = 9,
    LK_ENTER = 13,
    LK_ESCAPE = 27,
    LK_BACKSPACE = 127,
    LK_ALT = 132,
    LK_CTRL = 133,
    LK_SHIFT = 134,
    LK_F1 = 135,  // .. LK_F12 = 146
    LK_INSERT = 147,
    LK_DELETE = 148,
    LK_PAGE_DOWN = 149,
    LK_PAGE_UP = 150,
    LK_HOME = 151,
    LK_END = 152,
};

static unsigned short vk_for_key(int key, char* ascii)
{
    *ascii = 0;
    if (key >= 'a' && key <= 'z') {
        *ascii = (char)key;
        return VK_A + (key - 'a');
    }
    if (key >= 'A' && key <= 'Z') {
        *ascii = (char)key;
        return VK_A + (key - 'A');
    }
    if (key >= '0' && key <= '9') {
        *ascii = (char)key;
        return VK_0 + (key - '0');
    }
    if (key >= LK_F1 && key <= LK_F1 + 11) {
        return VK_F1 + (key - LK_F1);
    }
    switch (key) {
    case ' ':
        *ascii = ' ';
        return VK_SPACE;
    case LK_TAB:
        return VK_TAB;
    case LK_ENTER:
        *ascii = '\r';
        return VK_RETURN;
    case LK_ESCAPE:
        return VK_ESCAPE;
    case LK_BACKSPACE:
        *ascii = '\b';
        return VK_BACK;
    case LK_ALT:
        return VK_MENU;
    case LK_CTRL:
        return VK_CONTROL;
    case LK_SHIFT:
        return VK_SHIFT;
    case LK_INSERT:
        return VK_INSERT;
    case LK_DELETE:
        return VK_DELETE;
    case LK_PAGE_DOWN:
        return VK_NEXT;
    case LK_PAGE_UP:
        return VK_PRIOR;
    case LK_HOME:
        return VK_HOME;
    case LK_END:
        return VK_END;
    }
    // Punctuation: Win32 OEM codes; shifted variants arrive as their own ASCII.
    static const struct
    {
        char plain, shifted;
        unsigned short vk;
    } punct[] = {
        {';', ':', VK_NONE_BA},
        {'=', '+', VK_NONE_BB},
        {',', '<', VK_NONE_BC},
        {'-', '_', VK_NONE_BD},
        {'.', '>', VK_NONE_BE},
        {'/', '?', VK_NONE_BF},
        {'`', '~', VK_NONE_C0},
        {'[', '{', VK_NONE_DB},
        {'\\', '|', VK_NONE_DC},
        {']', '}', VK_NONE_DD},
        {'\'', '"', VK_NONE_DE},
    };
    for (const auto& p : punct) {
        if (key == p.plain || key == p.shifted) {
            *ascii = (char)key;
            return p.vk;
        }
    }
    return VK_NONE;
}

class WWKeyboardClassPAPP : public WWKeyboardClass
{
public:
    WWKeyboardClassPAPP()
    {
        memset(Ascii, 0, sizeof(Ascii));
        memset(&Pad, 0, sizeof(Pad));
    }

    KeyASCIIType To_ASCII(unsigned short key) override
    {
        if (key & WWKEY_RLS_BIT) {
            return KA_NONE;
        }
        return (KeyASCIIType)(unsigned char)Ascii[key & 0xFF];
    }

    bool Is_Gamepad_Active() override
    {
        return false;
    }

    void Poll()
    {
        Poll_Keyboard();
        Poll_Mouse();
        Poll_Touch();
        Poll_Gamepad();
    }

private:
    void Fill_Buffer_From_System() override
    {
#ifdef NETWORKING
        // The SDL keyboards pump the UDP socket here; without it network
        // games queue packets that are never sent or read.
        Process_Network();
#endif
        papp_input_poll();
    }

    void Key(unsigned short vk, bool down)
    {
        if (vk != VK_NONE) {
            Put_Key_Message(vk, !down);
        }
    }

    void Button(unsigned short vk, bool down)
    {
        Put_Mouse_Message(vk, (int)papp_mouse_x, (int)papp_mouse_y, !down);
    }

    void Poll_Keyboard()
    {
        if (papp_svc->input_keyboard_read == nullptr) {
            return;
        }
        papp_keyboard_event_t event;
        while (papp_svc->input_keyboard_read(&event)) {
            char ascii;
            const unsigned short vk = vk_for_key(event.key, &ascii);
            if (vk == VK_NONE) {
                continue;
            }
            if (ascii != 0) {
                Ascii[vk & 0xFF] = ascii;  // what To_ASCII reports for this key
            }
            Key(vk, event.down != 0);
        }
    }

    void Poll_Mouse()
    {
        if (papp_svc->input_mouse_read == nullptr) {
            return;
        }
        int dx = 0, dy = 0, buttons = 0;
        if (!papp_svc->input_mouse_read(&dx, &dy, &buttons)) {
            return;
        }
        if (dx != 0 || dy != 0) {
            Move_Video_Mouse((float)dx, (float)dy);
        }
        const int changed = buttons ^ MouseButtons;
        if (changed & 1) {
            Button(VK_LBUTTON, buttons & 1);
        }
        if (changed & 2) {
            Button(VK_RBUTTON, buttons & 2);
        }
        if (changed & 4) {
            Button(VK_MBUTTON, buttons & 4);
        }
        MouseButtons = buttons;
    }

    // The touch panel reports the 800x480 canvas; the game page sits centred in it.
    void Poll_Touch()
    {
        if (papp_svc->touch_read == nullptr) {
            return;
        }
        int x = 0, y = 0;
        const bool touching = papp_svc->touch_read(&x, &y) != 0;
        const long long now = papp_time_us();
        if (touching) {
            int w, h;
            papp_video_mode_size(&w, &h);
            const float gx = (float)(x - (800 - w) / 2);
            const float gy = (float)(y - (480 - h) / 2);
            Move_Video_Mouse(gx - papp_mouse_x, gy - papp_mouse_y);
            LastTouch = now;
        }
        // The panel is sampled every 50 ms and a sample can come back empty
        // mid-drag, which let go of a selection box. Only count a release
        // after no touch for TOUCH_RELEASE_US.
        const bool held = touching || (Touching && now - LastTouch < TOUCH_RELEASE_US);
        if (held != Touching) {
            Button(VK_LBUTTON, held);
            Touching = held;
        }
    }

    void Poll_Gamepad()
    {
        if (papp_svc->input_gamepad_read == nullptr) {
            return;
        }
        papp_gamepad_state_t now;
        papp_svc->input_gamepad_read(&now);
        if (now.values[PAPP_INPUT_MENU] && now.values[PAPP_INPUT_X]) {
            papp_svc->log_printf("RA: close requested\n");
            papp_quit(0);
        }
        struct
        {
            int input;
            unsigned short vk;
            bool mouse;
        } const map[] = {
            {PAPP_INPUT_UP, VK_UP, false},
            {PAPP_INPUT_DOWN, VK_DOWN, false},
            {PAPP_INPUT_LEFT, VK_LEFT, false},
            {PAPP_INPUT_RIGHT, VK_RIGHT, false},
            {PAPP_INPUT_START, VK_RETURN, false},
            {PAPP_INPUT_SELECT, VK_ESCAPE, false},
            {PAPP_INPUT_A, VK_LBUTTON, true},
            {PAPP_INPUT_B, VK_RBUTTON, true},
        };
        for (const auto& m : map) {
            const bool down = now.values[m.input] != 0;
            if (down != (Pad.values[m.input] != 0)) {
                if (m.mouse) {
                    Button(m.vk, down);
                } else {
                    Key(m.vk, down);
                }
            }
        }
        Pad = now;
    }

    char Ascii[256];
    papp_gamepad_state_t Pad;
    int MouseButtons = 0;
    bool Touching = false;
    long long LastTouch = 0;
    static const long long TOUCH_RELEASE_US = 120000;
};

static WWKeyboardClassPAPP* s_keyboard = nullptr;

WWKeyboardClass* CreateWWKeyboardClass(void)
{
    s_keyboard = new WWKeyboardClassPAPP;
    return s_keyboard;
}

extern "C" int papp_close_requested(void)
{
    if (papp_svc->input_gamepad_read == nullptr) {
        return 0;
    }
    papp_gamepad_state_t now;
    papp_svc->input_gamepad_read(&now);
    return now.values[PAPP_INPUT_MENU] && now.values[PAPP_INPUT_X];
}

// The game polls the keyboard constantly, including inside its busy-wait loops.
// Read the loader's inputs at most every few milliseconds and yield a tick each
// time, so the loop cannot starve the loader or trip the task watchdog.
extern "C" void papp_input_poll(void)
{
    static long long last = 0;
    const long long now = papp_time_us();
    if (now - last < 4000) {
        return;
    }
    last = now;
    papp_svc->delay_ms(1);
    static long long last_check = 0;
    if (now - last_check > 2000000) {
        last_check = now;
        papp_heap_check();
    }
    if (s_keyboard != nullptr) {
        s_keyboard->Poll();
    }
}
