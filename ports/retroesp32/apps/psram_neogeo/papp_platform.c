/* Minimal, single-threaded platform for the GnGeo PAPP port. */
#define PAPP_APP_SIDE 1

#include "runtime.h"
#include "SDL.h"
#include "emu.h"
#include "screen.h"
#include "conf.h"
#include "sound.h"
#include "event.h"
#include "menu.h"
#include "messages.h"
#include "video.h"
#include "memory.h"
#include "state.h"
#include "roms.h"
#include "debug.h"
#include "gnutil.h"
#include "perf_counters.h"
#include "stb_zlib.h"
#include "ym2610/ym2610.h"
#include "ff.h"
#include "diskio.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* The normal GnGeo platform has separate RTOS tasks for video and sound.
 * PAPPs already have a frame-paced launcher service, so keeping both paths on
 * the emulator thread avoids task/queue lifetime problems when the app closes. */

static SDL_PixelFormat neo_format = {
    .BitsPerPixel = 16, .BytesPerPixel = 2,
    .Rmask = 0xF800, .Gmask = 0x07E0, .Bmask = 0x001F, .Amask = 0
};
static SDL_Surface neo_buffer_surface;
static SDL_Surface neo_screen_surface;
static uint16_t *neo_buffer_pixels;
static uint16_t *neo_screen_pixels;

SDL_Surface *screen;
SDL_Surface *buffer;
SDL_Surface *sprbuf;
SDL_Surface *fps_buf;
SDL_Surface *scan;
SDL_Surface *fontbuf;
SDL_Rect visible_area;
int yscreenpadding;
Uint8 interpolation;
Uint8 nblitter;
Uint8 neffect;
Uint8 scale;
Uint8 fullscreen;

int pending_save_state;
int pending_load_state;
int show_fps;
int autoframeskip;
int sleep_idle;
int show_keysym;
char input_buf[256];

Uint8 key[SDLK_LAST];
Uint8 *joy_button[2];
Sint32 *joy_axe[2];
Uint32 joy_numaxes[2];

SDL_Surface *state_img;
Uint8 state_version = ST_VER3;
char fps_str[32];
struct gngeo_conf conf;
RGB2YUV *rgb2yuv;

SDL_AudioSpec *desired;
SDL_AudioSpec *obtain;
Uint16 *play_buffer;

static SDL_AudioSpec neo_desired;
static SDL_AudioSpec neo_obtain;
static uint16_t *neo_audio;
static unsigned neo_audio_samples;
static volatile int neo_nmi_pending;
static unsigned neo_frame_count;
static int neo_quit_requested;
static char neo_rompath[CF_MAXSTRLEN] = "/sd/roms/neogeo";

void neogeo_papp_log(const char *fmt, ...)
{
    if (!_papp_svc || !_papp_svc->log_vprintf) return;
    va_list ap;
    va_start(ap, fmt);
    _papp_svc->log_vprintf(fmt, ap);
    va_end(ap);
}

#define papp_log neogeo_papp_log

void papp_delay_ms(int ms)
{
    if (_papp_svc && ms > 0) _papp_svc->delay_ms(ms);
}

int64_t papp_esp_timer_get_time(void)
{
    return _papp_svc ? _papp_svc->get_time_us() : 0;
}

/* ── Video ──────────────────────────────────────────────────────────── */

int screen_init(void)
{
    if (neo_buffer_pixels) return 0;

    neo_buffer_pixels = (uint16_t *)calloc(352u * 256u, sizeof(uint16_t));
    neo_screen_pixels = (uint16_t *)calloc(352u * 256u, sizeof(uint16_t));
    if (!neo_buffer_pixels || !neo_screen_pixels) return -1;

    memset(&neo_buffer_surface, 0, sizeof(neo_buffer_surface));
    neo_buffer_surface.format = &neo_format;
    neo_buffer_surface.w = 352;
    neo_buffer_surface.h = 256;
    neo_buffer_surface.pitch = 352 * 2;
    neo_buffer_surface.pixels = neo_buffer_pixels;
    neo_buffer_surface.clip_rect.w = 352;
    neo_buffer_surface.clip_rect.h = 256;
    buffer = &neo_buffer_surface;

    memset(&neo_screen_surface, 0, sizeof(neo_screen_surface));
    neo_screen_surface.format = &neo_format;
    neo_screen_surface.w = 352;
    neo_screen_surface.h = 256;
    neo_screen_surface.pitch = 352 * 2;
    neo_screen_surface.pixels = neo_screen_pixels;
    neo_screen_surface.clip_rect.w = 352;
    neo_screen_surface.clip_rect.h = 256;
    screen = &neo_screen_surface;
    sprbuf = buffer;
    fps_buf = NULL;
    scan = NULL;
    fontbuf = NULL;

    visible_area.x = 16;
    visible_area.y = 16;
    visible_area.w = 304;
    visible_area.h = 224;

    state_img = SDL_CreateRGBSurface(SDL_SWSURFACE, 304, 224, 16,
                                     0xF800, 0x07E0, 0x001F, 0);
    if (!state_img) return -1;
    return 0;
}

