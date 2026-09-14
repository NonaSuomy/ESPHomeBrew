// The _papp module: files on the SD card and the Tulip filesystem image.
//
// The loader's file services are open/read/write/seek/tell/close on a path:
// there is no directory listing, stat, mkdir or delete. So:
//   _papp.VfsSd()          mounts the card at /sd for reading and writing
//                          files by name (open, import, stat of files);
//                          listing, mkdir and delete raise OSError.
//   _papp.BlockDev(path, block_size, block_count)
//                          a block device on one file of the card, which
//                          _boot.py formats and mounts with littlefs as
//                          Tulip's own filesystem (/user, /sys, ...): a real
//                          filesystem, like Tulip's flash partitions.
//   _papp.quit()           leave Tulip (back to the loader's menu).
//   _papp.sys_tar(), _papp.sys_version()
//                          Tulip's /sys files built into the binary.
#include "papp_port.h"

#include <stdio.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/objarray.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "extmod/vfs.h"

// ── Files ───────────────────────────────────────────────────────────────────

typedef struct {
    mp_obj_base_t base;
    void *fp;
} sd_file_obj_t;

static void sd_file_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    mp_printf(print, "<io.%s>", mp_obj_get_type_str(self_in));
}

static mp_uint_t sd_file_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode)
{
    sd_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fp == NULL) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)papp_svc->file_read(buf, 1, size, self->fp);
}

static mp_uint_t sd_file_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode)
{
    sd_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fp == NULL) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    size_t n = papp_svc->file_write(buf, 1, size, self->fp);
    if (n == 0 && size != 0) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)n;
}

static mp_uint_t sd_file_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode)
{
    sd_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (request == MP_STREAM_CLOSE) {
        if (self->fp != NULL) {
            papp_file_close(self->fp);
            self->fp = NULL;
        }
        return 0;
    }
    if (self->fp == NULL) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    switch (request) {
        case MP_STREAM_SEEK: {
            struct mp_stream_seek_t *s = (struct mp_stream_seek_t *)arg;
            if (papp_svc->file_seek(self->fp, (long)s->offset, s->whence) != 0) {
                *errcode = MP_EINVAL;
                return MP_STREAM_ERROR;
            }
            s->offset = papp_svc->file_tell(self->fp);
            return 0;
        }
        case MP_STREAM_FLUSH:
            // A seek makes the loader's stdio write out its buffer.
            papp_svc->file_seek(self->fp, 0, SEEK_CUR);
            return 0;
        case MP_STREAM_POLL:
            return arg & (MP_STREAM_POLL_RD | MP_STREAM_POLL_WR);
        default:
            *errcode = MP_EINVAL;
            return MP_STREAM_ERROR;
    }
}

static const mp_rom_map_elem_t sd_file_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(sd_file_locals_dict, sd_file_locals_dict_table);

static const mp_stream_p_t sd_fileio_stream_p = {
    .read = sd_file_read,
    .write = sd_file_write,
    .ioctl = sd_file_ioctl,
};

static const mp_stream_p_t sd_textio_stream_p = {
    .read = sd_file_read,
    .write = sd_file_write,
    .ioctl = sd_file_ioctl,
    .is_text = true,
};

static MP_DEFINE_CONST_OBJ_TYPE(
    sd_type_fileio,
    MP_QSTR_FileIO,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, sd_file_print,
    protocol, &sd_fileio_stream_p,
    locals_dict, &sd_file_locals_dict
    );

static MP_DEFINE_CONST_OBJ_TYPE(
    sd_type_textio,
    MP_QSTR_TextIOWrapper,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, sd_file_print,
    protocol, &sd_textio_stream_p,
    locals_dict, &sd_file_locals_dict
    );

// ── The /sd VFS ─────────────────────────────────────────────────────────────

typedef struct {
    mp_obj_base_t base;
    char root[32];     // "/sd": what the loader calls the card
    char cwd[MICROPY_ALLOC_PATH_MAX];
} vfs_sd_obj_t;

