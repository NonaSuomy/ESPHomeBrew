// MicroPython HAL for Tulip on the PAPP loader: console, clocks, delays.
//
// Console output goes where Tulip Desktop sends it (tulip/shared/desktop/
// unix_mphal.c): Tulip's text frame buffer on screen, plus the loader's log
// (one line at a time) so a session can be followed over the serial port or
// the ESP bridge. Console input is the ring Tulip's keyboard code fills
// (tx_char in tulip_helpers.c) from the display task (papp_display.c).
#include "papp_port.h"

#include <string.h>

#include "py/mphal.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "display.h"

static uint8_t stdin_ringbuf_array[512];
ringbuf_t stdin_ringbuf = {stdin_ringbuf_array, sizeof(stdin_ringbuf_array), 0, 0};

int mp_hal_stdin_rx_chr(void)
{
    for (;;) {
        int c = ringbuf_get(&stdin_ringbuf);
        if (c != -1) {
            return c;
        }
        MICROPY_EVENT_POLL_HOOK
    }
}

uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags)
{
    uintptr_t ret = 0;
    if ((poll_flags & MP_STREAM_POLL_RD) && ringbuf_peek(&stdin_ringbuf) != -1) {
        ret |= MP_STREAM_POLL_RD;
    }
    if (poll_flags & MP_STREAM_POLL_WR) {
        ret |= MP_STREAM_POLL_WR;
    }
    return ret;
}

// The log gets whole lines; the screen gets everything as it comes.
static void log_console(const char *str, size_t len)
{
    static char line[200];
    static size_t used = 0;
    for (size_t i = 0; i < len; i++) {
        const char c = str[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n' || used == sizeof(line) - 1) {
            line[used] = '\0';
            papp_svc->log_printf("> %s\n", line);
            used = 0;
            if (c == '\n') {
                continue;
            }
        }
        line[used++] = c;
    }
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
    if (len) {
        display_tfb_str((unsigned char *)str, (uint16_t)len, 0, tfb_fg_pal_color, tfb_bg_pal_color);
        log_console(str, len);
    }
    return len;
}

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len)
{
    mp_hal_stdout_tx_strn(str, len);
}

void mp_hal_stdout_tx_str(const char *str)
{
    mp_hal_stdout_tx_strn(str, strlen(str));
}

mp_uint_t mp_hal_ticks_ms(void)
{
    return (mp_uint_t)(papp_time_us() / 1000);
}

mp_uint_t mp_hal_ticks_us(void)
{
    return (mp_uint_t)papp_time_us();
}

uint64_t mp_hal_time_ns(void)
{
    return (uint64_t)papp_time_us() * 1000ULL;
}

void mp_hal_delay_ms(mp_uint_t ms)
{
    const int64_t end = papp_time_us() + (int64_t)ms * 1000;
    for (;;) {
        mp_handle_pending(true);
        const int64_t left = end - papp_time_us();
        if (left <= 0) {
            break;
        }
        if (left >= 10000) {
            papp_mp_wait();  // sleeps one 10 ms tick
        } else {
            papp_mp_poll();
            papp_svc->delay_ms(1);  // below a tick this is a yield
        }
    }
}

void papp_mp_delay_us(mp_uint_t us)
{
    if (us >= 10000) {
        mp_hal_delay_ms(us / 1000);
        return;
    }
    const int64_t end = papp_time_us() + us;
    while (papp_time_us() < end) {
    }
}

uint32_t papp_random_seed(void)
{
    return (uint32_t)papp_time_us() * 2654435761u;
}
