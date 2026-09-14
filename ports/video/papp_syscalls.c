// newlib glue for psram_video: heap, locks, clocks, logging and exit all go
// through the loader's service table (papp_svc). Same approach as the NetSurf,
// Tulip and OpenLara ports, plus aligned allocations (FFmpeg's av_malloc uses
// posix_memalign) and a heap that several tasks on both cores share.
#include "papp_port.h"

#include <errno.h>
#include <reent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// ── Locks ─────────────────────────────────────────────────────────────────
// The loader has tasks but no mutex. A lock is a spinlock whose word lives in
// internal RAM (atomic instructions are only used on internal RAM here); a
// waiter that spins for a while sleeps a tick so lower-priority tasks run.

struct papp_lock {
    volatile uint32_t word;
};

static struct papp_lock s_fallback_locks[4];
static int s_fallback_used = 0;

papp_lock_t *papp_lock_new(void)
{
    papp_lock_t *lock = NULL;
    if (papp_svc->mem_caps_alloc != NULL) {
        lock = (papp_lock_t *)papp_svc->mem_caps_alloc(sizeof(papp_lock_t), PAPP_MEM_CAP_INTERNAL);
    }
    if (lock == NULL && s_fallback_used < 4) {
        lock = &s_fallback_locks[s_fallback_used++];
    }
    if (lock != NULL) {
        lock->word = 0;
    }
    return lock;
}

void papp_lock_free(papp_lock_t *lock)
{
    if (lock != NULL && (lock < &s_fallback_locks[0] || lock >= &s_fallback_locks[4])) {
        papp_svc->mem_free(lock);
    }
}

void papp_lock_take(papp_lock_t *lock)
{
    unsigned spins = 0;
    while (__atomic_exchange_n(&lock->word, 1u, __ATOMIC_ACQUIRE) != 0) {
        if (++spins >= 2000) {
            spins = 0;
            papp_svc->delay_ms(10);
        }
    }
}

void papp_lock_give(papp_lock_t *lock)
{
    __atomic_store_n(&lock->word, 0u, __ATOMIC_RELEASE);
}

// ── Heap ──────────────────────────────────────────────────────────────────
// Every block has a header just before the payload that links it into a
// list, so everything still allocated goes back to the loader when the app
// quits. Payloads are 16-byte aligned (more for posix_memalign). All of it is
// PSRAM: plain malloc() would put small blocks into the loader's scarce
// internal RAM.

typedef struct block {
    struct block *prev;
    struct block *next;
    void *raw;        // what the loader returned
    size_t size;      // payload bytes
    uint32_t magic;   // HEAD_MAGIC while live
    uint32_t pad[3];
} block_t;

_Static_assert(sizeof(block_t) == 32, "block header must keep payloads aligned");

#define HEAD_MAGIC 0x56494446u  // "VIDF"

static block_t *s_blocks = NULL;
static papp_lock_t *s_heap_lock = NULL;
static int s_bad_frees = 0;

static void *raw_alloc(size_t size)
{
    void *p = NULL;
    if (papp_svc->mem_caps_alloc != NULL) {
        p = papp_svc->mem_caps_alloc(size, PAPP_MEM_CAP_SPIRAM);
    }
    return p != NULL ? p : papp_svc->mem_alloc(size);
}

static void *block_alloc(size_t size, size_t align)
{
    if (align < 16) {
        align = 16;
    }
    if (size > SIZE_MAX - sizeof(block_t) - align) {
        return NULL;
    }
    uint8_t *raw = (uint8_t *)raw_alloc(sizeof(block_t) + size + align);
    if (raw == NULL) {
        return NULL;
    }
    uintptr_t payload = ((uintptr_t)raw + sizeof(block_t) + align - 1) & ~(uintptr_t)(align - 1);
    block_t *b = (block_t *)payload - 1;
    b->raw = raw;
    b->size = size;
    b->magic = HEAD_MAGIC;
    b->prev = NULL;
    papp_lock_take(s_heap_lock);
    b->next = s_blocks;
    if (s_blocks != NULL) {
        s_blocks->prev = b;
    }
    s_blocks = b;
    papp_lock_give(s_heap_lock);
    return (void *)payload;
}

