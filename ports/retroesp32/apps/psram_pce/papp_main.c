/* HuExpress PC Engine / TurboGrafx-16 PAPP entry point. */
#define PAPP_APP_SIDE 1
#include "runtime.h"
#include "pce.h"
#include "mix.h"
#include "zipmgr.h"
#include <setjmp.h>
#include <strings.h>

const app_services_t *_papp_svc;

extern char *rom_file_name;
extern char *syscard_filename;
extern uchar *SPM_raw;

static volatile int pce_exit_requested;
bool skipNextFrame = false;
void app_return_to_launcher(void) { pce_exit_requested = 1; }
void HCD_shutdown(void) {}
int rmdir(const char *path) { (void)path; return 0; }

#define PCE_OUTPUT_W       256
#define PCE_OUTPUT_H       240
#define PCE_AUDIO_RATE     22050
#define PCE_AUDIO_CAP      768
#define PCE_AUDIO_CHANNELS 6
#define PCE_INDEX_STRIDE   XBUF_WIDTH

/* HuExpress expects these buffers to be present while the CPU is running.
 * Keep the indexed render buffer in PSRAM and convert only the visible area
 * when a frame is complete. */
static unsigned char *pce_xbuf;
static unsigned char *pce_spm_raw;
static uint32 *pce_sprite_positions;
static uint16_t *pce_palette;
static uint16_t *pce_pixels;
static char *pce_audio_channels[PCE_AUDIO_CHANNELS];
static int16_t *pce_audio;
static unsigned pce_frame_count;
static unsigned pce_changes;
static unsigned pce_last_hash;
static int pce_audio_peak;
static unsigned pce_audio_phase;
static int64_t pce_next_deadline;

/* Globals declared by the HuExpress OSD contract. */
unsigned char *osd_gfx_buffer;
char *sbuf[PCE_AUDIO_CHANNELS];

static void pce_free(void *p)
{
    if (p) free(p);
}