// Full loader path for a path inside the mount ("/roms/x.py" or relative).
static const char *sd_path(vfs_sd_obj_t *self, mp_obj_t path_in, char *out, size_t out_len)
{
    const char *path = mp_obj_str_get_str(path_in);
    int n;
    if (path[0] == '/') {
        n = snprintf(out, out_len, "%s%s", self->root, path);
    } else {
        n = snprintf(out, out_len, "%s%s%s%s", self->root, self->cwd, self->cwd[1] ? "/" : "", path);
    }
    if (n < 0 || (size_t)n >= out_len) {
        mp_raise_OSError(MP_EINVAL);
    }
    return out;
}

static mp_obj_t vfs_sd_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    vfs_sd_obj_t *self = mp_obj_malloc(vfs_sd_obj_t, type);
    const char *root = n_args > 0 ? mp_obj_str_get_str(args[0]) : "/sd";
    if (strlen(root) >= sizeof(self->root)) {
        mp_raise_ValueError(MP_ERROR_TEXT("root too long"));
    }
    strcpy(self->root, root);
    strcpy(self->cwd, "/");
    return MP_OBJ_FROM_PTR(self);
}

static void *sd_try_open(const char *path)
{
    return papp_svc->file_open(path, "rb");
}

static mp_import_stat_t vfs_sd_import_stat(void *self_in, const char *path)
{
    vfs_sd_obj_t *self = self_in;
    char full[MICROPY_ALLOC_PATH_MAX + 32];
    snprintf(full, sizeof(full), "%s%s%s", self->root, path[0] == '/' ? "" : "/", path);
    void *fp = sd_try_open(full);
    if (fp == NULL) {
        return MP_IMPORT_STAT_NO_EXIST;  // directories cannot be told apart from nothing
    }
    papp_svc->file_close(fp);
    return MP_IMPORT_STAT_FILE;
}