static void block_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    block_t *b = (block_t *)ptr - 1;
    papp_lock_take(s_heap_lock);
    if (b->magic != HEAD_MAGIC) {
        papp_lock_give(s_heap_lock);
        if (s_bad_frees++ < 8) {
            papp_svc->log_printf("VIDEO: free of a bad block %p\n", ptr);
        }
        return;
    }
    b->magic = 0;
    if (b->prev != NULL) {
        b->prev->next = b->next;
    } else {
        s_blocks = b->next;
    }
    if (b->next != NULL) {
        b->next->prev = b->prev;
    }
    papp_lock_give(s_heap_lock);
    papp_svc->mem_free(b->raw);
}

static void *block_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return block_alloc(size, 16);
    }
    if (size == 0) {
        block_free(ptr);
        return NULL;
    }
    block_t *old = (block_t *)ptr - 1;
    if (old->magic != HEAD_MAGIC) {
        return NULL;
    }
    if (size <= old->size) {
        return ptr;
    }
    void *fresh = block_alloc(size, 16);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, old->size);
    block_free(ptr);
    return fresh;
}

void *papp_alloc_aligned(size_t size, size_t align)
{
    return block_alloc(size, align);
}

void papp_free_all_memory(void)
{
    papp_lock_take(s_heap_lock);
    int count = 0;
    while (s_blocks != NULL) {
        block_t *b = s_blocks;
        s_blocks = b->next;
        b->magic = 0;
        papp_svc->mem_free(b->raw);
        count++;
    }
    papp_lock_give(s_heap_lock);
    if (count > 0) {
        papp_svc->log_printf("VIDEO: handed %d leftover block(s) back to the loader\n", count);
    }
}

void *__wrap_malloc(size_t size) { return block_alloc(size, 16); }
void __wrap_free(void *ptr) { block_free(ptr); }
void *__wrap_realloc(void *ptr, size_t size) { return block_realloc(ptr, size); }
void *__wrap_calloc(size_t n, size_t size)
{
    if (size != 0 && n > SIZE_MAX / size) {
        return NULL;
    }
    void *p = block_alloc(n * size, 16);
    if (p != NULL) {
        memset(p, 0, n * size);
    }
    return p;
}
void *__wrap__malloc_r(struct _reent *r, size_t size) { (void)r; return __wrap_malloc(size); }
void __wrap__free_r(struct _reent *r, void *ptr) { (void)r; __wrap_free(ptr); }
void *__wrap__realloc_r(struct _reent *r, void *ptr, size_t size) { (void)r; return __wrap_realloc(ptr, size); }
void *__wrap__calloc_r(struct _reent *r, size_t n, size_t size) { (void)r; return __wrap_calloc(n, size); }

// FFmpeg's av_malloc (HAVE_POSIX_MEMALIGN). Wrapped like malloc (papp.json
// ldflags), so newlib's own memalign, which would use its own heap, is never
// linked.
int __wrap_posix_memalign(void **out, size_t align, size_t size)
{
    if (align == 0 || (align & (align - 1)) != 0 || align > 4096) {
        return EINVAL;
    }
    void *p = block_alloc(size, align);
    if (p == NULL) {
        return ENOMEM;
    }
    *out = p;
    return 0;
}
void *__wrap_memalign(size_t align, size_t size) { return block_alloc(size, align); }
void *__wrap_aligned_alloc(size_t align, size_t size) { return block_alloc(size, align); }