void *my_special_alloc(unsigned char speed, unsigned char bytes, unsigned long size)
{
    (void)speed;
    (void)bytes;
    void *p = malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

/* ZIP loading is deliberately not part of this PAPP. The SD sidecar points
 * to a raw .pce image, avoiding a large decompressor and temporary files. */
uint32 zipmgr_probe_file(char *zipFilename, char *foundGameFile)
{
    (void)zipFilename;
    (void)foundGameFile;
    return ZIP_ERROR;
}
uint32 zipmgr_extract_to_disk(char *zipFilename, char *destination)
{
    (void)zipFilename;
    (void)destination;
    return 0;
}
char *zipmgr_extract_to_memory(char *zipFilename, char *cartFilename, size_t *cartSize)
{
    (void)zipFilename;
    (void)cartFilename;
    (void)cartSize;
    return NULL;
}

static unsigned pce_frame_hash(void)
{
    unsigned h = 2166136261u;
    for (int y = 0; y < PCE_OUTPUT_H; y += 7)
        for (int x = 0; x < PCE_OUTPUT_W; x += 11)
            h = (h ^ pce_pixels[y * PCE_OUTPUT_W + x]) * 16777619u;
    return h;
}

static void pce_emit_audio(void)
{
    /* Bresenham-style fractional sample count: 22050 / 60 = 367.5. */
    pce_audio_phase += PCE_AUDIO_RATE;
    unsigned count = pce_audio_phase / 60;
    pce_audio_phase %= 60;
    if (count > PCE_AUDIO_CAP) count = PCE_AUDIO_CAP;

    for (int ch = 0; ch < PCE_AUDIO_CHANNELS; ch++)
        WriteBuffer(pce_audio_channels[ch], ch, count * 2);

    int lvol = (int)((io.psg_volume >> 4) * 1.22f);
    int rvol = (int)((io.psg_volume & 0x0F) * 1.22f);
    for (unsigned i = 0; i < count; i++) {
        int left = 0, right = 0;
        for (int ch = 0; ch < PCE_AUDIO_CHANNELS; ch++) {
            left += (signed char)pce_audio_channels[ch][2 * i];
            right += (signed char)pce_audio_channels[ch][2 * i + 1];
        }
        int l = left * lvol;
        int r = right * rvol;
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        pce_audio[2 * i] = (int16_t)l;
        pce_audio[2 * i + 1] = (int16_t)r;
        int al = l < 0 ? -l : l;
        int ar = r < 0 ? -r : r;
        if (al > pce_audio_peak) pce_audio_peak = al;
        if (ar > pce_audio_peak) pce_audio_peak = ar;
    }
    _papp_svc->audio_submit(pce_audio, (int)count);
}

static int pce_gfx_init(void) { return 1; }
static int pce_gfx_mode(void) { return 1; }
static void pce_gfx_shut(void) {}

static void pce_gfx_draw(void)
{
    int src_w = io.screen_w;
    int src_h = io.screen_h;
    if (src_w <= 0 || src_w > PCE_OUTPUT_W) src_w = PCE_OUTPUT_W;
    if (src_h <= 0 || src_h > PCE_OUTPUT_H) src_h = 224;

    memset(pce_pixels, 0, PCE_OUTPUT_W * PCE_OUTPUT_H * sizeof(*pce_pixels));
    int dst_x = (PCE_OUTPUT_W - src_w) / 2;
    int dst_y = (PCE_OUTPUT_H - src_h) / 2;
    for (int y = 0; y < src_h; y++) {
        const unsigned char *src = osd_gfx_buffer + y * PCE_INDEX_STRIDE;
        uint16_t *dst = pce_pixels + (dst_y + y) * PCE_OUTPUT_W + dst_x;
        for (int x = 0; x < src_w; x++) dst[x] = pce_palette[src[x]];
    }

    _papp_svc->display_write_frame_custom(pce_pixels, PCE_OUTPUT_W,
                                          PCE_OUTPUT_H, 2.0f, false);
    pce_emit_audio();

    unsigned hash = pce_frame_hash();
    if (pce_frame_count && hash != pce_last_hash) pce_changes++;
    pce_last_hash = hash;
    pce_frame_count++;

    if (pce_next_deadline == 0) pce_next_deadline = _papp_svc->get_time_us();
    pce_next_deadline += 1000000 / 60;
    int64_t now = _papp_svc->get_time_us();
    if (pce_next_deadline > now)
        _papp_svc->delay_ms((int)((pce_next_deadline - now) / 1000));
    else if (now - pce_next_deadline > 50000)
        pce_next_deadline = now;

#ifdef PAPP_TEST_FRAMES
    if (pce_frame_count >= PAPP_TEST_FRAMES) pce_exit_requested = 1;
#endif
}

osd_gfx_driver osd_gfx_driver_list[1] = {
    { pce_gfx_init, pce_gfx_mode, pce_gfx_draw, pce_gfx_shut }
};

void osd_gfx_set_color(uchar index, uchar r, uchar g, uchar b)
{
    /* HuExpress supplies 6-bit VCE components. */
    uint8_t r8 = (uint8_t)((r << 2) | (r >> 4));
    uint8_t g8 = (uint8_t)((g << 2) | (g >> 4));
    uint8_t b8 = (uint8_t)((b << 2) | (b >> 4));
    pce_palette[index] = (uint16_t)(((r8 << 8) & 0xF800) |
                                    ((g8 << 3) & 0x07E0) | (b8 >> 3));
}
void osd_gfx_set_message(char *message)
{
    if (_papp_svc && message) _papp_svc->log_printf("PCE: %s\n", message);
}
uint16 osd_gfx_savepict(void) { return 0; }

void osd_snd_set_volume(uchar volume) { (void)volume; }
int osd_snd_init_sound(void) { return 1; }
void osd_snd_trash_sound(void) {}
int osd_init_machine(void) { return 0; }
void osd_shut_machine(void) {}
int osd_init_input(void) { return 0; }
void osd_shutdown_input(void) {}
char osd_keypressed(void) { return 0; }
uint16 osd_readkey(void) { return 0; }

int osd_keyboard(void)
{
    papp_gamepad_state_t pad;
    _papp_svc->input_gamepad_read(&pad);
    io.JOY[0] = 0;
    if (pad.values[PAPP_INPUT_LEFT])  io.JOY[0] |= 0x80;
    if (pad.values[PAPP_INPUT_RIGHT]) io.JOY[0] |= 0x20;
    if (pad.values[PAPP_INPUT_UP])    io.JOY[0] |= 0x10;
    if (pad.values[PAPP_INPUT_DOWN])  io.JOY[0] |= 0x40;
    if (pad.values[PAPP_INPUT_A])     io.JOY[0] |= 0x01;
    if (pad.values[PAPP_INPUT_B])     io.JOY[0] |= 0x02;
    if (pad.values[PAPP_INPUT_SELECT]) io.JOY[0] |= 0x04;
    if (pad.values[PAPP_INPUT_START])  io.JOY[0] |= 0x08;
    if (pad.values[PAPP_INPUT_MENU] ||
        (_papp_svc->input_l3_read && _papp_svc->input_l3_read()) ||
        pce_exit_requested)
        return 1;
    return 0;
}

/* The generic engine references these OSD CD operations even for cartridge
 * builds. This PAPP intentionally supports raw HuCard images only. */
int osd_cd_init(char *device) { (void)device; return 1; }
void osd_cd_stop_audio(void) {}
void osd_cd_close(void) {}
void osd_cd_read(uchar *p, uint32 sector) { (void)sector; memset(p, 0, 2048); }
void osd_cd_subchannel_info(uint16 offset) { (void)offset; }
void osd_cd_status(int *status) { if (status) *status = 0; }
void osd_cd_track_info(uchar track, int *min, int *sec, int *fra, int *control)
{ (void)track; if (min) *min = 0; if (sec) *sec = 0; if (fra) *fra = 0; if (control) *control = 0; }
void osd_cd_nb_tracks(int *first, int *last) { if (first) *first = 1; if (last) *last = 1; }
void osd_cd_length(int *min, int *sec, int *fra) { if (min) *min = 0; if (sec) *sec = 0; if (fra) *fra = 0; }
void osd_cd_pause(void) {}
void osd_cd_resume(void) {}
void osd_cd_play_audio_track(uchar track) { (void)track; }
void osd_cd_play_audio_range(uchar min_from, uchar sec_from, uchar fra_from,
                             uchar min_to, uchar sec_to, uchar fra_to)
{ (void)min_from; (void)sec_from; (void)fra_from; (void)min_to; (void)sec_to; (void)fra_to; }

static int pce_load_sidecar(char *path, size_t size)
{
    return emu_rom_path("pce", ".pce|", path, size);
}

static void pce_free_paths(void)
{
    pce_free(cart_name); cart_name = NULL;
    pce_free(short_cart_name); short_cart_name = NULL;
    pce_free(short_iso_name); short_iso_name = NULL;
    pce_free(rom_file_name); rom_file_name = NULL;
    pce_free(config_basepath); config_basepath = NULL;
    pce_free(sav_path); sav_path = NULL;
    pce_free(sav_basepath); sav_basepath = NULL;
    pce_free(tmp_basepath); tmp_basepath = NULL;
    pce_free(video_path); video_path = NULL;
    pce_free(ISO_filename); ISO_filename = NULL;
    pce_free(syscard_filename); syscard_filename = NULL;
    pce_free(cdsystem_path); cdsystem_path = NULL;
    pce_free(log_filename); log_filename = NULL;
}

static int pce_alloc_paths(void)
{
    char **paths[] = { &cart_name, &short_cart_name, &short_iso_name,
        &rom_file_name, &config_basepath, &sav_path, &sav_basepath,
        &tmp_basepath, &video_path, &ISO_filename, &syscard_filename,
        &cdsystem_path, &log_filename };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        *paths[i] = calloc(1, 512);
        if (!*paths[i]) return -1;
    }
    strcpy(config_basepath, "/sd/roms/papp");
    strcpy(sav_basepath, "/sd/roms/papp");
    strcpy(sav_path, "pce");
    return 0;
}