int screen_reinit(void) { return neo_buffer_pixels ? 0 : screen_init(); }
int screen_resize(int w, int h) { (void)w; (void)h; return 0; }
void screen_fullscreen(void) { fullscreen = !fullscreen; }
void init_sdl(void) { (void)screen_init(); }

void screen_update(void)
{
    if (!buffer || !_papp_svc || !_papp_svc->display_write_frame_custom) return;

    /* GnGeo renders into a 352×256 working surface.  The actual Neo Geo
     * picture is the centered 304×224 viewport. */
    for (int y = 0; y < 224; ++y) {
        memcpy(neo_screen_pixels + y * 304,
               neo_buffer_pixels + (y + 16) * 352 + 16,
               304 * sizeof(uint16_t));
    }
    _papp_svc->display_write_frame_custom(neo_screen_pixels, 304, 224,
                                          2.0f, false);
    ++neo_frame_count;
    if ((neo_frame_count % 60u) == 0u) {
        papp_log("NEO GEO PAPP: video frames=%u sample_pixels=%04x,%04x,%04x\n",
                 neo_frame_count, neo_screen_pixels[0],
                 neo_screen_pixels[304 * 112 + 152], neo_screen_pixels[304 * 224 - 1]);
    }
}

void screen_close(void)
{
    if (state_img) {
        SDL_FreeSurface(state_img);
        state_img = NULL;
    }
    free(neo_buffer_pixels);
    free(neo_screen_pixels);
    neo_buffer_pixels = NULL;
    neo_screen_pixels = NULL;
    screen = NULL;
    buffer = NULL;
    sprbuf = NULL;
}

void init_rgb2yuv_table(void) { }

Uint8 get_effect_by_name(char *name) { (void)name; return 0; }
Uint8 get_blitter_by_name(char *name) { (void)name; return 0; }
void print_blitter_list(void) { }
void print_effect_list(void) { }
LIST *create_effect_list(void) { return NULL; }
LIST *create_blitter_list(void) { return NULL; }

/* ── Audio and Z80 ─────────────────────────────────────────────────── */

int init_sdl_audio(void)
{
    neo_audio_samples = (unsigned)conf.sample_rate / 60u;
    if (neo_audio_samples < 1) neo_audio_samples = 1;
    neo_audio = (uint16_t *)calloc(neo_audio_samples * 2u, sizeof(uint16_t));
    if (!neo_audio) return -1;
    play_buffer = neo_audio;

    memset(&neo_desired, 0, sizeof(neo_desired));
    neo_desired.freq = conf.sample_rate;
    neo_desired.format = AUDIO_S16;
    neo_desired.channels = 2;
    neo_desired.samples = (uint16_t)neo_audio_samples;
    neo_desired.size = neo_audio_samples * 2u * sizeof(int16_t);
    neo_obtain = neo_desired;
    desired = &neo_desired;
    obtain = &neo_obtain;
    if (_papp_svc && _papp_svc->audio_init)
        _papp_svc->audio_init(conf.sample_rate);
    return 0;
}

void close_sdl_audio(void)
{
    if (_papp_svc && _papp_svc->audio_init) _papp_svc->audio_init(0);
    free(neo_audio);
    neo_audio = NULL;
    play_buffer = NULL;
}

void pause_audio(int on) { (void)on; }

void esp32_z80_queue_nmi(void) { neo_nmi_pending = 1; }
void esp32_z80_start_frame(void) { }

void esp32_z80_wait_frame(void)
{
    static unsigned neo_audio_frames;
    if (!conf.sound || !play_buffer) {
        my_timer();
        return;
    }
    if (neo_nmi_pending) {
        cpu_z80_nmi();
        cpu_z80_run(300);
        neo_nmi_pending = 0;
    }
    cpu_z80_run(73333);
    YM2610Update_stream((int)neo_audio_samples);
    int peak = 0;
    for (unsigned i = 0; i < neo_audio_samples * 2u; ++i) {
        int value = ((int16_t *)play_buffer)[i];
        if (value < 0) value = -value;
        if (value > peak) peak = value;
    }
    if (_papp_svc && _papp_svc->audio_submit)
        _papp_svc->audio_submit((short *)play_buffer, (int)neo_audio_samples);
    ++neo_audio_frames;
    if ((neo_audio_frames % 60u) == 0u)
        papp_log("NEO GEO PAPP: audio frames=%u peak=%d\n",
                 neo_audio_frames, peak);
}

