#define PAPP_APP_SIDE 1

#include "psram_app.h"
#include "runtime.h"
#include "spconf.h"
#include "spkey.h"
#include "spperif.h"
#include "spscr.h"
#include "spsound.h"
#include "sptape.h"
#include "snapshot.h"
#include "tapefile.h"
#include "vgascr.h"
#include "z80.h"
#include "ay_sound.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const app_services_t *_papp_svc;

/* Globals normally supplied by spmain.c. The PAPP owns the emulation loop,
 * so it supplies only the state the reusable core and key handler need. */
int endofsingle;
int sp_nosync;
int showframe = 1;
int load_immed;
qbyte sp_int_ctr;
uint16_t *sp_framebuffer;
extern int small_screen;

static jmp_buf exit_env;
static unsigned spectrum_frame_count;
static unsigned spectrum_changes;
static unsigned spectrum_last_hash;

int64_t papp_esp_timer_get_time(void)
{
    return _papp_svc ? _papp_svc->get_time_us() : 0;
}

void app_return_to_launcher(void) { longjmp(exit_env, 1); }

/* sptiming.c is intentionally not part of this PAPP: its POSIX select-based
 * wait cannot run in the loader context. The main loop paces through the
 * launcher's delay service instead. */
void spti_init(void) {}
void spti_sleep(unsigned long usecs)
{
    if (_papp_svc && usecs) _papp_svc->delay_ms((int)((usecs + 999) / 1000));
}
void spti_reset(void) {}
void spti_wait(void) {}

/* Snapshot/tape loading calls this helper in the desktop configuration. A
 * PAPP has already validated the sidecar extension, so no probing or file
 * substitution is needed here. */
int spcf_find_file_type(char *filename, int *ftp, int *ftsubp)
{
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;
    if (!strcasecmp(dot, ".z80")) {
        if (ftp) *ftp = FT_SNAPSHOT;
        if (ftsubp) *ftsubp = SN_Z80;
        return 1;
    }
    if (!strcasecmp(dot, ".sna")) {
        if (ftp) *ftp = FT_SNAPSHOT;
        if (ftsubp) *ftsubp = SN_SNA;
        return 1;
    }
    if (!strcasecmp(dot, ".tzx")) {
        if (ftp) *ftp = FT_TAPEFILE;
        if (ftsubp) *ftsubp = TAP_TZX;
        return 1;
    }
    if (!strcasecmp(dot, ".tap")) {
        if (ftp) *ftp = FT_TAPEFILE;
        if (ftsubp) *ftsubp = TAP_TAP;
        return 1;
    }
    return 0;
}

static unsigned spectrum_frame_hash(void)
{
    unsigned h = 2166136261u;
    for (int y = 0; y < 240; y += 7)
        for (int x = 0; x < 320; x += 11)
            h = (h ^ sp_framebuffer[y * 320 + x]) * 16777619u;
    return h;
}

static void spectrum_render_frame(void)
{
    translate_screen();
    _papp_svc->display_write_frame_custom(sp_framebuffer, 320, 240,
                                          2.0f, false);

#ifdef PAPP_TEST_FRAMES
    unsigned hash = spectrum_frame_hash();
    if (spectrum_frame_count && hash != spectrum_last_hash)
        ++spectrum_changes;
    spectrum_last_hash = hash;
#endif
    ++spectrum_frame_count;
}

static void spectrum_load_media(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot && (!strcasecmp(dot, ".tzx") || !strcasecmp(dot, ".tap"))) {
        int type = !strcasecmp(dot, ".tzx") ? TAP_TZX : TAP_TAP;
        start_play_file_type((char *)path, 0, type);
        play_tape();
        _papp_svc->log_printf("Spectrum PAPP: playing tape %s\n", path);
    } else {
        int type = (dot && !strcasecmp(dot, ".sna")) ? SN_SNA : SN_Z80;
        load_snapshot_file_type((char *)path, type);
        _papp_svc->log_printf("Spectrum PAPP: loaded snapshot %s\n", path);
    }
}

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;

    char media_path[512];
    if (emu_rom_path("spectrum", ".z80|.sna|.tap|.tzx|",
                     media_path, sizeof(media_path))) {
        svc->log_printf("Spectrum: set a .z80, .sna, .tap, or .tzx path in "
                        "/sd/roms/papp/spectrum.rom\n");
        return -1;
    }

    int result = -1;
    if (setjmp(exit_env)) goto cleanup;

    sp_framebuffer = calloc(320 * 240, sizeof(*sp_framebuffer));
    if (!sp_framebuffer) app_return_to_launcher();

    /* The core produces one 624-sample block per 50 Hz Spectrum frame. */
    ay_init(31200);
    svc->audio_init(31200);

    small_screen = 1;
    sp_nosync = 0;
    showframe = 1;
    sp_paused = 0;
    sp_int_ctr = 0;
    endofsingle = 0;

    sp_init();
    init_spect_scr();
    init_spect_key();
    spectrum_load_media(media_path);

    svc->log_printf("Spectrum PAPP: running %s\n", media_path);

    int t = 0;
    emu_clock clock = { svc->get_time_us(), 0, 50 };
    spectrum_frame_count = 0;
    spectrum_changes = 0;
    spectrum_last_hash = 0;

    for (;;) {
        papp_gamepad_state_t pad;
        svc->input_gamepad_read(&pad);
#ifdef PAPP_TEST_FRAMES
        memset(&pad, 0, sizeof(pad));
#endif
        if (emu_quit(&pad)) break;

        int evenframe = !(sp_int_ctr & 1);
        if (!(sp_int_ctr % 25)) {
            sp_flash_state = !sp_flash_state;
            flash_change();
        }
        if (evenframe) {
            play_tape();
            sp_scline = 0;
        }

        spkb_process_events(evenframe);
        sp_updating = 1;
        t += CHKTICK;
        t = sp_halfframe(t, evenframe ? EVENHF : ODDHF);
        if (SPNM(load_trapped)) {
            SPNM(load_trapped) = 0;
            DANM(haltstate) = 0;
            qload();
        }
        z80_interrupt(0xFF);
        ++sp_int_ctr;
        if (!evenframe) rec_tape();

        /* The odd half completes a full 50 Hz frame and submits the sound
         * block assembled during both halves. */
        if (!evenframe) {
            sp_border_update >>= 1;
            sp_imag_vert = sp_imag_horiz = 0;
            spectrum_render_frame();
            play_sound(0);
            emu_pace(&clock);

#ifdef PAPP_TEST_FRAMES
            if (spectrum_frame_count >= PAPP_TEST_FRAMES) break;
#endif
        }
    }

    result = 0;

cleanup:
    svc->log_printf("Spectrum PAPP: frames=%u changed=%u\n",
                    spectrum_frame_count, spectrum_changes);
    papp_close_streams();
    papp_cleanup_fds();
    papp_cleanup_heap();
    svc->display_clear(0);
    svc->display_flush();
    sp_framebuffer = NULL;
    return result;
}