static int pce_alloc_buffers(void)
{
    pce_xbuf = calloc(XBUF_WIDTH * XBUF_HEIGHT, 1);
    pce_spm_raw = calloc(XBUF_WIDTH * XBUF_HEIGHT, 1);
    pce_sprite_positions = calloc(1024, sizeof(*pce_sprite_positions));
    pce_palette = calloc(256, sizeof(*pce_palette));
    pce_pixels = calloc(PCE_OUTPUT_W * PCE_OUTPUT_H, sizeof(*pce_pixels));
    for (int i = 0; i < PCE_AUDIO_CHANNELS; i++)
        pce_audio_channels[i] = calloc(PCE_AUDIO_CAP * 2, 1);
    pce_audio = calloc(PCE_AUDIO_CAP * 2, sizeof(*pce_audio));
    if (!pce_xbuf || !pce_spm_raw || !pce_sprite_positions || !pce_palette ||
        !pce_pixels || !pce_audio) return -1;
    for (int i = 0; i < PCE_AUDIO_CHANNELS; i++)
        if (!pce_audio_channels[i]) return -1;
    osd_gfx_buffer = pce_xbuf + 32 + 64 * XBUF_WIDTH;
    SPM_raw = pce_spm_raw;
    SPM = pce_spm_raw + 64 * XBUF_WIDTH + 32;
    spr_init_pos = pce_sprite_positions;
    for (int i = 0; i < PCE_AUDIO_CHANNELS; i++) sbuf[i] = pce_audio_channels[i];
    return 0;
}

