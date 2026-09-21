#pragma once

#include <string.h>
#include "psram_app.h"

/* The legacy emulator cores use the ODROID input names.  A PAPP has the
 * same logical button layout, so keep this compatibility layer header-only
 * and source the state from the launcher's service table. */
typedef papp_gamepad_state_t odroid_gamepad_state;

enum {
    ODROID_INPUT_UP = PAPP_INPUT_UP,
    ODROID_INPUT_RIGHT = PAPP_INPUT_RIGHT,
    ODROID_INPUT_DOWN = PAPP_INPUT_DOWN,
    ODROID_INPUT_LEFT = PAPP_INPUT_LEFT,
    ODROID_INPUT_SELECT = PAPP_INPUT_SELECT,
    ODROID_INPUT_START = PAPP_INPUT_START,
    ODROID_INPUT_A = PAPP_INPUT_A,
    ODROID_INPUT_B = PAPP_INPUT_B,
    ODROID_INPUT_X = PAPP_INPUT_X,
    ODROID_INPUT_Y = PAPP_INPUT_Y,
    ODROID_INPUT_L = PAPP_INPUT_L,
    ODROID_INPUT_R = PAPP_INPUT_R,
    ODROID_INPUT_MENU = PAPP_INPUT_MENU,
    ODROID_INPUT_VOLUME = PAPP_INPUT_VOLUME,
    ODROID_INPUT_MAX = PAPP_INPUT_MAX
};

extern const app_services_t *_papp_svc;
extern volatile int odroid_paddle_adc_raw;
extern bool odroid_input_xy_menu_disable;
extern bool odroid_input_touch_buttons_disable;
#ifdef __cplusplus
extern "C" {
#endif
void odroid_paddle_adc_init(void);
#ifdef __cplusplus
}
#endif

static inline void odroid_input_gamepad_read(odroid_gamepad_state *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    if (_papp_svc && _papp_svc->input_gamepad_read)
        _papp_svc->input_gamepad_read(state);
}