static mp_obj_t vfs_sd_mount(mp_obj_t self_in, mp_obj_t readonly, mp_obj_t mkfs)
{
    (void)self_in;
    (void)readonly;
    (void)mkfs;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(vfs_sd_mount_obj, vfs_sd_mount);

static mp_obj_t vfs_sd_umount(mp_obj_t self_in)
{
    (void)self_in;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vfs_sd_umount_obj, vfs_sd_umount);

static mp_obj_t vfs_sd_open(mp_obj_t self_in, mp_obj_t path_in, mp_obj_t mode_in)
{
    vfs_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *mode_str = mp_obj_str_get_str(mode_in);
    const mp_obj_type_t *type = &sd_type_textio;
    char mode[4] = {'r', 0, 0, 0};
    bool plus = false;
    for (const char *m = mode_str; *m; m++) {
        switch (*m) {
            case 'r':
            case 'w':
            case 'a':
                mode[0] = *m;
                break;
            case 'x':
                mode[0] = 'w';
                break;
            case '+':
                plus = true;
                break;
            case 'b':
                type = &sd_type_fileio;
                break;
        }
    }
    // Always binary for the loader: Tulip does its own newline handling.
    mode[1] = plus ? '+' : 'b';
    mode[2] = plus ? 'b' : 0;
    char full[MICROPY_ALLOC_PATH_MAX + 32];
    sd_path(self, path_in, full, sizeof(full));
    void *fp = papp_file_open(full, mode);
    if (fp == NULL) {
        mp_raise_OSError(MP_ENOENT);
    }
    sd_file_obj_t *o = mp_obj_malloc_with_finaliser(sd_file_obj_t, type);
    o->fp = fp;
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_3(vfs_sd_open_obj, vfs_sd_open);

static mp_obj_t vfs_sd_stat(mp_obj_t self_in, mp_obj_t path_in)
{
    vfs_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    char full[MICROPY_ALLOC_PATH_MAX + 32];
    sd_path(self, path_in, full, sizeof(full));
    void *fp = sd_try_open(full);
    if (fp == NULL) {
        mp_raise_OSError(MP_ENOENT);
    }
    const long size = papp_file_size(fp);
    papp_svc->file_close(fp);
    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(10, NULL));
    t->items[0] = MP_OBJ_NEW_SMALL_INT(MP_S_IFREG);
    for (int i = 1; i < 10; i++) {
        t->items[i] = MP_OBJ_NEW_SMALL_INT(0);
    }
    t->items[6] = mp_obj_new_int_from_uint((mp_uint_t)size);
    return MP_OBJ_FROM_PTR(t);
}
static MP_DEFINE_CONST_FUN_OBJ_2(vfs_sd_stat_obj, vfs_sd_stat);

static mp_obj_t vfs_sd_chdir(mp_obj_t self_in, mp_obj_t path_in)
{
    vfs_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *path = mp_obj_str_get_str(path_in);
    char next[MICROPY_ALLOC_PATH_MAX];
    int n = path[0] == '/' ? snprintf(next, sizeof(next), "%s", path)
                           : snprintf(next, sizeof(next), "%s%s%s", self->cwd, self->cwd[1] ? "/" : "", path);
    if (n < 0 || (size_t)n >= sizeof(next)) {
        mp_raise_OSError(MP_EINVAL);
    }
    while (n > 1 && next[n - 1] == '/') {
        next[--n] = '\0';
    }
    strcpy(self->cwd, next);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vfs_sd_chdir_obj, vfs_sd_chdir);

static mp_obj_t vfs_sd_getcwd(mp_obj_t self_in)
{
    vfs_sd_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_str(self->cwd, strlen(self->cwd));
}
static MP_DEFINE_CONST_FUN_OBJ_1(vfs_sd_getcwd_obj, vfs_sd_getcwd);

// What the loader cannot do: list folders, make or delete things.
static mp_obj_t vfs_sd_unsupported(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    (void)args;
    mp_raise_OSError(MP_EOPNOTSUPP);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(vfs_sd_unsupported_obj, 1, 3, vfs_sd_unsupported);

static mp_obj_t vfs_sd_statvfs(mp_obj_t self_in, mp_obj_t path_in)
{
    (void)self_in;
    (void)path_in;
    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(10, NULL));
    for (int i = 0; i < 10; i++) {
        t->items[i] = MP_OBJ_NEW_SMALL_INT(0);
    }
    t->items[0] = MP_OBJ_NEW_SMALL_INT(512);
    t->items[9] = MP_OBJ_NEW_SMALL_INT(255);
    return MP_OBJ_FROM_PTR(t);
}
static MP_DEFINE_CONST_FUN_OBJ_2(vfs_sd_statvfs_obj, vfs_sd_statvfs);

static const mp_rom_map_elem_t vfs_sd_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_mount), MP_ROM_PTR(&vfs_sd_mount_obj) },
    { MP_ROM_QSTR(MP_QSTR_umount), MP_ROM_PTR(&vfs_sd_umount_obj) },
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&vfs_sd_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&vfs_sd_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_statvfs), MP_ROM_PTR(&vfs_sd_statvfs_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&vfs_sd_chdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&vfs_sd_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_ilistdir), MP_ROM_PTR(&vfs_sd_unsupported_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&vfs_sd_unsupported_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&vfs_sd_unsupported_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&vfs_sd_unsupported_obj) },
    { MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&vfs_sd_unsupported_obj) },
};
static MP_DEFINE_CONST_DICT(vfs_sd_locals_dict, vfs_sd_locals_dict_table);

static const mp_vfs_proto_t vfs_sd_proto = {
    .import_stat = vfs_sd_import_stat,
};

static MP_DEFINE_CONST_OBJ_TYPE(
    vfs_sd_type,
    MP_QSTR_VfsSd,
    MP_TYPE_FLAG_NONE,
    make_new, vfs_sd_make_new,
    protocol, &vfs_sd_proto,
    locals_dict, &vfs_sd_locals_dict
    );

// ── Block device on a card file ─────────────────────────────────────────────

typedef struct {
    mp_obj_base_t base;
    void *fp;
    uint32_t block_size;
    uint32_t block_count;
    char path[128];
} sd_bdev_obj_t;