static void pce_free_buffers(void)
{
    pce_free(pce_xbuf); pce_xbuf = NULL; osd_gfx_buffer = NULL;
    pce_free(pce_spm_raw); pce_spm_raw = NULL; SPM_raw = NULL; SPM = NULL;
    pce_free(pce_sprite_positions); pce_sprite_positions = NULL; spr_init_pos = NULL;
    pce_free(pce_palette); pce_palette = NULL;
    pce_free(pce_pixels); pce_pixels = NULL;
    for (int i = 0; i < PCE_AUDIO_CHANNELS; i++) {
        pce_free(pce_audio_channels[i]);
        pce_audio_channels[i] = NULL;
        sbuf[i] = NULL;
    }
    pce_free(pce_audio); pce_audio = NULL;
}

__attribute__((section(".text.entry")))
int app_entry(const app_services_t *svc)
{
    if (!svc || svc->abi_version != PAPP_ABI_VERSION) return -1;
    _papp_svc = svc;
    char rom_path[512];
    if (pce_load_sidecar(rom_path, sizeof(rom_path))) {
        svc->log_printf("PCE PAPP: set /sd/roms/papp/pce.rom to a .pce file\n");
        return -1;
    }

    int result = -1;
    if (pce_alloc_paths() == 0 && pce_alloc_buffers() == 0) {
        host.sound.stereo = 1;
        host.sound.signed_sound = 1;
        host.sound.freq = PCE_AUDIO_RATE;
        host.sound.sample_size = 1;
        option.want_supergraphx_emulation = 0;
        option.want_fullscreen_aspect = 1;
        video_driver = 0;
        UPeriod = 0;
        if (!InitPCE(rom_path)) {
            SetPalette();
            svc->audio_init(PCE_AUDIO_RATE);
            svc->log_printf("PCE PAPP: running %s\n", rom_path);
            pce_frame_count = 0;
            pce_changes = 0;
            pce_last_hash = 0;
            pce_audio_peak = 0;
            pce_audio_phase = 0;
            pce_next_deadline = 0;
            pce_exit_requested = 0;
            RunPCE();
            svc->log_printf("PCE test/runtime: frames=%u changed=%u audio_peak=%d\n",
                            pce_frame_count, pce_changes, pce_audio_peak);
            TrashPCE();
            result = 0;
        }
    }
    pce_free_buffers();
    pce_free_paths();
    papp_close_streams();
    papp_cleanup_fds();
    papp_cleanup_heap();
    svc->display_clear(0);
    svc->display_flush();
    return result;
}
