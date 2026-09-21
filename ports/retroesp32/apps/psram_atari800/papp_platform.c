/* Launcher-backed platform functions for the Atari800 PAPP wrapper. */
#include "psram_app.h"
#include "odroid_audio.h"
#include "odroid_display.h"
#include "odroid_input.h"
#include "odroid_settings.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

extern const app_services_t *_papp_svc;

volatile int odroid_paddle_adc_raw = -1;
bool odroid_input_xy_menu_disable = false;
bool odroid_input_touch_buttons_disable = false;

void papp_delay_ms(int ms)
{
    if (ms > 0 && _papp_svc && _papp_svc->delay_ms) _papp_svc->delay_ms(ms);
}

int64_t papp_esp_timer_get_time(void)
{
    return _papp_svc && _papp_svc->get_time_us ? _papp_svc->get_time_us() : 0;
}

void odroid_paddle_adc_init(void)
{
    /* The launcher refreshes the optional paddle value while reading input. */
}

void odroid_audio_init(int sample_rate)
{
    if (_papp_svc && _papp_svc->audio_init) _papp_svc->audio_init(sample_rate);
}
void odroid_audio_terminate(void) {}
void odroid_audio_volume_set(int volume)
{
    if (_papp_svc && _papp_svc->settings_volume_set)
        _papp_svc->settings_volume_set(volume);
}
odroid_volume_level odroid_audio_volume_get(void)
{
    return _papp_svc && _papp_svc->settings_volume_get
        ? (odroid_volume_level)_papp_svc->settings_volume_get()
        : ODROID_VOLUME_LEVEL4;
}
void odroid_audio_volume_change(void)
{
    odroid_audio_volume_set((odroid_audio_volume_get() + 1) % 5);
}
void odroid_audio_submit(short *buffer, int frames)
{
    if (_papp_svc && _papp_svc->audio_submit)
        _papp_svc->audio_submit(buffer, frames);
}
void odroid_audio_submit_zero(void) {}
int odroid_audio_sample_rate_get(void) { return 15720; }

void ili9341_init(void) {}
void ili9341_write_frame_rgb565(const uint16_t *buffer)
{
    if (_papp_svc && _papp_svc->display_write_frame_rgb565)
        _papp_svc->display_write_frame_rgb565(buffer);
}
void ili9341_write_frame_rgb565_ex(const uint16_t *buffer, bool byte_swap_input)
{
    if (_papp_svc && _papp_svc->display_write_frame_custom)
        _papp_svc->display_write_frame_custom(buffer, 320, 240, 2.0f,
                                               byte_swap_input);
}
void ili9341_write_frame_rgb565_custom(const uint16_t *buffer, uint16_t w,
                                       uint16_t h, float scale,
                                       bool byte_swap_input)
{
    if (_papp_svc && _papp_svc->display_write_frame_custom)
        _papp_svc->display_write_frame_custom(buffer, w, h, scale,
                                               byte_swap_input);
}

char *odroid_util_GetFileName(const char *path)
{
    const char *slash = path ? strrchr(path, '/') : NULL;
    const char *base = slash ? slash + 1 : path;
    if (!base) return NULL;
    size_t len = strlen(base);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, base, len + 1);
    return copy;
}
char *odroid_util_GetFileExtenstion(const char *path)
{
    const char *dot = path;
    if (path) while (*path) { if (*path++ == '.') dot = path; }
    return (char *)dot;
}
char *odroid_util_GetFileNameWithoutExtension(const char *path)
{
    char *copy = odroid_util_GetFileName(path);
    if (!copy) return NULL;
    char *dot = strrchr(copy, '.');
    if (dot) *dot = 0;
    return copy;
}
void odroid_settings_RomFilePath_set(const char *path)
{
    if (_papp_svc && _papp_svc->settings_rom_path_set)
        _papp_svc->settings_rom_path_set(path);
}

int32_t odroid_settings_Volume_get(void)
{
    return _papp_svc && _papp_svc->settings_volume_get
        ? _papp_svc->settings_volume_get() : ODROID_VOLUME_LEVEL4;
}
void odroid_settings_Volume_set(int32_t value)
{
    if (_papp_svc && _papp_svc->settings_volume_set)
        _papp_svc->settings_volume_set(value);
}
ODROID_START_ACTION odroid_settings_StartAction_get(void)
{
    return ODROID_START_ACTION_NORMAL;
}
void odroid_settings_StartAction_set(ODROID_START_ACTION value) { (void)value; }

int _stat(const char *path, struct stat *st)
{
    if (!_papp_svc || !_papp_svc->file_open) return -1;
    void *f = _papp_svc->file_open(path, "rb");
    if (!f) return -1;
    if (st) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFREG;
        st->st_size = 0;
    }
    _papp_svc->file_close(f);
    return 0;
}

/* Atari800's optional device/file-management paths are not used by the
 * sidecar ROM launcher, but newlib still references these POSIX hooks. */
int rmdir(const char *path) { (void)path; return 0; }
int _fcntl(int fd, int cmd, int arg) { (void)fd; (void)cmd; (void)arg; return 0; }
int _rename(const char *old_path, const char *new_path)
{
    (void)old_path; (void)new_path; errno = EROFS; return -1;
}
