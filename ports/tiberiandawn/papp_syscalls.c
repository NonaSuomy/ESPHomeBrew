// newlib glue for the Tiberian Dawn PAPP: heap, files, clocks and exit all go
// through the loader's service table (papp_svc).
//
// Heap: every block carries a small header that links it into a list, so all
// memory the game still holds is returned to the loader when it quits. The
// loader does not reclaim an app's heap on its own.
#include "papp_port.h"

#include <errno.h>
#include <fcntl.h>
#include <reent.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// ── Heap ─────────────────────────────────────────────────────────────────────

typedef struct block {
    struct block *prev;
    struct block *next;
    size_t size;
    uint32_t magic;   // HEAD_MAGIC while the block is live
    void *caller;     // who allocated it (an address in the app, see .sym)
    uint32_t pad[3];  // keeps the payload 16-byte aligned like the loader's blocks
} block_t;

// Heap guards: a magic in each header and GUARD_BYTES of GUARD_FILL after each
// payload, checked on free and every few seconds (papp_heap_check). A game
// buffer overrun then shows up in the log with the block's size and owner
// instead of as a crash somewhere else later.
#define HEAD_MAGIC 0x5241484bu
#define GUARD_BYTES 16
#define GUARD_FILL 0xfd

static block_t *s_blocks = NULL;

// PSRAM only. Plain malloc() puts blocks under 16 KiB in internal RAM first,
// and the game makes thousands of small allocations: internal RAM ran out and
// the firmware aborted when it could not create a mutex (lock_init_generic
// from fclose, right after "TD: video mode").
static void *heap_alloc(size_t bytes)
{
    if (papp_svc->mem_caps_alloc != NULL) {
        return papp_svc->mem_caps_alloc(bytes, PAPP_MEM_CAP_SPIRAM);
    }
    return papp_svc->mem_alloc(bytes);
}

static int s_guard_reports = 0;

static int guard_intact(const block_t *b)
{
    const unsigned char *g = (const unsigned char *)(b + 1) + b->size;
    for (int i = 0; i < GUARD_BYTES; i++) {
        if (g[i] != GUARD_FILL) {
            return 0;
        }
    }
    return 1;
}

static void report_block(const block_t *b, const char *what)
{
    if (s_guard_reports++ < 8) {
        papp_svc->log_printf("TD: heap guard: %s: block %p, %u bytes, allocated from %p\n", what, (void *)(b + 1),
                             (unsigned)b->size, b->caller);
    }
}

static void *block_alloc(size_t size, void *caller)
{
    if (size > SIZE_MAX - sizeof(block_t) - GUARD_BYTES) {
        return NULL;
    }
    block_t *b = (block_t *)heap_alloc(sizeof(block_t) + size + GUARD_BYTES);
    if (b == NULL) {
        return NULL;
    }
    b->size = size;
    b->magic = HEAD_MAGIC;
    b->caller = caller;
    memset((unsigned char *)(b + 1) + size, GUARD_FILL, GUARD_BYTES);
    b->prev = NULL;
    b->next = s_blocks;
    if (s_blocks) {
        s_blocks->prev = b;
    }
    s_blocks = b;
    return b + 1;
}

static void block_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    block_t *b = (block_t *)ptr - 1;
    if (b->magic != HEAD_MAGIC) {
        // Not ours, freed twice, or its header was overwritten: leave it alone.
        if (s_guard_reports++ < 8) {
            papp_svc->log_printf("TD: heap guard: free of a bad block %p (header %08x)\n", ptr, (unsigned)b->magic);
        }
        return;
    }
    if (!guard_intact(b)) {
        report_block(b, "overrun found on free");
    }
    b->magic = 0;
    if (b->prev) {
        b->prev->next = b->next;
    } else {
        s_blocks = b->next;
    }
    if (b->next) {
        b->next->prev = b->prev;
    }
    papp_svc->mem_free(b);
}

