#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GX4000_LCD_W 240
#define GX4000_LCD_H 135
#define GX4000_CPC_VISIBLE_W 384
#define GX4000_CPC_VISIBLE_H 272

void gx4000_display_init(void);
void gx4000_display_shutdown(void);
void gx4000_display_flush_line(int vis_y, const unsigned char *data,
                               int x_offset, int render_w,
                               int bytes_per_pixel);
void gx4000_display_frame_done(void);
unsigned long gx4000_display_take_drop_count(void);
void gx4000_display_cycle_view(void);
void gx4000_display_zoom_in(void);
void gx4000_display_zoom_out(void);

#ifdef __cplusplus
}
#endif
