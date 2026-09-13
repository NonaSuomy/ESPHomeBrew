// MicroPython configuration for Tulip on the ESP32-P4 PAPP loader.
//
// A bare-metal style port: no OS underneath, only the loader's service table
// (papp_svc). Modelled on Tulip's esp32s3 port (tulip/esp32s3/mpconfigport.h)
// minus the hardware drivers, networking, threads and native code.
#pragma once

#include <stdint.h>
#include <alloca.h>

// Tulip builds its C modules with STATIC, which newer MicroPython dropped.
#ifndef STATIC
#define STATIC static
#endif

#define MICROPY_CONFIG_ROM_LEVEL            (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)
#define MICROPY_OBJ_REPR                    (MICROPY_OBJ_REPR_A)

// setjmp-based NLR and GC register scan: newlib's RISC-V setjmp also saves
// the callee-saved float registers (fs0-fs11) of the ilp32f ABI.
#define MICROPY_NLR_SETJMP                  (1)
#define MICROPY_GCREGS_SETJMP               (1)

#define MICROPY_ALLOC_PATH_MAX              (256)
#define MICROPY_ENABLE_GC                   (1)
#define MICROPY_ENABLE_FINALISER            (1)
#define MICROPY_STACK_CHECK                 (1)
#define MICROPY_STACK_CHECK_MARGIN          (2048)
#define MICROPY_LONGINT_IMPL                (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_ERROR_REPORTING             (MICROPY_ERROR_REPORTING_NORMAL)
#define MICROPY_WARNINGS                    (1)
#define MICROPY_FLOAT_IMPL                  (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_USE_INTERNAL_ERRNO          (0)
#define MICROPY_USE_INTERNAL_PRINTF         (0)
#define MICROPY_ENABLE_SCHEDULER            (1)
#define MICROPY_SCHEDULER_DEPTH             (128)
#define MICROPY_PERSISTENT_CODE_LOAD        (1)
#define MICROPY_EMIT_RV32                   (0)  // no executable heap on the loader
#define MICROPY_OPT_COMPUTED_GOTO           (1)
#define MICROPY_READER_VFS                  (1)
#define MICROPY_VFS                         (1)
#define MICROPY_VFS_LFS2                    (1)
#define FFCONF_H                            "lib/oofatfs/ffconf.h"  // extmod/modvfs.c includes vfs_fat.h
#define MICROPY_HELPER_REPL                 (1)
#define MICROPY_REPL_AUTO_INDENT            (1)
#define MICROPY_REPL_EMACS_KEYS             (1)
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF (1)
#define MICROPY_EMERGENCY_EXCEPTION_BUF_SIZE   (256)
#define MICROPY_PY_STR_BYTES_CMP_WARN       (1)
#define MICROPY_PY_ALL_INPLACE_SPECIAL_METHODS (1)
#define MICROPY_PY_BUILTINS_HELP_TEXT       tulip_desktop_help_text
#define MICROPY_PY_BUILTINS_INPUT           (1)
#define MICROPY_PY_IO                       (1)
#define MICROPY_PY_IO_BUFFEREDWRITER        (1)
#define MICROPY_PY_SYS_PLATFORM             "papp"
#define MICROPY_PY_SYS_STDFILES             (1)
#define MICROPY_PY_SYS_STDIO_BUFFER         (1)
#define MICROPY_PY_SYS_EXIT                 (1)
#define MICROPY_PY_SYS_ATEXIT               (1)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_TIME_TIME_NS        (1)
#define MICROPY_PY_TIME_INCLUDEFILE         "papp_modtime.c"
#define MICROPY_EPOCH_IS_1970               (1)
#define MICROPY_PY_OS_UNAME                 (0)
#define MICROPY_PY_OS_URANDOM               (0)
#define MICROPY_PY_OS_SYNC                  (0)
#define MICROPY_PY_OS_DUPTERM               (0)
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC    (papp_random_seed())
#define MICROPY_PY_HASHLIB                  (1)
#define MICROPY_PY_HASHLIB_MD5              (0)
#define MICROPY_PY_HASHLIB_SHA1             (0)
#define MICROPY_PY_HASHLIB_SHA256           (1)
#define MICROPY_PY_CRYPTOLIB                (0)
#define MICROPY_PY_DEFLATE                  (1)
#define MICROPY_PY_FRAMEBUF                 (1)
#define MICROPY_PY_ASYNCIO                  (1)
#define MICROPY_PY_SELECT                   (1)
#define MICROPY_PY_MACHINE                  (0)
#define MICROPY_PY_NETWORK                  (0)
#define MICROPY_PY_SOCKET                   (0)
#define MICROPY_PY_SSL                      (0)
#define MICROPY_PY_BLUETOOTH                (0)
#define MICROPY_PY_THREAD                   (0)
#define MICROPY_PY_BTREE                    (0)
#define MICROPY_PY_ONEWIRE                  (0)
#define MICROPY_PY_WEBREPL                  (0)
#define MICROPY_PY_LWIP                     (0)

// Frozen Python (Tulip's tulip/shared/py, AMY's python package, asyncio,
// this port's _boot.py): ports/tulip/gen_tulip.py writes frozen_content.c.
#define MICROPY_MODULE_FROZEN_MPY           (1)
#define MICROPY_MODULE_FROZEN_STR           (1)
#define MICROPY_QSTR_EXTRA_POOL             mp_qstr_frozen_const_pool

#define MICROPY_HW_BOARD_NAME               "Tulip4 PAPP"
#define MICROPY_HW_MCU_NAME                 "ESP32-P4"

#define MP_STATE_PORT                       MP_STATE_VM
#define MICROPY_MAKE_POINTER_CALLABLE(p)    ((void *)((mp_uint_t)(p)))

// The display and audio tasks schedule callbacks (frame, touch, keys,
// sequencer) into the MicroPython task: the scheduler queue is shared.
unsigned papp_atomic_begin(void);
void papp_atomic_end(unsigned state);
#define MICROPY_BEGIN_ATOMIC_SECTION()      papp_atomic_begin()
#define MICROPY_END_ATOMIC_SECTION(state)   papp_atomic_end(state)

// Waiting for input or sleeping: give the other tasks the CPU. The poll also
// lets the quit request park the MicroPython task at a safe point.
void papp_mp_poll(void);
void papp_mp_wait(void);
#define MICROPY_INTERNAL_WFE(TIMEOUT_MS)    papp_mp_wait()
#define MICROPY_EVENT_POLL_HOOK \
    do { \
        extern void mp_handle_pending(bool); \
        mp_handle_pending(true); \
        papp_mp_wait(); \
    } while (0);

#define MICROPY_VM_HOOK_COUNT               (256)
#define MICROPY_VM_HOOK_INIT                static unsigned vm_hook_divisor = MICROPY_VM_HOOK_COUNT;
#define MICROPY_VM_HOOK_POLL                if (--vm_hook_divisor == 0) { \
        vm_hook_divisor = MICROPY_VM_HOOK_COUNT; \
        papp_mp_poll(); \
}
#define MICROPY_VM_HOOK_LOOP                MICROPY_VM_HOOK_POLL
#define MICROPY_VM_HOOK_RETURN              MICROPY_VM_HOOK_POLL

uint32_t papp_random_seed(void);
extern const char tulip_desktop_help_text[];  // tulip/shared/help.c

#define MP_SSIZE_MAX (0x7fffffff)  // newlib's limits.h has no SSIZE_MAX here

typedef int32_t mp_int_t;   // must be pointer size
typedef uint32_t mp_uint_t; // must be pointer size
typedef long mp_off_t;
