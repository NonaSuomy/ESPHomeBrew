#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void msx_save_set_game_identity(const char *rom_name, const char *rom_path);
void msx_save_clear_game_identity(void);
int msx_save_build_path(char *dst, unsigned int dst_len, const char *ext);

#ifdef __cplusplus
}
#endif
