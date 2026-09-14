// newlib glue for the NetSurf PAPP: heap, files, clocks, locks and exit all
// go through the loader's service table (papp_svc). Same approach as the
// Tulip, Red Alert and OpenLara ports, plus what NetSurf's POSIX side needs:
// stat() that knows its resource folder is a directory, access(), a
// directory API that always fails (the loader cannot list directories),
// uname(), pread()/pwrite() and a wall clock set from HTTP Date headers.
#include "papp_port.h"

#include <dirent.h>
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
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

// ── Locks ──────────────────────────────────────────────────────────────────
// The loader offers tasks but no mutex. The heap is shared by the NetSurf
// task and the host-name lookup task, so it is guarded by a spinlock whose
// word lives in internal RAM; a waiter that spins for a while sleeps a tick.

typedef struct papp_lock {
    volatile uint32_t word;
} papp_lock_t;

static papp_lock_t s_static_locks[2];
static papp_lock_t *s_heap_lock;
static papp_lock_t *s_file_lock;

static papp_lock_t *lock_new(int index)
{
    papp_lock_t *lock = NULL;
    if (papp_svc->mem_caps_alloc != NULL) {
        lock = (papp_lock_t *)papp_svc->mem_caps_alloc(sizeof(papp_lock_t), PAPP_MEM_CAP_INTERNAL);
    }
    if (lock == NULL) {
        lock = &s_static_locks[index];
    }
    lock->word = 0;
    return lock;
}

static void lock_take(papp_lock_t *lock)
{
    unsigned spins = 0;
    while (__atomic_exchange_n(&lock->word, 1u, __ATOMIC_ACQUIRE) != 0) {
        if (++spins >= 2000) {
            spins = 0;
            papp_svc->delay_ms(1);
        }
    }
}

static void lock_give(papp_lock_t *lock)
{
    __atomic_store_n(&lock->word, 0u, __ATOMIC_RELEASE);
}

static void lock_free(papp_lock_t **lock)
{
    papp_lock_t *l = *lock;
    if (l != NULL && (l < &s_static_locks[0] || l >= &s_static_locks[2])) {
        papp_svc->mem_free(l);
    }
    *lock = NULL;
}

// ── Heap ─────────────────────────────────────────────────────────────────────
// Every block carries a small header that links it into a list, so all
// memory NetSurf still holds goes back to the loader when it quits (the
// loader does not reclaim an app's heap). Everything is PSRAM: NetSurf makes
// very many small allocations, and plain malloc() would put those under
// 16 KiB into the loader's scarce internal RAM.

typedef struct block {
    struct block *prev;
    struct block *next;
    size_t size;
    uint32_t magic;   // HEAD_MAGIC while the block is live
    uint32_t pad[4];  // keeps the payload 16-byte aligned like the loader's blocks
} block_t;

#define HEAD_MAGIC 0x4e535246u  // "NSRF"

static block_t *s_blocks = NULL;
static int s_bad_frees = 0;

void *papp_alloc_raw(size_t size, int internal)
{
    if (papp_svc->mem_caps_alloc != NULL) {
        void *p = papp_svc->mem_caps_alloc(size, internal ? (PAPP_MEM_CAP_INTERNAL | PAPP_MEM_CAP_DMA)
                                                          : PAPP_MEM_CAP_SPIRAM);
        if (p != NULL || internal) {
            return p;
        }
    }
    return internal ? NULL : papp_svc->mem_alloc(size);
}

void papp_free_raw(void *ptr)
{
    if (ptr != NULL) {
        papp_svc->mem_free(ptr);
    }
}

static void *block_alloc(size_t size)
{
    if (size > SIZE_MAX - sizeof(block_t)) {
        return NULL;
    }
    block_t *b = (block_t *)papp_alloc_raw(sizeof(block_t) + size, 0);
    if (b == NULL) {
        return NULL;
    }
    b->size = size;
    b->magic = HEAD_MAGIC;
    b->prev = NULL;
    lock_take(s_heap_lock);
    b->next = s_blocks;
    if (s_blocks) {
        s_blocks->prev = b;
    }
    s_blocks = b;
    lock_give(s_heap_lock);
    return b + 1;
}