static mp_obj_t sd_bdev_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    mp_arg_check_num(n_args, n_kw, 3, 3, false);
    const char *path = mp_obj_str_get_str(args[0]);
    if (strlen(path) >= sizeof(((sd_bdev_obj_t *)0)->path)) {
        mp_raise_ValueError(MP_ERROR_TEXT("path too long"));
    }
    const uint32_t block_size = (uint32_t)mp_obj_get_int(args[1]);
    const uint32_t block_count = (uint32_t)mp_obj_get_int(args[2]);
    if (block_size < 128 || (block_size & (block_size - 1)) != 0 || block_count < 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad block geometry"));
    }
    void *fp = papp_file_open(path, "r+b");
    if (fp == NULL) {
        fp = papp_file_open(path, "w+b");  // first run: create the image
    }
    if (fp == NULL) {
        mp_raise_OSError(MP_ENOENT);
    }
    const long want = (long)block_size * (long)block_count;
    if (papp_file_size(fp) < want) {
        // Grow the file to its full size once, so littlefs never extends it:
        // a write at the last byte (FAT allocates the clusters in between),
        // or zeros all the way if the card's filesystem will not do that.
        mp_printf(&mp_plat_print, "Creating the Tulip filesystem image %s (%u KiB)...\n", path,
                  (unsigned)(want / 1024));
        static const uint8_t zero[512];
        if (papp_svc->file_seek(fp, want - 1, SEEK_SET) == 0) {
            papp_svc->file_write(zero, 1, 1, fp);
        }
        long size = papp_file_size(fp);
        papp_svc->file_seek(fp, size, SEEK_SET);
        unsigned chunks = 0;
        while (size < want) {
            long n = want - size < (long)sizeof(zero) ? want - size : (long)sizeof(zero);
            if (papp_svc->file_write(zero, 1, (size_t)n, fp) != (size_t)n) {
                papp_file_close(fp);
                mp_raise_OSError(MP_ENOSPC);
            }
            size += n;
            if (++chunks % 1024 == 0) {
                papp_mp_poll();  // seconds for 16 MiB: let the idle task in
            }
        }
        papp_svc->file_seek(fp, 0, SEEK_SET);
    }
    sd_bdev_obj_t *self = mp_obj_malloc_with_finaliser(sd_bdev_obj_t, type);
    self->fp = fp;
    self->block_size = block_size;
    self->block_count = block_count;
    strcpy(self->path, path);
    return MP_OBJ_FROM_PTR(self);
}

static void sd_bdev_rw(size_t n_args, const mp_obj_t *args, bool write)
{
    sd_bdev_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    const uint32_t block = (uint32_t)mp_obj_get_int(args[1]);
    const uint32_t offset = n_args > 3 ? (uint32_t)mp_obj_get_int(args[3]) : 0;
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[2], &buf, write ? MP_BUFFER_READ : MP_BUFFER_WRITE);
    if (self->fp == NULL || (uint64_t)block * self->block_size + offset + buf.len >
        (uint64_t)self->block_count * self->block_size) {
        mp_raise_OSError(MP_EIO);
    }
    if (papp_svc->file_seek(self->fp, (long)(block * self->block_size + offset), SEEK_SET) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    if (write) {
        if (papp_svc->file_write(buf.buf, 1, buf.len, self->fp) != buf.len) {
            mp_raise_OSError(MP_EIO);
        }
    } else {
        size_t got = papp_svc->file_read(buf.buf, 1, buf.len, self->fp);
        if (got < buf.len) {
            memset((uint8_t *)buf.buf + got, 0xff, buf.len - got);
        }
    }
}

static mp_obj_t sd_bdev_readblocks(size_t n_args, const mp_obj_t *args)
{
    sd_bdev_rw(n_args, args, false);
    return MP_OBJ_NEW_SMALL_INT(0);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sd_bdev_readblocks_obj, 3, 4, sd_bdev_readblocks);

static mp_obj_t sd_bdev_writeblocks(size_t n_args, const mp_obj_t *args)
{
    sd_bdev_rw(n_args, args, true);
    return MP_OBJ_NEW_SMALL_INT(0);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sd_bdev_writeblocks_obj, 3, 4, sd_bdev_writeblocks);