static void *block_realloc(void *ptr, size_t size, void *caller)
{
    if (ptr == NULL) {
        return block_alloc(size, caller);
    }
    if (size == 0) {
        block_free(ptr);
        return NULL;
    }
    block_t *old = (block_t *)ptr - 1;
    if (size <= old->size) {
        return ptr;
    }
    void *fresh = block_alloc(size, caller);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, old->size);
    block_free(ptr);
    return fresh;
}

// Walk every live block; report the first damaged ones. Game task only (the
// list is not locked).
void papp_heap_check(void)
{
    int count = 0;
    for (block_t *b = s_blocks; b != NULL; b = b->next) {
        if (b->magic != HEAD_MAGIC) {
            if (s_guard_reports++ < 8) {
                papp_svc->log_printf("TD: heap guard: header overwritten at %p after %d blocks\n", (void *)(b + 1), count);
            }
            return; // the links are not trustworthy past this point
        }
        if (!guard_intact(b)) {
            report_block(b, "overrun");
            // Re-arm so a second overrun of the same block is reported again.
            memset((unsigned char *)(b + 1) + b->size, GUARD_FILL, GUARD_BYTES);
        }
        if (b->next != NULL && b->next->prev != b) {
            if (s_guard_reports++ < 8) {
                papp_svc->log_printf("TD: heap guard: broken link after block %p (%u bytes)\n", (void *)(b + 1),
                                     (unsigned)b->size);
            }
            return;
        }
        count++;
    }
}

void papp_free_all_memory(void)
{
    while (s_blocks) {
        block_t *b = s_blocks;
        s_blocks = b->next;
        papp_svc->mem_free(b);
    }
}

void *__wrap_malloc(size_t size) { return block_alloc(size, __builtin_return_address(0)); }
void __wrap_free(void *ptr) { block_free(ptr); }
void *__wrap_realloc(void *ptr, size_t size) { return block_realloc(ptr, size, __builtin_return_address(0)); }
void *__wrap_calloc(size_t n, size_t size)
{
    if (size != 0 && n > SIZE_MAX / size) {
        return NULL;
    }
    void *p = block_alloc(n * size, __builtin_return_address(0));
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}
void *__wrap__malloc_r(struct _reent *r, size_t size) { (void)r; return __wrap_malloc(size); }
void __wrap__free_r(struct _reent *r, void *ptr) { (void)r; __wrap_free(ptr); }
void *__wrap__realloc_r(struct _reent *r, void *ptr, size_t size) { (void)r; return __wrap_realloc(ptr, size); }
void *__wrap__calloc_r(struct _reent *r, size_t n, size_t size) { (void)r; return __wrap_calloc(n, size); }

// Single game task: newlib's locks have nothing to protect.
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

// ── Files: fds 3.. are loader file handles ─────────────────────────────────

// The game builds paths like "/sd/roms/tiberiandawn/./CONQUER.MIX" (its data folder
// is the "." CD volume). FATFS does not resolve "." components, so drop them,
// and trailing slashes, before a path reaches the loader.
static const char *clean_path(const char *path, char *out, size_t size)
{
    size_t n = 0;
    for (const char *p = path; *p != '\0' && n < size - 1; p++) {
        if (*p == '/' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            p++;
            continue;
        }
        out[n++] = *p;
    }
    while (n > 1 && out[n - 1] == '/') {
        n--;
    }
    out[n] = '\0';
    return out;
}

#define MAX_FDS 32
static void *s_files[MAX_FDS];

// Read-ahead for files opened read-only. The game reads its MIX files and
// movies ~1 KB at a time; each read went to the SD card separately, and the
// intro spent more than half its time in them (2.7 s of every 5 s), which
// made the movie stutter. One 32 KiB read now serves the small ones.
#define READ_AHEAD 32768
typedef struct {
    unsigned char *buf;  // NULL: not cached (written files)
    size_t len;          // bytes in buf
    size_t pos;          // next byte to hand out
} read_ahead_t;
static read_ahead_t s_ahead[MAX_FDS];