static void block_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    block_t *b = (block_t *)ptr - 1;
    lock_take(s_heap_lock);
    if (b->magic != HEAD_MAGIC) {
        lock_give(s_heap_lock);
        // Not ours, freed twice, or its header was overwritten: leave it alone.
        if (s_bad_frees++ < 8) {
            papp_svc->log_printf("NETSURF: free of a bad block %p (header %08x)\n", ptr, (unsigned)b->magic);
        }
        return;
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
    lock_give(s_heap_lock);
    papp_svc->mem_free(b);
}

static void *block_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return block_alloc(size);
    }
    if (size == 0) {
        block_free(ptr);
        return NULL;
    }
    block_t *old = (block_t *)ptr - 1;
    if (size <= old->size) {
        return ptr;
    }
    void *fresh = block_alloc(size);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, old->size);
    block_free(ptr);
    return fresh;
}

void papp_free_all_memory(void)
{
    lock_take(s_heap_lock);
    while (s_blocks) {
        block_t *b = s_blocks;
        s_blocks = b->next;
        b->magic = 0;
        papp_svc->mem_free(b);
    }
    lock_give(s_heap_lock);
}

void *__wrap_malloc(size_t size) { return block_alloc(size); }
void __wrap_free(void *ptr) { block_free(ptr); }
void *__wrap_realloc(void *ptr, size_t size) { return block_realloc(ptr, size); }
void *__wrap_calloc(size_t n, size_t size)
{
    if (size != 0 && n > SIZE_MAX / size) {
        return NULL;
    }
    void *p = block_alloc(n * size);
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}
void *__wrap__malloc_r(struct _reent *r, size_t size) { (void)r; return __wrap_malloc(size); }
void __wrap__free_r(struct _reent *r, void *ptr) { (void)r; __wrap_free(ptr); }
void *__wrap__realloc_r(struct _reent *r, void *ptr, size_t size) { (void)r; return __wrap_realloc(ptr, size); }
void *__wrap__calloc_r(struct _reent *r, size_t n, size_t size) { (void)r; return __wrap_calloc(n, size); }

// newlib's own locks (stdio, environment) guard nothing here: only the
// NetSurf task uses stdio and files.
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

// ── Loader file handles ─────────────────────────────────────────────────────
// Every file the app opens is remembered, so it can be closed when NetSurf
// quits.

#define MAX_FILES 32
static void *s_open_files[MAX_FILES];

void *papp_file_open(const char *path, const char *mode)
{
    void *fp = papp_svc->file_open(path, mode);
    if (fp == NULL) {
        return NULL;
    }
    lock_take(s_file_lock);
    for (int i = 0; i < MAX_FILES; i++) {
        if (s_open_files[i] == NULL) {
            s_open_files[i] = fp;
            lock_give(s_file_lock);
            return fp;
        }
    }
    lock_give(s_file_lock);
    papp_svc->file_close(fp);  // table full
    return NULL;
}

int papp_file_close(void *fp)
{
    if (fp == NULL) {
        return -1;
    }
    lock_take(s_file_lock);
    for (int i = 0; i < MAX_FILES; i++) {
        if (s_open_files[i] == fp) {
            s_open_files[i] = NULL;
        }
    }
    lock_give(s_file_lock);
    return papp_svc->file_close(fp);
}

long papp_file_size(void *fp)
{
    long here = papp_svc->file_tell(fp);
    papp_svc->file_seek(fp, 0, SEEK_END);
    long size = papp_svc->file_tell(fp);
    papp_svc->file_seek(fp, here, SEEK_SET);
    return size;
}

int papp_file_exists(const char *path)
{
    void *fp = papp_svc->file_open(path, "rb");
    if (fp == NULL) {
        return 0;
    }
    papp_svc->file_close(fp);
    return 1;
}

void papp_close_all_files(void)
{
    for (int i = 0; i < MAX_FILES; i++) {
        if (s_open_files[i] != NULL) {
            papp_svc->file_close(s_open_files[i]);
            s_open_files[i] = NULL;
        }
    }
}

// ── newlib file descriptors: fds 3.. are loader files ──────────────────────