static mp_obj_t sd_bdev_ioctl(mp_obj_t self_in, mp_obj_t op_in, mp_obj_t arg_in)
{
    (void)arg_in;
    sd_bdev_obj_t *self = MP_OBJ_TO_PTR(self_in);
    switch (mp_obj_get_int(op_in)) {
        case MP_BLOCKDEV_IOCTL_INIT:
            return MP_OBJ_NEW_SMALL_INT(0);
        case MP_BLOCKDEV_IOCTL_DEINIT:
        case MP_BLOCKDEV_IOCTL_SYNC:
            // The loader has no fsync: closing the file is the only way to get
            // its stdio and FAT buffers onto the card, so littlefs's commits
            // survive a power cut. Then open it again.
            if (self->fp != NULL) {
                papp_file_close(self->fp);
                self->fp = papp_file_open(self->path, "r+b");
                if (self->fp == NULL) {
                    return MP_OBJ_NEW_SMALL_INT(-MP_EIO);
                }
            }
            return MP_OBJ_NEW_SMALL_INT(0);
        case MP_BLOCKDEV_IOCTL_BLOCK_COUNT:
            return MP_OBJ_NEW_SMALL_INT(self->block_count);
        case MP_BLOCKDEV_IOCTL_BLOCK_SIZE:
            return MP_OBJ_NEW_SMALL_INT(self->block_size);
        case MP_BLOCKDEV_IOCTL_BLOCK_ERASE:
            return MP_OBJ_NEW_SMALL_INT(0);  // a file needs no erase
        default:
            return mp_const_none;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_3(sd_bdev_ioctl_obj, sd_bdev_ioctl);

static mp_obj_t sd_bdev_close(mp_obj_t self_in)
{
    sd_bdev_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fp != NULL) {
        papp_file_close(self->fp);
        self->fp = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sd_bdev_close_obj, sd_bdev_close);

static const mp_rom_map_elem_t sd_bdev_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_readblocks), MP_ROM_PTR(&sd_bdev_readblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeblocks), MP_ROM_PTR(&sd_bdev_writeblocks_obj) },
    { MP_ROM_QSTR(MP_QSTR_ioctl), MP_ROM_PTR(&sd_bdev_ioctl_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&sd_bdev_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&sd_bdev_close_obj) },
};
static MP_DEFINE_CONST_DICT(sd_bdev_locals_dict, sd_bdev_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    sd_bdev_type,
    MP_QSTR_BlockDev,
    MP_TYPE_FLAG_NONE,
    make_new, sd_bdev_make_new,
    locals_dict, &sd_bdev_locals_dict
    );

// ── Module ──────────────────────────────────────────────────────────────────

static mp_obj_t papp_mod_quit(void)
{
    papp_request_quit(0);
    papp_mp_poll();  // parks the MicroPython task until app_entry deletes it
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(papp_mod_quit_obj, papp_mod_quit);

// Tulip's /sys files as a tar built into the binary (.papp-gen/sys_tar.c,
// from tulip/fs/tulip); _boot.py unpacks it into /sys.
extern const unsigned char papp_sys_tar[];
extern const size_t papp_sys_tar_len;
extern const char papp_sys_version[];

static mp_obj_t papp_mod_sys_tar(void)
{
    return mp_obj_new_memoryview('B', papp_sys_tar_len, (void *)papp_sys_tar);  // read-only, no copy
}
static MP_DEFINE_CONST_FUN_OBJ_0(papp_mod_sys_tar_obj, papp_mod_sys_tar);

static mp_obj_t papp_mod_sys_version(void)
{
    return mp_obj_new_str(papp_sys_version, strlen(papp_sys_version));
}
static MP_DEFINE_CONST_FUN_OBJ_0(papp_mod_sys_version_obj, papp_mod_sys_version);

static const mp_rom_map_elem_t papp_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__papp) },
    { MP_ROM_QSTR(MP_QSTR_VfsSd), MP_ROM_PTR(&vfs_sd_type) },
    { MP_ROM_QSTR(MP_QSTR_BlockDev), MP_ROM_PTR(&sd_bdev_type) },
    { MP_ROM_QSTR(MP_QSTR_quit), MP_ROM_PTR(&papp_mod_quit_obj) },
    { MP_ROM_QSTR(MP_QSTR_sys_tar), MP_ROM_PTR(&papp_mod_sys_tar_obj) },
    { MP_ROM_QSTR(MP_QSTR_sys_version), MP_ROM_PTR(&papp_mod_sys_version_obj) },
};
static MP_DEFINE_CONST_DICT(papp_module_globals, papp_module_globals_table);

const mp_obj_module_t mp_module_papp = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&papp_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR__papp, mp_module_papp);
