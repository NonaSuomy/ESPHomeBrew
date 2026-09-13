# _boot.py for Tulip on the ESP32-P4 PAPP loader.
# Replaces tulip/shared/py/_boot.py; the rest of Tulip's Python is frozen as is.
#
# Filesystems, as on a Tulip with its flash partitions:
#   /       Tulip's own littlefs filesystem (/user, /sys, /user/lib), kept in
#           one image file on the card: /sd/roms/tulip/tulip.lfs (16 MiB,
#           made on first start).
#   /sd     the SD card, for opening (and importing) files by name. The
#           loader cannot list folders or delete files, so ls('/sd') fails.
import gc
import sys
import os
import vfs
import _papp

_IMAGES = ('/sd/roms/tulip/tulip.lfs', '/sd/roms/tulip.lfs')
_BLOCK = 4096
_BLOCKS = 4096


class _RAMBlockDev:
    # No card: a filesystem in RAM, so Tulip still runs (nothing is kept).
    def __init__(self, blocks):
        self.data = bytearray(blocks * _BLOCK)
        self.blocks = blocks

    def readblocks(self, n, buf, off=0):
        a = n * _BLOCK + off
        buf[:] = self.data[a:a + len(buf)]

    def writeblocks(self, n, buf, off=0):
        a = n * _BLOCK + off
        self.data[a:a + len(buf)] = buf

    def ioctl(self, op, arg):
        if op == 4:
            return self.blocks
        if op == 5:
            return _BLOCK
        return 0


def _mount_lfs(bdev):
    try:
        vfs.mount(vfs.VfsLfs2(bdev), '/')
    except OSError:
        print('Making a new Tulip filesystem...')
        vfs.VfsLfs2.mkfs(bdev)
        vfs.mount(vfs.VfsLfs2(bdev), '/')


def _mount_filesystems():
    try:
        vfs.mount(_papp.VfsSd(), '/sd')
    except Exception as e:
        print('papp: /sd not mounted:', e)
    for image in _IMAGES:
        try:
            bdev = _papp.BlockDev(image, _BLOCK, _BLOCKS)
        except OSError:
            continue
        try:
            _mount_lfs(bdev)
            return image
        except Exception as e:
            print('papp: cannot use', image, e)
    _mount_lfs(_RAMBlockDev(256))
    return None


def _makedirs(path):
    here = ''
    for part in path.strip('/').split('/'):
        here += '/' + part
        try:
            os.mkdir(here)
        except OSError:
            pass


def _install_sys():
    # Tulip's system files (examples, images: tulip/fs/tulip) are built into
    # this app as a tar; unpack them into /sys once per build.
    version = _papp.sys_version()
    try:
        with open('/sys/.papp_version') as f:
            if f.read() == version:
                return
    except OSError:
        pass
    print('Installing Tulip system files in /sys...')
    tar = _papp.sys_tar()
    pos, count = 0, 0
    while pos + 512 <= len(tar):
        header = bytes(tar[pos:pos + 512])
        name = header[0:100].split(b'\0', 1)[0].decode()
        if not name:
            break
        size = int(header[124:136].split(b'\0', 1)[0].strip().decode() or '0', 8)
        prefix = header[345:500].split(b'\0', 1)[0].decode()
        if prefix:
            name = prefix + '/' + name
        pos += 512
        if header[156] in (0, 48):  # a regular file
            path = '/sys/' + name
            _makedirs(path.rsplit('/', 1)[0])
            with open(path, 'wb') as f:
                f.write(tar[pos:pos + size])
            count += 1
        pos += (size + 511) // 512 * 512
    with open('/sys/.papp_version', 'w') as f:
        f.write(version)
    print(count, 'files')


_image = _mount_filesystems()
for _d in ('/user', '/sys', '/user/lib'):
    _makedirs(_d)
try:
    _install_sys()
except Exception as e:
    print('papp: /sys not installed:', e)
os.chdir('/user')
sys.path.append('/user/lib')
sys.path.append('/sys/ex')
gc.collect()

# Tulip itself (tulip/shared/py/_boot.py from here on, for a Tulip with a
# screen and no network).
try:
    import tulip, midi, synth, amy, world, sequencer
    from upysh import *
    from tulip import board, edit, run

    tulip.add_to_bootpy("# boot.py\n# Put anything here you want to run on Tulip startup\n", only_first_create=True)

    amy.AMY_SAMPLE_RATE = 44100
    # amy.message() in C (see Tulip's _boot.py): the Python original stays
    # the fallback for anything the C version does not reproduce exactly.
    _py_amy_message = amy.message
    amy._py_message = _py_amy_message

    def _c_amy_message(**kwargs):
        if amy.show_warnings and ('patch_string' in kwargs or 'num_partials' in kwargs or
                ('voices' in kwargs and ('preset' in kwargs or 'synth' in kwargs))):
            return _py_amy_message(**kwargs)
        m = tulip.amy_message(**kwargs)
        if m is None:
            return _py_amy_message(**kwargs)
        return m
    amy.message = _c_amy_message

    midi.setup()
except Exception as e:
    # Keep a usable REPL even if part of Tulip failed to start.
    sys.print_exception(e)
    import _tulip
    _tulip.set_screen_as_repl(1)

print('Tulip on the ESP32-P4 PAPP loader. Files: /user (%s), card: /sd.' %
      (_image or 'in RAM, not kept: no card'))
print('Hold Menu (or Escape) for 3 s to quit. Ctrl-D returns to the launcher.')
del _d
gc.collect()
