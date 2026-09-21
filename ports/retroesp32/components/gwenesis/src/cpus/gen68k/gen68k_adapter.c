/*
 * gen68k_adapter.c — Drop-in replacement for Musashi M68K core
 *
 * Implements the exact same m68k_* API that gwenesis calls,
 * but uses Generator68K (from the NeoGeo port) internally.
 *
 * Key differences from Musashi:
 *   - Generator68K uses IPC (Instruction Pre-Compilation) + block execution
 *   - Memory access via function pointer tables (mem68k_fetch_word[4096])
 *   - Register state in global `regs` struct (t_regs)
 *   - Cycle count in `cpu68k_clocks` global
 *
 * Both Genesis and NeoGeo are big-endian 68000 systems, so the
 * Generator68K core works directly — no endianness changes needed
 * in the CPU core itself.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

/* Generator68K headers */
#include "generator.h"
#include "cpu68k.h"
#include "reg68k.h"
#include "mem68k.h"

/* Musashi-compatible header — we provide the m68ki_cpu_core struct */
#include "m68k.h"

#include "gen68k_adapter.h"
#include "perf_counters.h"

static const char *TAG = "GEN68K";

/* Generator68K references these NeoGeo globals — provide stubs for Genesis */
uint32 bankaddress = 0;
perf_counters_t g_perf;

/* ═══════════════════════════════════════════════════════════════════
 *  Generator68K memory function pointer tables
 *
 *  These are normally defined in gngeo/generator68k_interf.c (NeoGeo)
 *  but we provide Genesis-specific definitions here.
 *  Use Uint8/Uint16/Uint32 (SDL types) to match mem68k.h declarations.
 * ═══════════════════════════════════════════════════════════════════ */
Uint8 *(*mem68k_memptr[0x1000])(Uint32 addr);
Uint8  (*mem68k_fetch_byte[0x1000])(Uint32 addr);
Uint16 (*mem68k_fetch_word[0x1000])(Uint32 addr);
Uint32 (*mem68k_fetch_long[0x1000])(Uint32 addr);
void   (*mem68k_store_byte[0x1000])(Uint32 addr, Uint8 data);
void   (*mem68k_store_word[0x1000])(Uint32 addr, Uint16 data);
void   (*mem68k_store_long[0x1000])(Uint32 addr, Uint32 data);

/* ═══════════════════════════════════════════════════════════════════
 *  Musashi-compatible global state
 *
 *  gwenesis_bus.c and genesis_run.c access `m68k->cycles` directly,
 *  so we maintain a thin m68ki_cpu_core shell that mirrors the
 *  Generator68K state.
 * ═══════════════════════════════════════════════════════════════════ */
m68ki_cpu_core *m68k = NULL;

/* IRQ acknowledge callback (set by gwenesis_bus via m68k_set_int_ack_callback) */
static int (*s_int_ack_callback)(int int_level) = NULL;

/* ─── Memory bus wrappers ─────────────────────────────────────────
 *  Generator68K uses mem68k_fetch_word[4096] etc.
 *  Genesis has 24-bit address space, mapped as:
 *    0x000000–0x3FFFFF : ROM  (up to 4 MB)  → slots 0x000–0x3FF
 *    0x400000–0x9FFFFF : varies (SRAM, etc.) → slots 0x400–0x9FF
 *    0xA00000–0xA0FFFF : Z80 space           → slots 0xA00–0xA0F
 *    0xA10000–0xA1001F : I/O                 → slot  0xA10
 *    0xC00000–0xC0001F : VDP                 → slot  0xC00
 *    0xE00000–0xEFFFFF : RAM mirrors         → slots 0xE00–0xEFF
 *    0xFF0000–0xFFFFFF : 68K RAM (64 KB)     → slots 0xFF0–0xFFF
 *
 *  FAST PATH: ROM and RAM slots get direct-access functions that
 *  bypass the full gwenesis_bus switch statement.
 *  I/O slots use the generic bus wrapper.
 * ──────────────────────────────────────────────────────────────── */

