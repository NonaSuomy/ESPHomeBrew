#!/usr/bin/env python3
"""Prebuild step for video: FFmpeg's configure output, without configure.

FFmpeg's configure needs a host shell toolchain and probes the build machine;
this writes the few files it would generate, for a fixed configuration that
suits the ESP32-P4 PAPP loader (plain C, no threads, no asm):

  <gen>/config.h                 every CONFIG_*, HAVE_* and ARCH_* macro the
                                 sources use, 0 or 1, plus configure's strings
  <gen>/config_components.h      includes config.h (the component macros are
                                 all there already)
  <gen>/libavutil/avconfig.h     AV_HAVE_BIGENDIAN, AV_HAVE_FAST_UNALIGNED
  <gen>/libavutil/ffversion.h    FFMPEG_VERSION
  <gen>/libavcodec/codec_list.c, parser_list.c, bsf_list.c
  <gen>/libavformat/demuxer_list.c, muxer_list.c, protocol_list.c

The enabled components are COMPONENTS below plus everything configure's
*_select / *_deps rules pull in (read from the pinned configure). The C files
compiled are listed explicitly in apps/video/papp.json; they are what
FFmpeg's Makefiles build for this set.

Macros are found by scanning the sources, so every one the code tests is
defined (FFmpeg writes `if (CONFIG_X)` in C as well as `#if CONFIG_X`, so an
undefined one would not compile). Reproducible: no dates, paths or host
details go into the output.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

VERSION = "7.1.5"

# What video plays. Decoders and demuxers first, then the parsers the
# demuxers want for these codecs, then the libraries.
COMPONENTS = """
    h264_decoder mpeg4_decoder mjpeg_decoder vp8_decoder
    aac_decoder mp3_decoder opus_decoder vorbis_decoder
    pcm_s16le_decoder pcm_s16be_decoder pcm_u8_decoder pcm_s24le_decoder pcm_s32le_decoder
    pcm_f32le_decoder pcm_alaw_decoder pcm_mulaw_decoder
    mov_demuxer matroska_demuxer avi_demuxer mpegts_demuxer mp3_demuxer ogg_demuxer wav_demuxer
    h264_parser aac_parser mpegaudio_parser opus_parser vorbis_parser
    mpeg4video_parser vp8_parser mjpeg_parser
    avcodec avformat avutil swresample
""".split()

# configure's *_suggest entries taken when a component suggests them
# (zlib, bzlib and iamf stay off: no zlib here, and they are rarely needed).
SUGGESTED = {"error_resilience"}

# Other CONFIG_ switches that are on (configure's defaults that matter here).
CONFIG_ON = """
    static safe_bitstream_reader decoders demuxers parsers
""".split()

# HAVE_ features newlib and GCC give this build (plus the syscalls the port
# supplies in ports/video/papp_syscalls.c). Everything else is 0: no threads,
# asm, SIMD, sockets or OS services.
HAVE_ON = """
    atan2f atanf cbrt cbrtf copysign cosf erf exp2 exp2f expf hypot isfinite isinf isnan
    ldexpf llrint llrintf log10f log2 log2f lrint lrintf powf rint round roundf sinf trunc truncf
    unistd_h sys_time_h gettimeofday clock_gettime usleep gmtime_r localtime_r posix_memalign
    local_aligned pragma_deprecated attribute_packed attribute_may_alias
""".split()

LIB_DIRS = ["libavutil", "libavcodec", "libavformat", "libswresample", "compat"]
TOKEN = re.compile(r"\b(?:CONFIG|HAVE|ARCH)_[A-Z0-9_]+\b")
# Defined on the command line (or by the header itself), never here.
NOT_OURS = {"HAVE_AV_CONFIG_H", "CONFIG_THIS_YEAR"}


def rules(configure: str) -> dict[str, dict[str, list[str]]]:
    out: dict[str, dict[str, list[str]]] = {}
    for m in re.finditer(r'^(\w+?)_(select|deps|suggest)="([^"]*)"', configure, re.M):
        out.setdefault(m.group(1), {})[m.group(2)] = m.group(3).split()
    return out


def closure(configure: str) -> set[str]:
    table = rules(configure)
    enabled: set[str] = set()
    todo = list(COMPONENTS)
    while todo:
        name = todo.pop()
        if name in enabled:
            continue
        enabled.add(name)
        rule = table.get(name, {})
        todo += rule.get("select", []) + rule.get("deps", [])
        todo += [s for s in rule.get("suggest", []) if s in SUGGESTED]
    return enabled


def externs(path: Path, struct: str, kind: str) -> list[str]:
    """configure's find_things_extern: ff_<name>_<kind> in declaration order."""
    pattern = re.compile(rf"^[^#\n]*extern\s[^\n]*{struct}\s+ff_(\w+)_{kind};", re.M)
    return [f"{m.group(1)}_{kind}" for m in pattern.finditer(path.read_text(encoding="utf-8"))]


