/*
 * gen68k_adapter.h — Adapter layer: Musashi API → Generator68K
 *
 * Provides all m68k_* functions that gwenesis expects (same API as Musashi)
 * but internally uses the Generator68K CPU core for execution.
 */
#ifndef GEN68K_ADAPTER_H
#define GEN68K_ADAPTER_H

#include <stdint.h>

/* Initialise the Generator68K adapter and memory map tables */
void gen68k_adapter_init(void);

#endif /* GEN68K_ADAPTER_H */