#define MAX_FDS 24
static void *s_fds[MAX_FDS];

static void *fd_file(int fd)
{
    return (fd >= 3 && fd < MAX_FDS) ? s_fds[fd] : NULL;
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
    while (fd < MAX_FDS && s_fds[fd]) {
        fd++;
    }
    if (fd == MAX_FDS) {
        errno = EMFILE;
        return -1;
    }
    void *fp = papp_file_open(path, open_mode(flags));
    if (fp == NULL && (flags & O_CREAT) && (flags & O_ACCMODE) == O_RDWR) {
        fp = papp_file_open(path, "w+b");  // "r+b" needs the file to exist
    }
    if (fp == NULL) {
        errno = ENOENT;
        return -1;
    }
    s_fds[fd] = fp;
    return fd;
}

int _close(int fd)
{
    void *fp = fd_file(fd);
    if (fp == NULL) {
        errno = EBADF;
        return -1;
    }
    s_fds[fd] = NULL;
    return papp_file_close(fp);
}

ssize_t _read(int fd, void *buf, size_t count)
{
    void *fp = fd_file(fd);
    if (fp == NULL) {
        errno = EBADF;
        return -1;
    }
    return (ssize_t)papp_svc->file_read(buf, 1, count, fp);
}

// stdout/stderr (NetSurf's log): the loader's log, one line per call.
ssize_t _write(int fd, const void *buf, size_t count)
{
    if (fd == 1 || fd == 2) {
        static char line[256];
        static size_t used = 0;
        const char *s = (const char *)buf;
        lock_take(s_file_lock);
        for (size_t i = 0; i < count; i++) {
            if (s[i] == '\n' || used == sizeof(line) - 1) {
                line[used] = '\0';
                papp_svc->log_printf("NS: %s\n", line);
                used = 0;
                if (s[i] == '\n') {
                    continue;
                }
            }
            if (s[i] != '\r') {
                line[used++] = s[i];
            }
        }
        lock_give(s_file_lock);
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
    if (papp_svc->file_seek(fp, (long)offset, whence) != 0) {
        errno = EINVAL;
        return -1;
    }
    return (off_t)papp_svc->file_tell(fp);
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    if (_lseek(fd, offset, SEEK_SET) < 0) {
        return -1;
    }
    return _read(fd, buf, count);
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    if (_lseek(fd, offset, SEEK_SET) < 0) {
        return -1;
    }
    return _write(fd, buf, count);
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
    st->st_size = papp_file_size(fp);
    return 0;
}

// The loader has no stat(): a path is a file when it opens. It cannot tell a
// directory either, so the folders NetSurf looks for its resources in (and
// any path written with a trailing '/') count as directories.
static int is_known_dir(const char *path)
{
    static const char *const dirs[] = {"/", "/sd", "/sd/roms", PAPP_NS_DIR};
    size_t len = strlen(path);
    if (len > 0 && path[len - 1] == '/') {
        return 1;
    }
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (strcmp(path, dirs[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

int _stat(const char *path, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    void *fp = papp_svc->file_open(path, "rb");
    if (fp != NULL) {
        st->st_mode = S_IFREG | 0644;
        st->st_size = papp_file_size(fp);
        papp_svc->file_close(fp);
        return 0;
    }
    if (is_known_dir(path)) {
        st->st_mode = S_IFDIR | 0755;
        return 0;
    }
    errno = ENOENT;
    return -1;
}

int stat(const char *path, struct stat *st) { return _stat(path, st); }
int lstat(const char *path, struct stat *st) { return _stat(path, st); }
int fstat(int fd, struct stat *st) { return _fstat(fd, st); }

int access(const char *path, int mode)
{
    struct stat st;
    if (_stat(path, &st) != 0) {
        return -1;
    }
    if ((mode & W_OK) && S_ISDIR(st.st_mode)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int mkdir(const char *path, mode_t mode)
{
    (void)mode;
    if (is_known_dir(path)) {
        errno = EEXIST;
        return -1;
    }
    errno = ENOSYS;
    return -1;
}

int rmdir(const char *path) { (void)path; errno = ENOSYS; return -1; }
char *getcwd(char *buf, size_t size)
{
    if (buf == NULL || size < 2) {
        errno = ERANGE;
        return NULL;
    }
    strcpy(buf, "/");
    return buf;
}

// No directory listing in the loader.
DIR *opendir(const char *name) { (void)name; errno = ENOSYS; return NULL; }
struct dirent *readdir(DIR *dir) { (void)dir; errno = EBADF; return NULL; }
int closedir(DIR *dir) { (void)dir; return 0; }
void rewinddir(DIR *dir) { (void)dir; }

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

// Sockets are the loader's (net_* services, papp_http.c), not POSIX ones.
int socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    errno = ENOSYS;
    return -1;
}

int uname(struct utsname *buf)
{
    memset(buf, 0, sizeof(*buf));
    strcpy(buf->sysname, "ESP32-P4");
    strcpy(buf->nodename, "papp");
    strcpy(buf->release, "PAPP");
    strcpy(buf->version, "1");
    strcpy(buf->machine, "riscv32");
    return 0;
}

// ── Time ─────────────────────────────────────────────────────────────────────
// gettimeofday()/clock_gettime() count from boot: NetSurf's scheduler uses
// them and must not see the clock jump. time() is the wall clock once an HTTP
// Date header has set it (cookies and cache expiry use it).

static int64_t s_wall_offset_s = 0;  // wall clock - uptime, in seconds

int64_t papp_time_us(void)
{
    return papp_svc->get_time_us();
}

void papp_set_wallclock(time_t now)
{
    if (s_wall_offset_s == 0 && now > 1600000000) {
        s_wall_offset_s = (int64_t)now - papp_time_us() / 1000000;
        papp_svc->log_printf("NETSURF: wall clock set from an HTTP Date header\n");
    }
}

time_t time(time_t *out)
{
    time_t now = (time_t)(papp_time_us() / 1000000 + s_wall_offset_s);
    if (out) {
        *out = now;
    }
    return now;
}

int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv) {
        int64_t us = papp_time_us();
        tv->tv_sec = (time_t)(us / 1000000);
        tv->tv_usec = (suseconds_t)(us % 1000000);
    }
    return 0;
}
int _gettimeofday_r(struct _reent *r, struct timeval *tv, void *tz) { (void)r; return _gettimeofday(tv, tz); }

int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    (void)clock_id;  // REALTIME and MONOTONIC: both time since boot
    int64_t us = papp_time_us();
    tp->tv_sec = (time_t)(us / 1000000);
    tp->tv_nsec = (long)(us % 1000000) * 1000;
    return 0;
}

static int64_t s_last_sleep_us = 0;

void papp_sleep_ms(int ms)
{
    papp_svc->delay_ms(ms < 10 ? 10 : ms);  // one tick (100 Hz); shorter delays are only a yield
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
    papp_svc->delay_ms(us >= 1000 ? (int)(us / 1000) : 1);
    s_last_sleep_us = papp_time_us();
    return 0;
}

unsigned sleep(unsigned seconds)
{
    papp_svc->delay_ms((int)seconds * 1000);
    s_last_sleep_us = papp_time_us();
    return 0;
}

// ── Exit ─────────────────────────────────────────────────────────────────────

void __wrap_exit(int code) { papp_quit(code); }
void _exit(int code) { papp_quit(code); }
void abort(void)
{
    papp_svc->log_printf("NETSURF: abort()\n");
    papp_quit(-1);
}

void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    papp_svc->log_printf("NETSURF: assert '%s' failed at %s:%d (%s)\n", expr, file, line, func ? func : "");
    papp_quit(-4);
}

void *__dso_handle = &__dso_handle;
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
int atexit(void (*fn)(void)) { (void)fn; return 0; }

// Called first by app_entry.
void papp_syscalls_init(void)
{
    s_heap_lock = lock_new(0);
    s_file_lock = lock_new(1);
    s_last_sleep_us = papp_time_us();
}

// Called last by app_entry, once no other task of the app runs.
void papp_syscalls_deinit(void)
{
    lock_free(&s_heap_lock);
    lock_free(&s_file_lock);
}