// newlib's own locks (stdio, environment): nothing to guard, only log output
// goes through stdio.
struct __lock;
void __wrap___retarget_lock_init(struct __lock **lock) { if (lock) *lock = NULL; }
void __wrap___retarget_lock_init_recursive(struct __lock **lock) { if (lock) *lock = NULL; }
void __wrap___retarget_lock_close(struct __lock *lock) { (void)lock; }
void __wrap___retarget_lock_close_recursive(struct __lock *lock) { (void)lock; }
void __wrap___retarget_lock_acquire(struct __lock *lock) { (void)lock; }
int __wrap___retarget_lock_try_acquire(struct __lock *lock) { (void)lock; return 0; }
void __wrap___retarget_lock_acquire_recursive(struct __lock *lock) { (void)lock; }
int __wrap___retarget_lock_try_acquire_recursive(struct __lock *lock) { (void)lock; return 0; }
void __wrap___retarget_lock_release(struct __lock *lock) { (void)lock; }
void __wrap___retarget_lock_release_recursive(struct __lock *lock) { (void)lock; }

struct _reent *__getreent(void) { return _impure_ptr; }

void *_sbrk(ptrdiff_t incr) { (void)incr; errno = ENOMEM; return (void *)-1; }
void *_sbrk_r(struct _reent *r, ptrdiff_t incr) { (void)r; return _sbrk(incr); }

// ── Files ─────────────────────────────────────────────────────────────────
// FFmpeg reads through the port's AVIOContext (papp_io.c), never through
// POSIX files; only stdout and stderr exist, and go to the loader's log.

int _open(const char *path, int flags, int mode)
{
    (void)path;
    (void)flags;
    (void)mode;
    errno = ENOENT;
    return -1;
}

int _close(int fd) { (void)fd; errno = EBADF; return -1; }
ssize_t _read(int fd, void *buf, size_t count) { (void)fd; (void)buf; (void)count; errno = EBADF; return -1; }

ssize_t _write(int fd, const void *buf, size_t count)
{
    if (fd != 1 && fd != 2) {
        errno = EBADF;
        return -1;
    }
    static char line[256];
    static size_t used = 0;
    const char *s = (const char *)buf;
    for (size_t i = 0; i < count; i++) {
        if (s[i] == '\n' || used == sizeof(line) - 1) {
            line[used] = '\0';
            papp_svc->log_printf("VIDEO: %s\n", line);
            used = 0;
            if (s[i] == '\n') {
                continue;
            }
        }
        if (s[i] != '\r') {
            line[used++] = s[i];
        }
    }
    return (ssize_t)count;
}

off_t _lseek(int fd, off_t offset, int whence) { (void)fd; (void)offset; (void)whence; errno = EBADF; return -1; }

int _fstat(int fd, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    if (fd >= 0 && fd <= 2) {
        st->st_mode = S_IFCHR;
        return 0;
    }
    errno = EBADF;
    return -1;
}

int _stat(const char *path, struct stat *st) { (void)path; memset(st, 0, sizeof(*st)); errno = ENOENT; return -1; }
int _unlink(const char *path) { (void)path; errno = EACCES; return -1; }
int _link(const char *a, const char *b) { (void)a; (void)b; errno = EMLINK; return -1; }
int _rename(const char *a, const char *b) { (void)a; (void)b; errno = EACCES; return -1; }
int _isatty(int fd) { return fd >= 0 && fd <= 2; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void) { return 1; }

