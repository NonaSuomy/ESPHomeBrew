// MicroPython HAL for Tulip on the PAPP loader (papp_mphal.c).
#pragma once

#include <stdint.h>
#include "shared/runtime/interrupt_char.h"

#define mp_hal_ticks_cpu() (0)

static inline void mp_hal_delay_us(mp_uint_t us)
{
    extern void papp_mp_delay_us(mp_uint_t us);
    papp_mp_delay_us(us);
}

// Tulip's keyboard input (tulip_helpers.c tx_char) fills this ring; the REPL
// reads it through mp_hal_stdin_rx_chr().
#include "py/ringbuf.h"
extern ringbuf_t stdin_ringbuf;