/* These are the Musashi-named functions defined in gwenesis_bus.c */
extern unsigned int m68k_read_memory_8(unsigned int address);
extern unsigned int m68k_read_memory_16(unsigned int address);
extern unsigned int m68k_read_memory_32(unsigned int address);
extern void m68k_write_memory_8(unsigned int address, unsigned int value);
extern void m68k_write_memory_16(unsigned int address, unsigned int value);
extern void m68k_write_memory_32(unsigned int address, unsigned int value);

extern unsigned char *ROM_DATA;
extern unsigned char *M68K_RAM;
extern unsigned int gwenesis_rom_size;

/* ── FAST ROM readers (slots 0x000–0x3FF) ────────────────────── */
static Uint8  rom_fetch_byte(Uint32 addr) {
    addr &= 0xFFFFFF;
    return (addr < gwenesis_rom_size) ? ROM_DATA[addr] : 0xFF;
}
static Uint16 rom_fetch_word(Uint32 addr) {
    addr &= 0xFFFFFF;
    return (addr + 1 < gwenesis_rom_size)
        ? __builtin_bswap16(*(Uint16 *)(ROM_DATA + addr))
        : 0xFFFF;
}
static Uint32 rom_fetch_long(Uint32 addr) {
    addr &= 0xFFFFFF;
    return (addr + 3 < gwenesis_rom_size)
        ? __builtin_bswap32(*(Uint32 *)(ROM_DATA + addr))
        : 0xFFFFFFFF;
}
static void rom_store_byte(Uint32 addr, Uint8 data)  { /* ROM is read-only */ (void)addr; (void)data; }
static void rom_store_word(Uint32 addr, Uint16 data) { /* ROM is read-only */ (void)addr; (void)data; }
static void rom_store_long(Uint32 addr, Uint32 data) { /* ROM is read-only */ (void)addr; (void)data; }

/* ── FAST RAM readers (slots 0xE00–0xFFF) ────────────────────── */
static Uint8  ram_fetch_byte(Uint32 addr) {
    return M68K_RAM[addr & 0xFFFF];
}
static Uint16 ram_fetch_word(Uint32 addr) {
    return __builtin_bswap16(*(Uint16 *)(M68K_RAM + (addr & 0xFFFF)));
}
static Uint32 ram_fetch_long(Uint32 addr) {
    return __builtin_bswap32(*(Uint32 *)(M68K_RAM + (addr & 0xFFFF)));
}
static void ram_store_byte(Uint32 addr, Uint8 data) {
    M68K_RAM[addr & 0xFFFF] = data;
}
static void ram_store_word(Uint32 addr, Uint16 data) {
    *(Uint16 *)(M68K_RAM + (addr & 0xFFFF)) = __builtin_bswap16(data);
}
static void ram_store_long(Uint32 addr, Uint32 data) {
    *(Uint32 *)(M68K_RAM + (addr & 0xFFFF)) = __builtin_bswap32(data);
}

/* ── SLOW generic bus wrappers (I/O, VDP, Z80, etc.) ─────────── */
static Uint8  gen_fetch_byte(Uint32 addr) { return (Uint8)m68k_read_memory_8(addr); }
static Uint16 gen_fetch_word(Uint32 addr) { return (Uint16)m68k_read_memory_16(addr); }
static Uint32 gen_fetch_long(Uint32 addr) { return (Uint32)m68k_read_memory_32(addr); }
static void   gen_store_byte(Uint32 addr, Uint8 data)  { m68k_write_memory_8(addr, data); }
static void   gen_store_word(Uint32 addr, Uint16 data)  { m68k_write_memory_16(addr, data); }
static void   gen_store_long(Uint32 addr, Uint32 data)  { m68k_write_memory_32(addr, data); }

/* memptr — returns pointer to memory for direct access (used for IPC building).
 * For ROM and RAM we can return direct pointers; for I/O we return NULL
 * and Generator68K will fall back to fetch functions. */

static Uint8 *rom_memptr(Uint32 addr)
{
    addr &= 0xFFFFFF;
    return (addr < gwenesis_rom_size) ? ROM_DATA + addr : NULL;
}

