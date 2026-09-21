#pragma once
#define ODROID_SD_ERR_BADFILE 1
void odroid_display_show_sderr(int error);
/* The PAPP renders synchronously: no competing display task. */
static inline void odroid_display_lock_gb_display(void) {}
static inline void odroid_display_unlock_gb_display(void) {}