/* ── Input ──────────────────────────────────────────────────────────── */

JOYMAP *jmap;
Uint8 joy_state[2][GN_MAX_KEY];

int init_event(void) { memset(joy_state, 0, sizeof(joy_state)); return 0; }
void reset_event(void) { }
int wait_event(void) { return 0; }
int create_joymap_from_string(int player, char *jconf)
{ (void)player; (void)jconf; return 0; }

int handle_event(void)
{
    papp_gamepad_state_t pad;
    memset(&pad, 0, sizeof(pad));
    if (_papp_svc && _papp_svc->input_gamepad_read)
        _papp_svc->input_gamepad_read(&pad);

    joy_state[0][GN_UP] = (Uint8)pad.values[PAPP_INPUT_UP];
    joy_state[0][GN_DOWN] = (Uint8)pad.values[PAPP_INPUT_DOWN];
    joy_state[0][GN_LEFT] = (Uint8)pad.values[PAPP_INPUT_LEFT];
    joy_state[0][GN_RIGHT] = (Uint8)pad.values[PAPP_INPUT_RIGHT];
    joy_state[0][GN_A] = (Uint8)pad.values[PAPP_INPUT_A];
    joy_state[0][GN_B] = (Uint8)pad.values[PAPP_INPUT_B];
    joy_state[0][GN_C] = (Uint8)pad.values[PAPP_INPUT_X];
    joy_state[0][GN_D] = (Uint8)pad.values[PAPP_INPUT_Y];
    joy_state[0][GN_START] = (Uint8)pad.values[PAPP_INPUT_START];
    joy_state[0][GN_SELECT_COIN] = (Uint8)pad.values[PAPP_INPUT_SELECT];

    if (pad.values[PAPP_INPUT_MENU] ||
        (_papp_svc->input_l3_read && _papp_svc->input_l3_read()))
        neo_quit_requested = 1;
#ifdef PAPP_TEST_FRAMES
    if (neo_frame_count >= PAPP_TEST_FRAMES) neo_quit_requested = 1;
#endif
    return neo_quit_requested ? 1 : 0;
}

void update_p1_key(void)
{
    Uint8 value = 0xFF;
    if (joy_state[0][GN_UP]) value &= (Uint8)~(1u << 0);
    if (joy_state[0][GN_DOWN]) value &= (Uint8)~(1u << 1);
    if (joy_state[0][GN_LEFT]) value &= (Uint8)~(1u << 2);
    if (joy_state[0][GN_RIGHT]) value &= (Uint8)~(1u << 3);
    if (joy_state[0][GN_A]) value &= (Uint8)~(1u << 4);
    if (joy_state[0][GN_B]) value &= (Uint8)~(1u << 5);
    if (joy_state[0][GN_C]) value &= (Uint8)~(1u << 6);
    if (joy_state[0][GN_D]) value &= (Uint8)~(1u << 7);
    memory.intern_p1 = value;
}
void update_p2_key(void) { memory.intern_p2 = 0xFF; }
void update_start(void)
{
    memory.intern_start = joy_state[0][GN_START] ? 0xFE : 0xFF;
}
void update_coin(void)
{
    memory.intern_coin = joy_state[0][GN_SELECT_COIN] ? 0x3E : 0x3F;
}

/* ── Menu/loading/message/debug compatibility ──────────────────────── */

Uint32 run_menu(void)
{
    if (neo_quit_requested) {
        neo_quit_requested = 0;
        return 2;
    }
    return 0;
}
int gn_init_skin(void) { return 0; }
void gn_reset_pbar(void) { }
void gn_init_pbar(char *name, int size)
{ papp_log("Neo Geo: loading %s (%d bytes)\n", name ? name : "ROM", size); }
void gn_update_pbar(int pos) { (void)pos; }
void gn_terminate_pbar(void) { }
void gn_loading_info(const char *msg)
{ if (msg) papp_log("Neo Geo: %s\n", msg); }
void gn_set_loading_game(const char *name) { (void)name; }
void gn_popup_error(char *name, char *fmt, ...)
{
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    papp_log("Neo Geo error (%s): %s\n", name ? name : "", msg);
}
int gn_popup_question(char *name, char *fmt, ...)
{ (void)name; (void)fmt; return 0; }