static Uint8 *ram_memptr(Uint32 addr)
{
    return M68K_RAM + (addr & 0xFFFF);
}

static Uint8 *null_memptr(Uint32 addr)
{
    (void)addr;
    return NULL;
}

static void gen68k_init_memtable(void)
{
    /* Default: all slots use generic bus wrappers */
    for (int i = 0; i < 0x1000; i++) {
        mem68k_memptr[i]      = null_memptr;
        mem68k_fetch_byte[i]  = gen_fetch_byte;
        mem68k_fetch_word[i]  = gen_fetch_word;
        mem68k_fetch_long[i]  = gen_fetch_long;
        mem68k_store_byte[i]  = gen_store_byte;
        mem68k_store_word[i]  = gen_store_word;
        mem68k_store_long[i]  = gen_store_long;
    }

    /* ROM: slots 0x000–0x3FF (0x000000–0x3FFFFF) */
    for (int i = 0x000; i <= 0x3FF; i++) {
        mem68k_memptr[i]      = rom_memptr;
        mem68k_fetch_byte[i]  = rom_fetch_byte;
        mem68k_fetch_word[i]  = rom_fetch_word;
        mem68k_fetch_long[i]  = rom_fetch_long;
        mem68k_store_byte[i]  = rom_store_byte;
        mem68k_store_word[i]  = rom_store_word;
        mem68k_store_long[i]  = rom_store_long;
    }

    /* RAM: slots 0xE00–0xFFF (0xE00000–0xFFFFFF, 64K mirrored) */
    for (int i = 0xE00; i <= 0xFFF; i++) {
        mem68k_memptr[i]      = ram_memptr;
        mem68k_fetch_byte[i]  = ram_fetch_byte;
        mem68k_fetch_word[i]  = ram_fetch_word;
        mem68k_fetch_long[i]  = ram_fetch_long;
        mem68k_store_byte[i]  = ram_store_byte;
        mem68k_store_word[i]  = ram_store_word;
        mem68k_store_long[i]  = ram_store_long;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Sync m68ki_cpu_core ↔ Generator68K regs
 * ═══════════════════════════════════════════════════════════════════ */

/* Sync Generator68K state → m68ki_cpu_core (after execution) */
static void sync_regs_to_shell(void)
{
    m68k->cycles = cpu68k_clocks;
    m68k->pc     = regs.pc;
    for (int i = 0; i < 16; i++)
        m68k->dar[i] = regs.regs[i];
    m68k->ir = 0; /* not directly available in generator68k */
}

/* Sync m68ki_cpu_core → Generator68K (before execution, after savestate load) */
static void sync_shell_to_regs(void)
{
    cpu68k_clocks = m68k->cycles;
    regs.pc       = m68k->pc;
    for (int i = 0; i < 16; i++)
        regs.regs[i] = m68k->dar[i];
}

/* ═══════════════════════════════════════════════════════════════════
 *  Musashi-compatible API implementation
 * ═══════════════════════════════════════════════════════════════════ */

void m68k_init(void)
{
    /* Allocate the shell struct */
    if (!m68k) {
        m68k = (m68ki_cpu_core *)heap_caps_calloc(1, sizeof(m68ki_cpu_core),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    /* Initialise Generator68K opcode tables */
    cpu68k_init();

    /* Wire Genesis memory bus into Generator68K */
    gen68k_init_memtable();

    ESP_LOGI(TAG, "Generator68K initialised (%d instructions, %d routines)",
             cpu68k_totalinstr, cpu68k_totalfuncs);
}

void m68k_pulse_reset(void)
{
    /* Reset Generator68K state */
    memset(&regs, 0, sizeof(regs));
    cpu68k_clocks = 0;
    cpu68k_frozen = 0;

    /* Load SP and PC from vectors (big-endian ROM at address 0) */
    regs.regs[15] = fetchlong(0);   /* A7 = SP from vector 0 */
    regs.pc       = fetchlong(4);   /* PC from vector 1 */
    regs.sr.sr_int = 0x2700;        /* Supervisor mode, interrupts masked */

    /* Verify ROM vector reads are correct */
    ESP_LOGI(TAG, "ROM[0..7]: %02X %02X %02X %02X %02X %02X %02X %02X",
             ROM_DATA[0], ROM_DATA[1], ROM_DATA[2], ROM_DATA[3],
             ROM_DATA[4], ROM_DATA[5], ROM_DATA[6], ROM_DATA[7]);

    /* Clear IPC cache */
    cpu68k_clearcache();

    /* Sync to shell */
    m68k->cycles = 0;
    m68k->cycle_end = 0;
    sync_regs_to_shell();

    ESP_LOGI(TAG, "Reset: SP=%08X PC=%08X", (unsigned)regs.regs[15], (unsigned)regs.pc);

    /* Dump instructions around common stuck point */
    ESP_LOGI(TAG, "ROM@0x250: %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X",
             ROM_DATA[0x250], ROM_DATA[0x251], ROM_DATA[0x252], ROM_DATA[0x253],
             ROM_DATA[0x254], ROM_DATA[0x255], ROM_DATA[0x256], ROM_DATA[0x257],
             ROM_DATA[0x258], ROM_DATA[0x259], ROM_DATA[0x25A], ROM_DATA[0x25B],
             ROM_DATA[0x25C], ROM_DATA[0x25D], ROM_DATA[0x25E], ROM_DATA[0x25F]);
}

void m68k_run(unsigned int target_cycles)
{
    /* Genesis code manipulates m68k->cycles directly (e.g. subtracting
     * system_clock at end of each frame).  Propagate any external
     * change back to the Generator68K master counter. */
    cpu68k_clocks = m68k->cycles;

    if (cpu68k_clocks >= target_cycles)
        return;

    unsigned int clocks_to_run = target_cycles - cpu68k_clocks;

    /* Set cycle target for shell compatibility */
    m68k->cycle_end = target_cycles;

    /* Execute via Generator68K block executor */
    reg68k_external_execute(clocks_to_run);

    /* Sync back */
    sync_regs_to_shell();

    /* One-time diagnostic: log PC after first scanline */
    static int diag_count = 0;
    static unsigned int last_pc = 0;
    if (diag_count < 30 && (regs.pc != last_pc || diag_count < 5)) {
        ESP_LOGI(TAG, "run: target=%u clocks=%u PC=%06X SR=%04X",
                 target_cycles, cpu68k_clocks, regs.pc & 0xFFFFFF, regs.sr.sr_int);
        last_pc = regs.pc;
        diag_count++;
    }
}

/* ─── Interrupt management ─────────────────────────────────────── */

void m68k_set_irq(unsigned int int_level)
{
    /* Musashi: CPU_INT_LEVEL = int_level << 8
     * Generator68K: regs.pending = interrupt level (1-7) */
    static int irq_diag = 0;
    if (irq_diag < 3) {
        ESP_LOGI(TAG, "set_irq(%u) SR=%04X pending_was=%u PC=%06X",
                 int_level, regs.sr.sr_int, regs.pending, regs.pc & 0xFFFFFF);
        irq_diag++;
    }
    regs.pending = int_level;
}

void m68k_set_irq_delay(unsigned int int_level)
{
    /* Execute one instruction then set IRQ */
    reg68k_external_step();
    sync_regs_to_shell();
    regs.pending = int_level;
}

void m68k_update_irq(unsigned int mask)
{
    /* Musashi: CPU_INT_LEVEL |= (mask << 8)
     * For Genesis H-interrupt: if current pending < mask, raise it */
    if (mask > regs.pending)
        regs.pending = mask;
}

/* ─── Cycle counting ───────────────────────────────────────────── */

int m68k_cycles(void)
{
    return 0; /* Not meaningful for Generator68K */
}

int m68k_cycles_run(void)
{
    return m68k->cycle_end - cpu68k_clocks;
}

int m68k_cycles_master(void)
{
    return cpu68k_clocks;
}

/* ─── Register access ──────────────────────────────────────────── */

unsigned int m68k_get_reg(m68k_register_t reg)
{
    switch (reg) {
    case M68K_REG_D0 ... M68K_REG_D7:
        return regs.regs[reg - M68K_REG_D0];
    case M68K_REG_A0 ... M68K_REG_A7:
        return regs.regs[8 + (reg - M68K_REG_A0)];
    case M68K_REG_PC:
        return regs.pc;
    case M68K_REG_SR:
        return regs.sr.sr_int;
    case M68K_REG_SP:
        return regs.regs[15]; /* A7 */
    case M68K_REG_USP:
        return regs.sp;
    case M68K_REG_ISP:
        return regs.regs[15];
    case M68K_REG_IR:
        return 0;
    default:
        return 0;
    }
}

void m68k_set_reg(m68k_register_t reg, unsigned int value)
{
    switch (reg) {
    case M68K_REG_D0 ... M68K_REG_D7:
        regs.regs[reg - M68K_REG_D0] = value;
        break;
    case M68K_REG_A0 ... M68K_REG_A7:
        regs.regs[8 + (reg - M68K_REG_A0)] = value;
        break;
    case M68K_REG_PC:
        regs.pc = value;
        break;
    case M68K_REG_SR:
        regs.sr.sr_int = (uint16)value;
        break;
    case M68K_REG_SP:
        regs.regs[15] = value;
        break;
    case M68K_REG_USP:
        regs.sp = value;
        break;
    default:
        break;
    }
}

/* ─── Callback registration ───────────────────────────────────── */

void m68k_set_int_ack_callback(int (*callback)(int int_level))
{
    s_int_ack_callback = callback;
    if (m68k) m68k->int_ack_callback = callback;
}

void m68k_set_reset_instr_callback(void (*callback)(void))
{
    if (m68k) m68k->reset_instr_callback = callback;
}

void m68k_set_tas_instr_callback(int (*callback)(void))
{
    if (m68k) m68k->tas_instr_callback = callback;
}

void m68k_set_fc_callback(void (*callback)(unsigned int new_fc))
{
    if (m68k) m68k->set_fc_callback = callback;
}

/* ─── Halt control ─────────────────────────────────────────────── */

void m68k_pulse_halt(void)
{
    cpu68k_frozen = 1;
}

void m68k_clear_halt(void)
{
    cpu68k_frozen = 0;
}

/* ─── Savestate (gwenesis_m68k_save_state / load_state) ─────── */

#include "gwenesis_savestate.h"

void gwenesis_m68k_save_state(void)
{
    sync_regs_to_shell();

    SaveState *state = (SaveState *)1;
    saveGwenesisStateSetBuffer(state, "68K_REGS", regs.regs, sizeof(regs.regs));
    saveGwenesisStateSet(state, "68K_PC", regs.pc);
    saveGwenesisStateSet(state, "68K_SR", regs.sr.sr_int);
    saveGwenesisStateSet(state, "68K_SP", regs.sp);
    saveGwenesisStateSet(state, "68K_CYCLES", cpu68k_clocks);
    saveGwenesisStateSet(state, "68K_PENDING", regs.pending);
    saveGwenesisStateSet(state, "68K_STOP", regs.stop);
}

void gwenesis_m68k_load_state(void)
{
    SaveState *state = (SaveState *)1;
    saveGwenesisStateGetBuffer(state, "68K_REGS", regs.regs, sizeof(regs.regs));
    regs.pc       = saveGwenesisStateGet(state, "68K_PC");
    regs.sr.sr_int = (uint16)saveGwenesisStateGet(state, "68K_SR");
    regs.sp       = saveGwenesisStateGet(state, "68K_SP");
    cpu68k_clocks = saveGwenesisStateGet(state, "68K_CYCLES");
    regs.pending  = (uint16)saveGwenesisStateGet(state, "68K_PENDING");
    regs.stop     = (uint16)saveGwenesisStateGet(state, "68K_STOP");

    /* Flush IPC cache — code might have changed */
    cpu68k_clearcache();

    sync_regs_to_shell();
}

/* ─── Adapter init (called from genesis_run) ───────────────────── */

void gen68k_adapter_init(void)
{
    /* Already handled by m68k_init() + m68k_pulse_reset() */
}

/* Disassembler stubs already provided by gwenesis_bus.c — not needed here */
