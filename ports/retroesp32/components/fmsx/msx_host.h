#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MSX_HOST_VIEW_FIT43 = 0,
    MSX_HOST_VIEW_FULL = 1,
} msx_host_view_mode_t;

void msx_host_set_game(const uint8_t *rom_data, unsigned int rom_size,
                       const char *rom_name, const char *rom_path);
void msx_host_clear_game(void);
int msx_host_load_bios_for_mode(int mode);
void msx_host_unload_bios(void);
const uint8_t *msx_host_get_mapped_rom(const char *file_name,
                                       unsigned int *size);
const uint8_t *msx_host_get_builtin_file(const char *name,
                                         unsigned int *size);
int msx_host_is_cbios_fallback_active(void);
int msx_host_prepare_runtime(void);

#ifdef __cplusplus
}
#endif