static CONF_ITEM cf_rompath = {
    .name = "rompath", .type = CFT_STRING,
    .data.dt_str = { .str = "/sd/roms/neogeo" }
};
static CONF_ITEM cf_biospath = {
    .name = "biospath", .type = CFT_STRING,
    .data.dt_str = { .str = "/sd/roms/neogeo" }
};
static CONF_ITEM cf_datafile = {
    .name = "datafile", .type = CFT_STRING,
    .data.dt_str = { .str = "/sd/roms/neogeo/gngeo_data.zip" }
};
static CONF_ITEM cf_blitter = {
    .name = "blitter", .type = CFT_STRING,
    .data.dt_str = { .str = "soft" }
};
static CONF_ITEM cf_effect = {
    .name = "effect", .type = CFT_STRING,
    .data.dt_str = { .str = "none" }
};
static CONF_ITEM cf_transpack = {
    .name = "transpack", .type = CFT_STRING,
    .data.dt_str = { .str = "" }
};
static CONF_ITEM cf_68kclock = {
    .name = "68kclock", .type = CFT_INT,
    .data.dt_int = { .val = 0 }
};
static CONF_ITEM cf_z80clock = {
    .name = "z80clock", .type = CFT_INT,
    .data.dt_int = { .val = 0 }
};
static CONF_ITEM cf_raster = {
    .name = "raster", .type = CFT_BOOLEAN,
    .data.dt_bool = { .boolean = 0 }
};
static CONF_ITEM cf_sound = {
    .name = "sound", .type = CFT_BOOLEAN,
    .data.dt_bool = { .boolean = 1 }
};
static CONF_ITEM cf_scale = {
    .name = "scale", .type = CFT_INT,
    .data.dt_int = { .val = 1 }
};
static CONF_ITEM cf_showfps = {
    .name = "showfps", .type = CFT_BOOLEAN,
    .data.dt_bool = { .boolean = 0 }
};
static CONF_ITEM cf_autoframeskip = {
    .name = "autoframeskip", .type = CFT_BOOLEAN,
    .data.dt_bool = { .boolean = 0 }
};
static CONF_ITEM cf_sleepidle = {
    .name = "sleepidle", .type = CFT_BOOLEAN,
    .data.dt_bool = { .boolean = 0 }
};
static CONF_ITEM *cf_items[] = {
    &cf_rompath, &cf_biospath, &cf_datafile, &cf_blitter, &cf_effect,
    &cf_transpack, &cf_68kclock, &cf_z80clock, &cf_raster, &cf_sound,
    &cf_scale, &cf_showfps, &cf_autoframeskip, &cf_sleepidle, NULL
};

CONF_ITEM *cf_get_item_by_name(const char *name)
{
    for (int i = 0; cf_items[i]; ++i)
        if (!strcmp(cf_items[i]->name, name)) return cf_items[i];
    static CONF_ITEM dummy = { .name = "dummy", .type = CFT_INT };
    return &dummy;
}
void cf_init(void) { }
void cf_reset_to_default(void) { }
int cf_open_file(char *filename) { (void)filename; return 0; }
int cf_save_file(char *filename, int flags)
{ (void)filename; (void)flags; return 0; }
int cf_save_option(char *filename, char *optname, int flags)
{ (void)filename; (void)optname; (void)flags; return 0; }
void cf_create_bool_item(const char *n, const char *h, char s, int d)
{ (void)n; (void)h; (void)s; (void)d; }
void cf_create_action_item(const char *n, const char *h, char s,
                           int (*a)(struct CONF_ITEM *))
{ (void)n; (void)h; (void)s; (void)a; }
void cf_create_action_arg_item(const char *n, const char *h, const char *ha,
                               char s, int (*a)(struct CONF_ITEM *))
{ (void)n; (void)h; (void)ha; (void)s; (void)a; }
void cf_create_string_item(const char *n, const char *h, const char *ha,
                           char s, const char *d)
{ (void)n; (void)h; (void)ha; (void)s; (void)d; }
void cf_create_int_item(const char *n, const char *h, const char *ha,
                        char s, int d)
{ (void)n; (void)h; (void)ha; (void)s; (void)d; }
void cf_create_array_item(const char *n, const char *h, const char *ha,
                          char s, int z, int *d)
{ (void)n; (void)h; (void)ha; (void)s; (void)z; (void)d; }
void cf_create_str_array_item(const char *n, const char *h, const char *ha,
                              char s, char *d)
{ (void)n; (void)h; (void)ha; (void)s; (void)d; }
void cf_init_cmd_line(void) { }
int cf_get_non_opt_index(int argc, char *argv[])
{ (void)argc; (void)argv; return 0; }
char *cf_parse_cmd_line(int argc, char *argv[])
{ (void)argc; (void)argv; return NULL; }
void cf_print_help(void) { }
void cf_item_has_been_changed(CONF_ITEM *item) { (void)item; }