int _open_r(struct _reent *r, const char *path, int flags, int mode) { (void)r; return _open(path, flags, mode); }
int _close_r(struct _reent *r, int fd) { (void)r; return _close(fd); }
ssize_t _read_r(struct _reent *r, int fd, void *buf, size_t n) { (void)r; return _read(fd, buf, n); }
ssize_t _write_r(struct _reent *r, int fd, const void *buf, size_t n) { (void)r; return _write(fd, buf, n); }
off_t _lseek_r(struct _reent *r, int fd, off_t off, int whence) { (void)r; return _lseek(fd, off, whence); }
int _fstat_r(struct _reent *r, int fd, struct stat *st) { (void)r; return _fstat(fd, st); }
int _stat_r(struct _reent *r, const char *path, struct stat *st) { (void)r; return _stat(path, st); }
int _unlink_r(struct _reent *r, const char *path) { (void)r; return _unlink(path); }
int _link_r(struct _reent *r, const char *a, const char *b) { (void)r; return _link(a, b); }
int _rename_r(struct _reent *r, const char *a, const char *b) { (void)r; return _rename(a, b); }
int _isatty_r(struct _reent *r, int fd) { (void)r; return _isatty(fd); }
int _kill_r(struct _reent *r, int pid, int sig) { (void)r; return _kill(pid, sig); }
int _getpid_r(struct _reent *r) { (void)r; return 1; }

// ── Time ──────────────────────────────────────────────────────────────────
// Every clock counts from boot; the player only measures intervals.

int64_t papp_time_us(void)
{
    return papp_svc->get_time_us();
}

time_t time(time_t *out)
{
    time_t now = (time_t)(papp_time_us() / 1000000);
    if (out != NULL) {
        *out = now;
    }
    return now;
}

int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv != NULL) {
        int64_t us = papp_time_us();
        tv->tv_sec = (time_t)(us / 1000000);
        tv->tv_usec = (suseconds_t)(us % 1000000);
    }
    return 0;
}
int _gettimeofday_r(struct _reent *r, struct timeval *tv, void *tz) { (void)r; return _gettimeofday(tv, tz); }

int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    (void)clock_id;
    int64_t us = papp_time_us();
    tp->tv_sec = (time_t)(us / 1000000);
    tp->tv_nsec = (long)(us % 1000000) * 1000;
    return 0;
}

// Per task would be better; one shared stamp still makes every busy task
// sleep now and then.
static volatile int64_t s_last_sleep_us = 0;

void papp_sleep_ms(int ms)
{
    papp_svc->delay_ms(ms < 10 ? 10 : ms);  // one tick; shorter delays are only a yield
    s_last_sleep_us = papp_time_us();
}

void papp_yield_if_due(void)
{
    if (papp_time_us() - s_last_sleep_us > 200000) {
        papp_sleep_ms(10);
    }
}

int usleep(useconds_t us)
{
    papp_sleep_ms((int)(us / 1000));
    return 0;
}

unsigned sleep(unsigned seconds)
{
    papp_sleep_ms((int)seconds * 1000);
    return 0;
}

// ── Logging ───────────────────────────────────────────────────────────────

void papp_log(const char *fmt, ...)
{
    char line[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    papp_svc->log_printf("VIDEO: %s\n", line);
}

// ── Exit ──────────────────────────────────────────────────────────────────

volatile int papp_fatal_code = 0;
volatile int papp_fatal_hit = 0;

void papp_fatal(int code)
{
    if (!papp_fatal_hit) {
        papp_fatal_code = code;
        papp_fatal_hit = 1;
    }
    for (;;) {
        papp_svc->delay_ms(1000);  // app_entry stops this task
    }
}

void __wrap_exit(int code) { papp_fatal(code); }
void _exit(int code) { papp_fatal(code); }
void abort(void)
{
    papp_svc->log_printf("VIDEO: abort()\n");
    papp_fatal(-1);
}

void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    papp_svc->log_printf("VIDEO: assert '%s' failed at %s:%d (%s)\n", expr, file, line, func ? func : "");
    papp_fatal(-4);
}

void *__dso_handle = &__dso_handle;
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
int atexit(void (*fn)(void)) { (void)fn; return 0; }

void papp_syscalls_init(void)
{
    s_heap_lock = papp_lock_new();
    s_last_sleep_us = papp_time_us();
}

void papp_syscalls_deinit(void)
{
    papp_lock_free(s_heap_lock);
    s_heap_lock = NULL;
    s_fallback_used = 0;
}