static void drop_read_ahead(int fd)
{
    block_free(s_ahead[fd].buf);
    s_ahead[fd].buf = NULL;
    s_ahead[fd].len = s_ahead[fd].pos = 0;
}

void papp_close_all_files(void)
{
    for (int fd = 3; fd < MAX_FDS; fd++) {
        if (s_files[fd]) {
            papp_svc->file_close(s_files[fd]);
            s_files[fd] = NULL;
        }
        drop_read_ahead(fd);
    }
}

static void *fd_file(int fd)
{
    return (fd >= 3 && fd < MAX_FDS) ? s_files[fd] : NULL;
}

static const char *open_mode(int flags)
{
    const int access = flags & O_ACCMODE;
    if (flags & O_APPEND) {
        return access == O_RDWR ? "a+b" : "ab";
    }
    if (access == O_RDONLY) {
        return "rb";
    }
    if (flags & O_TRUNC || (flags & O_CREAT && access == O_WRONLY)) {
        return access == O_RDWR ? "w+b" : "wb";
    }
    return "r+b";
}

int _open(const char *path, int flags, int mode)
{
    (void)mode;
    int fd = 3;
    while (fd < MAX_FDS && s_files[fd]) {
        fd++;
    }
    if (fd == MAX_FDS) {
        errno = EMFILE;
        return -1;
    }
    char clean[256];
    path = clean_path(path, clean, sizeof(clean));
    void *fp = papp_svc->file_open(path, open_mode(flags));
    if (fp == NULL && (flags & O_CREAT) && (flags & O_ACCMODE) == O_RDWR) {
        fp = papp_svc->file_open(path, "w+b");  // "r+b" needs the file to exist
    }
    if (fp == NULL) {
        errno = ENOENT;
        return -1;
    }
    s_files[fd] = fp;
    s_ahead[fd].len = s_ahead[fd].pos = 0;
    s_ahead[fd].buf = (flags & O_ACCMODE) == O_RDONLY ? (unsigned char *)block_alloc(READ_AHEAD, NULL) : NULL;
    return fd;
}

int _close(int fd)
{
    if (fd >= PAPP_SOCKET_FD_BASE && fd < PAPP_SOCKET_FD_BASE + PAPP_SOCKET_FD_COUNT) {
        return papp_socket_close(fd);
    }
    void *fp = fd_file(fd);
    if (fp == NULL) {
        errno = EBADF;
        return -1;
    }
    s_files[fd] = NULL;
    drop_read_ahead(fd);
    return papp_svc->file_close(fp);
}

ssize_t _read(int fd, void *buf, size_t count)
{
    void *fp = fd_file(fd);
    if (fp == NULL) {
        errno = EBADF;
        return -1;
    }
    read_ahead_t *ra = &s_ahead[fd];
    size_t done = 0;
    if (ra->buf == NULL) {
        done = papp_svc->file_read(buf, 1, count, fp);
    } else {
        while (done < count) {
            if (ra->pos < ra->len) {
                size_t n = ra->len - ra->pos;
                if (n > count - done) {
                    n = count - done;
                }
                memcpy((unsigned char *)buf + done, ra->buf + ra->pos, n);
                ra->pos += n;
                done += n;
                continue;
            }
            if (count - done >= READ_AHEAD) {
                // A big read gains nothing from the buffer: straight in.
                done += papp_svc->file_read((unsigned char *)buf + done, 1, count - done, fp);
                break;
            }
            ra->len = papp_svc->file_read(ra->buf, 1, READ_AHEAD, fp);
            ra->pos = 0;
            if (ra->len == 0) {
                break; // end of file
            }
        }
    }
    return (ssize_t)done;
}

ssize_t _write(int fd, const void *buf, size_t count)
{
    if (fd == 1 || fd == 2) {  // stdout/stderr: the loader's log
        char line[256];
        size_t done = 0;
        while (done < count) {
            size_t n = count - done < sizeof(line) - 1 ? count - done : sizeof(line) - 1;
            memcpy(line, (const char *)buf + done, n);
            line[n] = '\0';
            papp_svc->log_printf("%s", line);
            done += n;
        }
        return (ssize_t)count;
    }
    void *fp = fd_file(fd);
    if (fp == NULL) {
        errno = EBADF;
        return -1;
    }
    return (ssize_t)papp_svc->file_write(buf, 1, count, fp);
}