void draw_message(const char *string) { (void)string; }
void stop_message(int param) { (void)param; }
void SDL_textout(SDL_Surface *dest, int x, int y, const char *string)
{ (void)dest; (void)x; (void)y; (void)string; }
void text_input(const char *message, int x, int y, char *string, int size)
{ (void)message; (void)x; (void)y; if (string && size) string[0] = 0; }
void error_box(char *fmt, ...)
{ (void)fmt; }

int dbg_step;
void show_bt(void) { }
void add_bt(Uint32 pc) { (void)pc; }
int check_bp(int pc) { (void)pc; return 0; }
void add_bp(int pc) { (void)pc; }
void del_bp(int pc) { (void)pc; }
int dbg_68k_run(Uint32 nbcycle) { (void)nbcycle; return 0; }

/* ── GnGeo configuration entry points used by the PAPP main ────────── */

void esp32_set_rompath(const char *path)
{
    if (!path) return;
    strncpy(neo_rompath, path, sizeof(neo_rompath) - 1);
    neo_rompath[sizeof(neo_rompath) - 1] = 0;
    strncpy(cf_rompath.data.dt_str.str, neo_rompath, CF_MAXSTRLEN - 1);
    cf_rompath.data.dt_str.str[CF_MAXSTRLEN - 1] = 0;
    strncpy(cf_biospath.data.dt_str.str, neo_rompath, CF_MAXSTRLEN - 1);
    cf_biospath.data.dt_str.str[CF_MAXSTRLEN - 1] = 0;
}

void esp32_enable_sound(int enable)
{
    cf_sound.data.dt_bool.boolean = enable;
    conf.sound = (Uint8)enable;
}

void esp32_init_conf(const char *game_name)
{
    memset(&conf, 0, sizeof(conf));
    conf.game = (char *)game_name;
    conf.x_start = 16;
    conf.y_start = 16;
    conf.res_x = 304;
    conf.res_y = 224;
    conf.sample_rate = 22050;
    conf.sound = 1;
    conf.system = SYS_ARCADE;
    conf.country = CTY_USA;
    conf.autoframeskip = 0;
    conf.show_fps = 0;
    conf.sleep_idle = 0;
    conf.screen320 = 0;
    conf.pal = 0;
    cf_sound.data.dt_bool.boolean = 1;
}

/* stb_zlib provides the inflater used by GnGeo's ZIP reader. */
int uncompress(uint8_t *dest, unsigned long *dest_len,
               const uint8_t *source, unsigned long source_len)
{
    zbuf z;
    memset(&z, 0, sizeof(z));
    z.zbuffer = (uint8_t *)source;
    z.zbuffer_end = (uint8_t *)source + source_len;
    int result = stbi_zlib_decode_noheader_stream(&z, (char *)dest,
                                                  (int)*dest_len);
    if (result < 0) return -1;
    *dest_len = (unsigned long)result;
    return 0;
}

/* The PAPP build always has the complete regions in PSRAM, so these are only
 * link-safe fallbacks for the legacy streaming code path. */
FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode)
{ (void)fp; (void)path; (void)mode; return FR_NO_FILE; }
FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{ (void)pdrv; (void)buff; (void)sector; (void)count; return RES_ERROR; }

/* POSIX calls retained by the legacy ZIP/cache implementation. */
int fsync(int fd) { (void)fd; return 0; }
int fileno(FILE *stream) { (void)stream; return -1; }
int nanosleep(const struct timespec *req, struct timespec *rem)
{
    (void)rem;
    if (req) {
        long ms = req->tv_sec * 1000L + req->tv_nsec / 1000000L;
        papp_delay_ms((int)ms);
    }
    return 0;
}

static int papp_stat_file(const char *path, struct stat *st)
{
    if (!path || !st || !_papp_svc || !_papp_svc->file_open) {
        errno = EINVAL;
        return -1;
    }
    void *f = _papp_svc->file_open(path, "rb");
    if (!f) {
        errno = ENOENT;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG;
    if (_papp_svc->file_seek(f, 0, SEEK_END) == 0)
        st->st_size = _papp_svc->file_tell(f);
    _papp_svc->file_close(f);
    return 0;
}

int lstat(const char *path, struct stat *st) { return papp_stat_file(path, st); }
int stat(const char *path, struct stat *st) { return papp_stat_file(path, st); }
int _stat(const char *path, struct stat *st) { return papp_stat_file(path, st); }
