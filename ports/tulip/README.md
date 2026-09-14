# Tulip Creative Computer on the PAPP loader (work in progress)

[Tulip CC](https://github.com/shorepine/tulipcc) built from Tulip Desktop's
code (`tulip/shared` of [NonaSuomy/tulipcc](https://github.com/NonaSuomy/tulipcc))
as a small MicroPython port for the ESP32-P4 PAPP loader. The upstream sources
are fetched at pinned commits (`apps/psram_tulip/papp.json`: tulipcc, and its
`micropython` and `amy` submodules); only the glue below lives here.

| File | What it does |
| --- | --- |
| `gen_tulip.py` | Prebuild: MicroPython's generated headers (qstrs, modules, root pointers, version), the LVGL binding (`lv_mpy.c`), frozen Python (bytecode via a host-built `mpy-cross`) and Tulip's `/sys` files as a tar |
| `mpconfigport.h`, `mphalport.h`, `papp_mphal.c`, `papp_modtime.c` | The MicroPython port: config, console, clocks |
| `papp_main.c` | `app_entry`, the MicroPython task (heap, LVGL, `_boot.py`, REPL), quitting |
| `papp_display.c` | Tulip's compositor into a 1024x600 loader canvas, or scaled to 800x480 on older loaders (30 fps target), keyboard, gamepad and touch input |
| `papp_audio.c` | AMY rendered into the loader's speaker (44.1 kHz), AMY's platform hooks |
| `papp_vfs.c` | `_papp`: `/sd` (files by name), the block device for Tulip's filesystem image, `/sys` tar, `quit()` |
| `papp_syscalls.c` | newlib: heap (PSRAM, freed on quit), files, time, spinlocks |
| `py/_boot.py` | Replaces Tulip's `_boot.py`: filesystems, `/sys`, then Tulip's own start-up |
| `patches/` | `tulip.board()` = `"PAPP"`, AMY's queue lock |

## Tasks and memory

- MicroPython: core 0, 96 KiB stack (PSRAM). GC heap in PSRAM: 16 MiB, or
  the largest of 12/8/6/4/2 MiB that still leaves 4 MiB for C allocations.
  LVGL allocates from the GC heap (`LV_STDLIB_MICROPYTHON`).
- Display: core 1, 16 KiB internal stack. Tulip's buffers: background
  928x580 RGB332 (538 KB), TFB layer (384 KB), LVGL band buffer, sprite RAM.
- Audio: core 1, 48 KiB stack (PSRAM). AMY's state is allocated at start.
- Everything is released when Tulip quits (the loader does not reclaim an
  app's heap or files).

## Files

- `/` is a littlefs filesystem in `/sd/roms/tulip/tulip.lfs` (16 MiB, made
  on first start; create the folder `/sd/roms/tulip` first, else
  `/sd/roms/tulip.lfs` is used). It holds `/user` (the working folder,
  `boot.py`, `lib`) and `/sys` (Tulip's examples and images, unpacked from the
  binary once per build), like a hardware Tulip's flash partitions.
- `/sd` is the card, for opening files by name. The loader has no directory
  listing, stat, mkdir or delete, so `ls('/sd')` raises `OSError`; copy files
  in with `cp('/sd/roms/x.py', '/user/x.py')`.
- Without a card Tulip runs on a 1 MiB RAM filesystem (nothing is kept).

## Input

- USB keyboard: the loader reports keys as taps. Ctrl arrives as its own
  tap, so it applies to the next key (Ctrl-C, Ctrl-X, Ctrl-Q, Ctrl-Tab);
  Alt/Shift/Home/End/Insert/F-keys are ignored, as Tulip's own scan_ascii()
  does. Arrow keys only exist as the loader's held gamepad directions, which
  W/A/S/D also press: a direction becomes an arrow key (with repeat) unless
  the same letter was typed around then.
- Gamepad: directions are arrow keys; held buttons appear in `tulip.keys()`
  as the scan codes `tulip_graphics.joyk()` maps to Tulip's `Joy` bits.
- Touch: Tulip's touch (`tulip.touch()`, touch callbacks) and LVGL's pointer.
- Quit: Menu (or Escape) held 3 s, the loader's close control, `_papp.quit()`,
  or Ctrl-D on an empty REPL line (a Tulip soft reboot restarts the chip).

## Not in this first version

- Network: no `socket`/`network`/TLS, so Tulip World, `tulip.wifi()`,
  `upgrade()` and `tuliprequests` do not work. The loader has UDP/TCP
  services a socket module could use; TLS would need mbedTLS in the app.
- MIDI: no MIDI in or out (the loader has no MIDI service); AMY runs with
  `AMY_HOST_MIDI` and empty `run_midi`/`stop_midi`/`midi_out`.
- Native code: no `@micropython.native`/`viper` and no tinycc (C compiled
  on the device): the loader has no executable-memory allocation. The
  tinycc submodule (LGPL) is not fetched or linked.
- Gamma9001 drum samples (they need a generated `drums_bin.c`), audio input.
- Alles / multi-device sync (already off in upstream `modtulip.c`).
- USB mouse (the loader's mouse service works, but there is no pointer yet).
- Wall clock: `time.time()` counts from 1970-01-01 at start (no RTC service).