off_t _lseek(int fd, off_t offset, int whence)
{
    void *fp = fd_file(fd);
    if (fp == NULL) {
        errno = EBADF;
        return -1;
    }
    read_ahead_t *ra = &s_ahead[fd];
    if (ra->buf != NULL && whence != SEEK_END) {
        // The file position is past the read-ahead; the game's is at pos.
        const long buf_start = papp_svc->file_tell(fp) - (long)ra->len;
        const long target = whence == SEEK_CUR ? buf_start + (long)ra->pos + (long)offset : (long)offset;
        if (target >= buf_start && target <= buf_start + (long)ra->len) {
            ra->pos = (size_t)(target - buf_start); // still inside the buffer
            return (off_t)target;
        }
        offset = target;
        whence = SEEK_SET;
    }
    ra->len = ra->pos = 0;
    if (papp_svc->file_seek(fp, (long)offset, whence) != 0) {
        errno = EINVAL;
        return -1;
    }
    return (off_t)papp_svc->file_tell(fp);
}

static int file_size(void *fp)
{
    long here = papp_svc->file_tell(fp);
    papp_svc->file_seek(fp, 0, SEEK_END);
    long size = papp_svc->file_tell(fp);
    papp_svc->file_seek(fp, here, SEEK_SET);
    return (int)size;
}

int _fstat(int fd, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    if (fd >= 0 && fd <= 2) {
        st->st_mode = S_IFCHR;
        return 0;
    }
    void *fp = fd_file(fd);
    if (fp == NULL) {
        errno = EBADF;
        return -1;
    }
    st->st_mode = S_IFREG | 0644;
    st->st_size = file_size(fp);
    return 0;
}

// The loader has no stat() or directory listing, so a folder counts as a
// directory when one of the game's data files opens inside it. The game only
// asks about its data folders: the data folder itself (".") and the CD
// folders "allied", "soviet", ... (Change_Local_Dir). Without this every
// folder looked missing and starting a mission failed in Force_CD_Available.
static int is_data_directory(const char *dir)
{
    static const char *const probes[] = {"CONQUER.MIX", "GENERAL.MIX"};
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        char probe[300];
        snprintf(probe, sizeof(probe), "%s/%s", dir, probes[i]);
        void *fp = papp_svc->file_open(probe, "rb");
        if (fp != NULL) {
            papp_svc->file_close(fp);
            return 1;
        }
    }
    return 0;
}

int _stat(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    char clean[256];
    path = clean_path(path, clean, sizeof(clean));
    void *fp = papp_svc->file_open(path, "rb");
    if (fp == NULL) {
        if (is_data_directory(path)) {
            st->st_mode = S_IFDIR | 0755;
            return 0;
        }
        errno = ENOENT;
        return -1;
    }
    st->st_mode = S_IFREG | 0644;
    st->st_size = file_size(fp);
    papp_svc->file_close(fp);
    return 0;
}

int _unlink(const char *path) { (void)path; errno = EACCES; return -1; }
int _link(const char *a, const char *b) { (void)a; (void)b; errno = EMLINK; return -1; }
int _isatty(int fd) { return fd >= 0 && fd <= 2; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void) { return 1; }
int mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }

int _open_r(struct _reent *r, const char *path, int flags, int mode) { (void)r; return _open(path, flags, mode); }
int _close_r(struct _reent *r, int fd) { (void)r; return _close(fd); }
ssize_t _read_r(struct _reent *r, int fd, void *buf, size_t n) { (void)r; return _read(fd, buf, n); }
ssize_t _write_r(struct _reent *r, int fd, const void *buf, size_t n) { (void)r; return _write(fd, buf, n); }
off_t _lseek_r(struct _reent *r, int fd, off_t off, int whence) { (void)r; return _lseek(fd, off, whence); }
int _fstat_r(struct _reent *r, int fd, struct stat *st) { (void)r; return _fstat(fd, st); }
int _stat_r(struct _reent *r, const char *path, struct stat *st) { (void)r; return _stat(path, st); }
int _unlink_r(struct _reent *r, const char *path) { (void)r; return _unlink(path); }
int _link_r(struct _reent *r, const char *a, const char *b) { (void)r; return _link(a, b); }
int _isatty_r(struct _reent *r, int fd) { (void)r; return _isatty(fd); }
int _kill_r(struct _reent *r, int pid, int sig) { (void)r; return _kill(pid, sig); }
int _getpid_r(struct _reent *r) { (void)r; return 1; }

// ── Time ─────────────────────────────────────────────────────────────────────

int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv) {
        long long us = papp_time_us();
        tv->tv_sec = (time_t)(us / 1000000);
        tv->tv_usec = (suseconds_t)(us % 1000000);
    }
    return 0;
}
int _gettimeofday_r(struct _reent *r, struct timeval *tv, void *tz) { (void)r; return _gettimeofday(tv, tz); }

int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    (void)clock_id;  // REALTIME and MONOTONIC: both time since boot
    long long us = papp_time_us();
    tp->tv_sec = (time_t)(us / 1000000);
    tp->tv_nsec = (long)(us % 1000000) * 1000;
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    long long ms = (long long)req->tv_sec * 1000 + req->tv_nsec / 1000000;
    papp_svc->delay_ms(ms > 0 ? (int)ms : 1);  // always yield at least one tick
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

int usleep(useconds_t us)
{
    papp_svc->delay_ms(us >= 1000 ? (int)(us / 1000) : 1);
    return 0;
}

unsigned sleep(unsigned seconds)
{
    papp_svc->delay_ms((int)seconds * 1000);
    return 0;
}

// ── pthread for libstdc++/libgcc (built with POSIX threads) ────────────────
// The game runs on one task: mutexes have nothing to do and thread-specific
// data is plain global data. <pthread.h> is not included on purpose; only the
// symbol names matter to the linker.

int pthread_mutex_init(void *mutex, const void *attr) { (void)mutex; (void)attr; return 0; }
int pthread_mutex_destroy(void *mutex) { (void)mutex; return 0; }
int pthread_mutex_lock(void *mutex) { (void)mutex; return 0; }
int pthread_mutex_trylock(void *mutex) { (void)mutex; return 0; }
int pthread_mutex_unlock(void *mutex) { (void)mutex; return 0; }

#define MAX_KEYS 16
static const void *s_key_values[MAX_KEYS];
static unsigned long s_keys_used;

int pthread_key_create(unsigned long *key, void (*destructor)(void *))
{
    (void)destructor;
    if (s_keys_used >= MAX_KEYS) {
        return EAGAIN;
    }
    *key = s_keys_used++;
    return 0;
}
int pthread_key_delete(unsigned long key) { (void)key; return 0; }
void *pthread_getspecific(unsigned long key) { return key < MAX_KEYS ? (void *)s_key_values[key] : NULL; }
int pthread_setspecific(unsigned long key, const void *value)
{
    if (key >= MAX_KEYS) {
        return EINVAL;
    }
    s_key_values[key] = value;
    return 0;
}

// ── Exit and the C++ runtime ────────────────────────────────────────────────

void __wrap_exit(int code) { papp_quit(code); }
void _exit(int code) { papp_quit(code); }
void abort(void)
{
    papp_svc->log_printf("TD: abort()\n");
    papp_quit(-1);
}

void __cxa_pure_virtual(void)
{
    papp_svc->log_printf("TD: pure virtual call\n");
    papp_quit(-1);
}

// Destructors of globals are never run: the app's memory is released as a whole.
void *__dso_handle = &__dso_handle;
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
int atexit(void (*fn)(void)) { (void)fn; return 0; }