def component_list(path: Path, struct: str, name: str, items: list[str], enabled: set[str]) -> None:
    lines = [f"static const {struct} * const {name}[] = {{"]
    lines += [f"    &ff_{c}," for c in items if c in enabled]
    lines += ["    NULL };", ""]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", type=Path, required=True)
    parser.add_argument("--gen", type=Path, required=True)
    args = parser.parse_args()
    src, gen = args.src, args.gen
    gen.mkdir(parents=True, exist_ok=True)

    enabled = closure((src / "configure").read_text(encoding="utf-8", errors="replace"))

    tokens: set[str] = set()
    for lib in LIB_DIRS:
        for path in sorted((src / lib).rglob("*")):
            if path.suffix in (".c", ".h") and path.is_file():
                tokens.update(TOKEN.findall(path.read_text(encoding="utf-8", errors="replace")))
    tokens -= NOT_OURS
    # Every component's switch, including the ones only built by token
    # pasting (pcm.c's CONFIG_ ## id ## _ENCODER), as configure lists them.
    avcodec, avformat = src / "libavcodec", src / "libavformat"
    everything = (externs(avcodec / "allcodecs.c", "FFCodec", "encoder")
                  + externs(avcodec / "allcodecs.c", "FFCodec", "decoder")
                  + externs(avcodec / "parsers.c", "AVCodecParser", "parser")
                  + externs(avcodec / "bitstream_filters.c", "FFBitStreamFilter", "bsf")
                  + externs(avcodec / "hwaccels.h", "FFHWAccel", "hwaccel")
                  + externs(avformat / "allformats.c", "FFInputFormat", "demuxer")
                  + externs(avformat / "allformats.c", "FFOutputFormat", "muxer")
                  + externs(avformat / "protocols.c", "URLProtocol", "protocol"))
    tokens |= {f"CONFIG_{c.upper()}" for c in everything}

    on = {f"CONFIG_{c.upper()}" for c in enabled | set(CONFIG_ON)} | {f"HAVE_{h.upper()}" for h in HAVE_ON}
    tokens |= on
    defines = "\n".join(f"#define {t} {1 if t in on else 0}" for t in sorted(tokens))
    (gen / "config.h").write_text(f"""/* Generated by ports/video/gen_ffmpeg.py for video - do not modify. */
#ifndef FFMPEG_CONFIG_H
#define FFMPEG_CONFIG_H
#define FFMPEG_CONFIGURATION "video (ports/video/gen_ffmpeg.py)"
#define FFMPEG_LICENSE "LGPL version 2.1 or later"
#define CONFIG_THIS_YEAR 2025
#define FFMPEG_DATADIR "/sd"
#define AVCONV_DATADIR "/sd"
#define CC_IDENT "riscv32-esp-elf-gcc"
#define OS_NAME none
#define EXTERN_PREFIX ""
#define EXTERN_ASM
#define BUILDSUF ""
#define SLIBSUF ".so"
#define SWS_MAX_FILTER_SIZE 256
{defines}
#endif /* FFMPEG_CONFIG_H */
""")
    (gen / "config_components.h").write_text("""/* Generated by ports/video/gen_ffmpeg.py - do not modify. */
#ifndef FFMPEG_CONFIG_COMPONENTS_H
#define FFMPEG_CONFIG_COMPONENTS_H
#include "config.h"
#endif /* FFMPEG_CONFIG_COMPONENTS_H */
""")
    (gen / "libavutil").mkdir(exist_ok=True)
    (gen / "libavutil" / "avconfig.h").write_text("""/* Generated by ports/video/gen_ffmpeg.py - do not modify. */
#ifndef AVUTIL_AVCONFIG_H
#define AVUTIL_AVCONFIG_H
#define AV_HAVE_BIGENDIAN 0
#define AV_HAVE_FAST_UNALIGNED 0
#endif /* AVUTIL_AVCONFIG_H */
""")
    (gen / "libavutil" / "ffversion.h").write_text(f"""/* Generated by ports/video/gen_ffmpeg.py - do not modify. */
#ifndef AVUTIL_FFVERSION_H
#define AVUTIL_FFVERSION_H
#define FFMPEG_VERSION "{VERSION}"
#endif /* AVUTIL_FFVERSION_H */
""")

    codecs = externs(avcodec / "allcodecs.c", "FFCodec", "encoder") + externs(avcodec / "allcodecs.c", "FFCodec", "decoder")
    component_list(gen / "libavcodec" / "codec_list.c", "FFCodec", "codec_list", codecs, enabled)
    component_list(gen / "libavcodec" / "parser_list.c", "AVCodecParser", "parser_list",
                   externs(avcodec / "parsers.c", "AVCodecParser", "parser"), enabled)
    component_list(gen / "libavcodec" / "bsf_list.c", "FFBitStreamFilter", "bitstream_filters",
                   externs(avcodec / "bitstream_filters.c", "FFBitStreamFilter", "bsf"), enabled)
    component_list(gen / "libavformat" / "demuxer_list.c", "FFInputFormat", "demuxer_list",
                   externs(avformat / "allformats.c", "FFInputFormat", "demuxer"), enabled)
    component_list(gen / "libavformat" / "muxer_list.c", "FFOutputFormat", "muxer_list",
                   externs(avformat / "allformats.c", "FFOutputFormat", "muxer"), enabled)
    component_list(gen / "libavformat" / "protocol_list.c", "URLProtocol", "url_protocols",
                   externs(avformat / "protocols.c", "URLProtocol", "protocol"), enabled)

    print(f"  gen_ffmpeg: {len(tokens)} macros, {sum(1 for t in tokens if t in on)} on; "
          f"{len(enabled)} components enabled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
