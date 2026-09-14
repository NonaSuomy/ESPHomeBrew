// time.time() / time.localtime() for the Tulip PAPP (included by
// extmod/modtime.c through MICROPY_PY_TIME_INCLUDEFILE).
//
// The loader has no real-time clock service, so the "wall clock" starts at
// the Epoch (1970-01-01 00:00) when Tulip starts and counts uptime.
#include "py/obj.h"
#include "shared/timeutils/timeutils.h"

extern int64_t papp_time_us(void);

static mp_obj_t mp_time_localtime_get(void)
{
    timeutils_struct_time_t tm;
    timeutils_seconds_since_epoch_to_struct_time((mp_uint_t)(papp_time_us() / 1000000), &tm);
    mp_obj_t tuple[8] = {
        mp_obj_new_int(tm.tm_year),
        mp_obj_new_int(tm.tm_mon),
        mp_obj_new_int(tm.tm_mday),
        mp_obj_new_int(tm.tm_hour),
        mp_obj_new_int(tm.tm_min),
        mp_obj_new_int(tm.tm_sec),
        mp_obj_new_int(tm.tm_wday),
        mp_obj_new_int(tm.tm_yday),
    };
    return mp_obj_new_tuple(8, tuple);
}

static mp_obj_t mp_time_time_get(void)
{
    return mp_obj_new_int_from_ull((unsigned long long)(papp_time_us() / 1000000));
}
