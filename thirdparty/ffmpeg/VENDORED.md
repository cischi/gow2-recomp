# FFmpeg subset vendored from xenia-canary

- Project: FFmpeg, fork https://github.com/has207/FFmpeg (branch `xmaframes`,
  submodule `third_party/FFmpeg` of xenia-canary)
- Commit: `d980192e175e6ff95bcd287af77e16fcb6597974` (FFmpeg 8.x with Xenia's
  xmaframes decoder ported on top)
- Configuration: overlay `third_party/ffmpeg-xenia/` of xenia-canary
  (https://github.com/xenia-canary/xenia-canary) @`e6cd2e4bc998609ca25e24a24c8e32b66c5908ad`:
  `config.h`, `config_components.h`, the `*_list.c` lists, `libavutil/avconfig.h`,
  `libavutil/ffversion.h` and the `premake5.lua` files the source list is derived from.
  Until 2026-10-22 the subset came from the xenia-project/FFmpeg fork @15ece08
  (FFmpeg 4.4).
- License: LGPL-2.1-or-later (`COPYING.LGPLv2.1` and `LICENSE.md` in this
  folder, copied from the fork; `FFMPEG_LICENSE` in `config.h` is "LGPL version 2.1
  or later"); some files have their own compatible permissive license
  ("License" column below). No file is under the GPL: no `CONFIG_GPL`,
  `CONFIG_NONFREE`, `CONFIG_GPLV3` is enabled and `libavutil/macos_kperf.h` (GPL,
  included only with `CONFIG_MACOS_KPERF`) is excluded (checked by
  `tests/test_vendor_ffmpeg.py`).
- Registered decoders (canary's `libavcodec/codec_list.c`): mp3float, mp3,
  wmapro, wmav2, xmaframes; the runtime only uses xmaframes.
- The original file headers are intact.
- Use: static library `xenia_ffmpeg` (`cmake/FFmpeg.cmake`) embedded in the
  GPL-3.0 executable under section 3 of the LGPL-2.1; the complete source of
  the subset is this folder.

Everything except `manifest.txt`, `VENDORED.md` and `patches/` is produced by
`tools/vendor_ffmpeg.py` and is never edited by hand:

```
python tools/vendor_ffmpeg.py --ffmpeg <has207/FFmpeg @d980192> --overlay <xenia-canary @e6cd2e4>                    # regenerate
python tools/vendor_ffmpeg.py --ffmpeg <has207/FFmpeg @d980192> --overlay <xenia-canary @e6cd2e4> --check            # verify byte by byte
python tools/vendor_ffmpeg.py --ffmpeg <has207/FFmpeg @d980192> --overlay <xenia-canary @e6cd2e4> --print-manifest   # expected manifest
```

The fork checkout is obtained with
`git init ffmpeg-has207 && git -C ffmpeg-has207 fetch --depth 1 https://github.com/has207/FFmpeg.git d980192e175e6ff95bcd287af77e16fcb6597974 && git -C ffmpeg-has207 checkout FETCH_HEAD`.
The script reads the files from the git objects of the two commits (not from the
working tree), copies only the paths in `manifest.txt` (from the overlay those
that exist there, except `*.lua` and `libavformat/`, the others from the fork)
and applies the patches in `patches/` in name order with `git apply -p1`
(`core.autocrlf=false`). `.gitattributes` at the repository root marks
`thirdparty/ffmpeg/**` as `-text`: the bytes stay those of the commits.

## How the manifest is derived

`--print-manifest` prints exactly `manifest.txt`
(`tests/test_vendor_ffmpeg.py` checks it):

- the `.c` files that the overlay's `libavcodec/premake5.lua` and
  `libavutil/premake5.lua` compile for Windows: `files()` blocks without a
  platform filter or with `platforms:Windows`; no `.S`/`.asm` (x86asm disabled);
  `../../FFmpeg/` prefix removed;
- the headers and `.c` files reached by their `#include "..."` (also under `#if`,
  hence an overestimate), searched in the file's folder, in the root, in
  `libavcodec/` and in `compat/atomics/win32`; paths of other architectures
  (`aarch64/`, `arm/`, `mips/`, `ppc/`, ...) and `libavutil/macos_kperf.h` are excluded;
- the overlay's configuration files;
- `COPYING.LGPLv2.1`, `LICENSE.md`, `compat/atomics/win32/stdatomic.h`.

`cmake/FFmpeg.cmake` compiles the `.c` files of `libavcodec/` and `libavutil/`
except `*_template.c` and `*_list.c` (included by other `.c` files). Options
like canary's `ffmpeg_common()`: `HAVE_AV_CONFIG_H`, `_USE_MATH_DEFINES`, include
`.`, `libavcodec` and `compat/atomics/win32`, link `bcrypt`; C11, warnings off
(`-w`) and `-Wno-incompatible-pointer-types` (the overlay's lists declare
`AVCodec*` where FFmpeg 8 wants `FFCodec*`: for MSVC it is a warning, for clang an
error by default).

## Patches

Every change to the vendored files is a patch in `patches/NNNN-<name>.patch`
(at most 10, each within 50 lines): every hunk contains a `gow2:` comment and
every patch has a row here with the reason and date (LGPL-2.1 section 2a).

| Patch | Date | Reason |
|---|---|---|

No patches: the subset builds with clang-cl against the overlay's
configuration without changes.

## Files

Path here (relative to `thirdparty/ffmpeg/`) and origin: a path of the fork
@d980192 or of the xenia-canary overlay @e6cd2e4.

| File | Origin | License |
|---|---|---|
| `COPYING.LGPLv2.1` | `COPYING.LGPLv2.1` | LGPL-2.1 text |
| `LICENSE.md` | `LICENSE.md` | FFmpeg project license notice |
| `compat/atomics/win32/stdatomic.h` | `compat/atomics/win32/stdatomic.h` | LGPL-2.1-or-later |
| `compat/os2threads.h` | `compat/os2threads.h` | LGPL-2.1-or-later |
| `compat/va_copy.h` | `compat/va_copy.h` | LGPL-2.1-or-later |
| `compat/w32pthreads.h` | `compat/w32pthreads.h` | LGPL-2.1-or-later |
| `config.h` | `third_party/ffmpeg-xenia/config.h` (xenia-canary @e6cd2e4) | LGPL-2.1-or-later (no header; xenia-canary overlay configuration) |
| `config_components.h` | `third_party/ffmpeg-xenia/config_components.h` (xenia-canary @e6cd2e4) | LGPL-2.1-or-later (no header; xenia-canary overlay configuration) |
| `libavcodec/aac_ac3_parser.h` | `libavcodec/aac_ac3_parser.h` | LGPL-2.1-or-later |
| `libavcodec/aactab.c` | `libavcodec/aactab.c` | LGPL-2.1-or-later |
| `libavcodec/aactab.h` | `libavcodec/aactab.h` | LGPL-2.1-or-later |
| `libavcodec/ac3_parser.c` | `libavcodec/ac3_parser.c` | LGPL-2.1-or-later |
| `libavcodec/ac3_parser.h` | `libavcodec/ac3_parser.h` | LGPL-2.1-or-later |
| `libavcodec/ac3_parser_internal.h` | `libavcodec/ac3_parser_internal.h` | LGPL-2.1-or-later |
| `libavcodec/ac3defs.h` | `libavcodec/ac3defs.h` | LGPL-2.1-or-later |
| `libavcodec/ac3tab.h` | `libavcodec/ac3tab.h` | LGPL-2.1-or-later |
| `libavcodec/adts_header.h` | `libavcodec/adts_header.h` | LGPL-2.1-or-later |
| `libavcodec/adts_parser.c` | `libavcodec/adts_parser.c` | LGPL-2.1-or-later |
| `libavcodec/adts_parser.h` | `libavcodec/adts_parser.h` | LGPL-2.1-or-later |
| `libavcodec/allcodecs.c` | `libavcodec/allcodecs.c` | LGPL-2.1-or-later |
| `libavcodec/avcodec.c` | `libavcodec/avcodec.c` | LGPL-2.1-or-later |
| `libavcodec/avcodec.h` | `libavcodec/avcodec.h` | LGPL-2.1-or-later |
| `libavcodec/avcodec_internal.h` | `libavcodec/avcodec_internal.h` | LGPL-2.1-or-later |
| `libavcodec/avdct.c` | `libavcodec/avdct.c` | LGPL-2.1-or-later |
| `libavcodec/avdct.h` | `libavcodec/avdct.h` | LGPL-2.1-or-later |
| `libavcodec/bit_depth_template.c` | `libavcodec/bit_depth_template.c` | LGPL-2.1-or-later |
| `libavcodec/bitstream.c` | `libavcodec/bitstream.c` | LGPL-2.1-or-later |
| `libavcodec/bitstream.h` | `libavcodec/bitstream.h` | LGPL-2.1-or-later |
| `libavcodec/bitstream_filters.c` | `libavcodec/bitstream_filters.c` | LGPL-2.1-or-later |
| `libavcodec/bitstream_template.h` | `libavcodec/bitstream_template.h` | LGPL-2.1-or-later |
| `libavcodec/bsf.c` | `libavcodec/bsf.c` | LGPL-2.1-or-later |
| `libavcodec/bsf.h` | `libavcodec/bsf.h` | LGPL-2.1-or-later |
| `libavcodec/bsf/null.c` | `libavcodec/bsf/null.c` | LGPL-2.1-or-later |
| `libavcodec/bsf_internal.h` | `libavcodec/bsf_internal.h` | LGPL-2.1-or-later |
| `libavcodec/bsf_list.c` | `third_party/ffmpeg-xenia/libavcodec/bsf_list.c` (xenia-canary @e6cd2e4) | LGPL-2.1-or-later (no header; xenia-canary overlay configuration) |
| `libavcodec/bytestream.h` | `libavcodec/bytestream.h` | LGPL-2.1-or-later |
| `libavcodec/codec.h` | `libavcodec/codec.h` | LGPL-2.1-or-later |
| `libavcodec/codec_desc.c` | `libavcodec/codec_desc.c` | LGPL-2.1-or-later |
| `libavcodec/codec_desc.h` | `libavcodec/codec_desc.h` | LGPL-2.1-or-later |
| `libavcodec/codec_id.h` | `libavcodec/codec_id.h` | LGPL-2.1-or-later |
| `libavcodec/codec_internal.h` | `libavcodec/codec_internal.h` | LGPL-2.1-or-later |
| `libavcodec/codec_list.c` | `third_party/ffmpeg-xenia/libavcodec/codec_list.c` (xenia-canary @e6cd2e4) | LGPL-2.1-or-later (no header; xenia-canary overlay configuration) |
| `libavcodec/codec_par.c` | `libavcodec/codec_par.c` | LGPL-2.1-or-later |
| `libavcodec/codec_par.h` | `libavcodec/codec_par.h` | LGPL-2.1-or-later |
| `libavcodec/d3d11va.c` | `libavcodec/d3d11va.c` | LGPL-2.1-or-later |
| `libavcodec/d3d11va.h` | `libavcodec/d3d11va.h` | LGPL-2.1-or-later |
| `libavcodec/dct.h` | `libavcodec/dct.h` | LGPL-2.1-or-later |
| `libavcodec/dct32.h` | `libavcodec/dct32.h` | LGPL-2.1-or-later |
| `libavcodec/dct32_fixed.c` | `libavcodec/dct32_fixed.c` | LGPL-2.1-or-later |
| `libavcodec/dct32_float.c` | `libavcodec/dct32_float.c` | LGPL-2.1-or-later |
| `libavcodec/dct32_template.c` | `libavcodec/dct32_template.c` | LGPL-2.1-or-later |
| `libavcodec/decode.c` | `libavcodec/decode.c` | LGPL-2.1-or-later |
| `libavcodec/decode.h` | `libavcodec/decode.h` | LGPL-2.1-or-later |
| `libavcodec/defs.h` | `libavcodec/defs.h` | LGPL-2.1-or-later |
| `libavcodec/dirac.c` | `libavcodec/dirac.c` | LGPL-2.1-or-later |
| `libavcodec/dirac.h` | `libavcodec/dirac.h` | LGPL-2.1-or-later |
| `libavcodec/dv_profile.c` | `libavcodec/dv_profile.c` | LGPL-2.1-or-later |
| `libavcodec/dv_profile.h` | `libavcodec/dv_profile.h` | LGPL-2.1-or-later |
| `libavcodec/dv_profile_internal.h` | `libavcodec/dv_profile_internal.h` | LGPL-2.1-or-later |
| `libavcodec/encode.c` | `libavcodec/encode.c` | LGPL-2.1-or-later |
| `libavcodec/encode.h` | `libavcodec/encode.h` | LGPL-2.1-or-later |
| `libavcodec/exif.c` | `libavcodec/exif.c` | LGPL-2.1-or-later |
| `libavcodec/exif.h` | `libavcodec/exif.h` | LGPL-2.1-or-later |
| `libavcodec/exif_internal.h` | `libavcodec/exif_internal.h` | LGPL-2.1-or-later |
| `libavcodec/faandct.c` | `libavcodec/faandct.c` | ISC |
| `libavcodec/faandct.h` | `libavcodec/faandct.h` | LGPL-2.1-or-later |
| `libavcodec/faanidct.c` | `libavcodec/faanidct.c` | LGPL-2.1-or-later |
| `libavcodec/faanidct.h` | `libavcodec/faanidct.h` | LGPL-2.1-or-later |
| `libavcodec/fdctdsp.c` | `libavcodec/fdctdsp.c` | LGPL-2.1-or-later |
| `libavcodec/fdctdsp.h` | `libavcodec/fdctdsp.h` | LGPL-2.1-or-later |
| `libavcodec/ffjni.h` | `libavcodec/ffjni.h` | LGPL-2.1-or-later |
| `libavcodec/fflcms2.h` | `libavcodec/fflcms2.h` | LGPL-2.1-or-later |
| `libavcodec/file_open.c` | `libavcodec/file_open.c` | LGPL-2.1-or-later (no header; generated by configure or includes an LGPL file) |
| `libavcodec/frame_thread_encoder.h` | `libavcodec/frame_thread_encoder.h` | LGPL-2.1-or-later |
| `libavcodec/get_bits.h` | `libavcodec/get_bits.h` | LGPL-2.1-or-later |
| `libavcodec/get_buffer.c` | `libavcodec/get_buffer.c` | LGPL-2.1-or-later |
| `libavcodec/golomb.h` | `libavcodec/golomb.h` | LGPL-2.1-or-later |
| `libavcodec/hwaccel_internal.h` | `libavcodec/hwaccel_internal.h` | LGPL-2.1-or-later |
| `libavcodec/hwaccels.h` | `libavcodec/hwaccels.h` | LGPL-2.1-or-later |
| `libavcodec/hwconfig.h` | `libavcodec/hwconfig.h` | LGPL-2.1-or-later |
| `libavcodec/idctdsp.c` | `libavcodec/idctdsp.c` | LGPL-2.1-or-later |
| `libavcodec/idctdsp.h` | `libavcodec/idctdsp.h` | LGPL-2.1-or-later |
| `libavcodec/imgconvert.c` | `libavcodec/imgconvert.c` | LGPL-2.1-or-later |
| `libavcodec/internal.h` | `libavcodec/internal.h` | LGPL-2.1-or-later |
| `libavcodec/jfdctfst.c` | `libavcodec/jfdctfst.c` | IJG license (Independent JPEG Group) |
| `libavcodec/jfdctint.c` | `libavcodec/jfdctint.c` | LGPL-2.1-or-later |
| `libavcodec/jfdctint_template.c` | `libavcodec/jfdctint_template.c` | IJG license (Independent JPEG Group) |
| `libavcodec/jni.c` | `libavcodec/jni.c` | LGPL-2.1-or-later |
| `libavcodec/jni.h` | `libavcodec/jni.h` | LGPL-2.1-or-later |
| `libavcodec/jrevdct.c` | `libavcodec/jrevdct.c` | IJG license (Independent JPEG Group) |
| `libavcodec/kbdwin.h` | `libavcodec/kbdwin.h` | LGPL-2.1-or-later |
| `libavcodec/lcevcdec.h` | `libavcodec/lcevcdec.h` | LGPL-2.1-or-later |
| `libavcodec/mathops.h` | `libavcodec/mathops.h` | LGPL-2.1-or-later |
| `libavcodec/mathtables.c` | `libavcodec/mathtables.c` | LGPL-2.1-or-later |
| `libavcodec/mediacodec.c` | `libavcodec/mediacodec.c` | LGPL-2.1-or-later |
| `libavcodec/mediacodec.h` | `libavcodec/mediacodec.h` | LGPL-2.1-or-later |
| `libavcodec/mediacodec_surface.h` | `libavcodec/mediacodec_surface.h` | LGPL-2.1-or-later |
| `libavcodec/mediacodec_wrapper.h` | `libavcodec/mediacodec_wrapper.h` | LGPL-2.1-or-later |
| `libavcodec/mediacodecdec_common.h` | `libavcodec/mediacodecdec_common.h` | LGPL-2.1-or-later |
| `libavcodec/mpeg12.h` | `libavcodec/mpeg12.h` | LGPL-2.1-or-later |
| `libavcodec/mpeg12data.h` | `libavcodec/mpeg12data.h` | LGPL-2.1-or-later |
| `libavcodec/mpeg12framerate.c` | `libavcodec/mpeg12framerate.c` | LGPL-2.1-or-later |
| `libavcodec/mpeg4audio.h` | `libavcodec/mpeg4audio.h` | LGPL-2.1-or-later |
| `libavcodec/mpegaudio.c` | `libavcodec/mpegaudio.c` | LGPL-2.1-or-later |
| `libavcodec/mpegaudio.h` | `libavcodec/mpegaudio.h` | LGPL-2.1-or-later |
| `libavcodec/mpegaudio_parser.c` | `libavcodec/mpegaudio_parser.c` | LGPL-2.1-or-later |
| `libavcodec/mpegaudio_tablegen.h` | `libavcodec/mpegaudio_tablegen.h` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodata.c` | `libavcodec/mpegaudiodata.c` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodata.h` | `libavcodec/mpegaudiodata.h` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodec_common.c` | `libavcodec/mpegaudiodec_common.c` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodec_common_tablegen.h` | `libavcodec/mpegaudiodec_common_tablegen.h` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodec_fixed.c` | `libavcodec/mpegaudiodec_fixed.c` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodec_float.c` | `libavcodec/mpegaudiodec_float.c` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodec_template.c` | `libavcodec/mpegaudiodec_template.c` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodecheader.c` | `libavcodec/mpegaudiodecheader.c` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodecheader.h` | `libavcodec/mpegaudiodecheader.h` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodsp.c` | `libavcodec/mpegaudiodsp.c` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodsp.h` | `libavcodec/mpegaudiodsp.h` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodsp_data.c` | `libavcodec/mpegaudiodsp_data.c` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodsp_fixed.c` | `libavcodec/mpegaudiodsp_fixed.c` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodsp_float.c` | `libavcodec/mpegaudiodsp_float.c` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiodsp_template.c` | `libavcodec/mpegaudiodsp_template.c` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiotabs.c` | `libavcodec/mpegaudiotabs.c` | LGPL-2.1-or-later |
| `libavcodec/mpegaudiotabs.h` | `libavcodec/mpegaudiotabs.h` | LGPL-2.1-or-later |
| `libavcodec/options.c` | `libavcodec/options.c` | LGPL-2.1-or-later |
| `libavcodec/options_table.h` | `libavcodec/options_table.h` | LGPL-2.1-or-later |
| `libavcodec/packet.c` | `libavcodec/packet.c` | LGPL-2.1-or-later |
| `libavcodec/packet.h` | `libavcodec/packet.h` | LGPL-2.1-or-later |
| `libavcodec/packet_internal.h` | `libavcodec/packet_internal.h` | LGPL-2.1-or-later |
| `libavcodec/parser.c` | `libavcodec/parser.c` | LGPL-2.1-or-later |
| `libavcodec/parser.h` | `libavcodec/parser.h` | LGPL-2.1-or-later |
| `libavcodec/parser_internal.h` | `libavcodec/parser_internal.h` | LGPL-2.1-or-later |
| `libavcodec/parser_list.c` | `third_party/ffmpeg-xenia/libavcodec/parser_list.c` (xenia-canary @e6cd2e4) | LGPL-2.1-or-later (no header; xenia-canary overlay configuration) |
| `libavcodec/parsers.c` | `libavcodec/parsers.c` | LGPL-2.1-or-later |
| `libavcodec/pixblockdsp.h` | `libavcodec/pixblockdsp.h` | LGPL-2.1-or-later |
| `libavcodec/profiles.c` | `libavcodec/profiles.c` | LGPL-2.1-or-later |
| `libavcodec/profiles.h` | `libavcodec/profiles.h` | LGPL-2.1-or-later |
| `libavcodec/progressframe.h` | `libavcodec/progressframe.h` | LGPL-2.1-or-later |
| `libavcodec/pthread.c` | `libavcodec/pthread.c` | LGPL-2.1-or-later |
| `libavcodec/pthread_frame.c` | `libavcodec/pthread_frame.c` | LGPL-2.1-or-later |
| `libavcodec/pthread_internal.h` | `libavcodec/pthread_internal.h` | LGPL-2.1-or-later |
| `libavcodec/pthread_slice.c` | `libavcodec/pthread_slice.c` | LGPL-2.1-or-later |
| `libavcodec/put_bits.h` | `libavcodec/put_bits.h` | LGPL-2.1-or-later |
| `libavcodec/qsv.h` | `libavcodec/qsv.h` | LGPL-2.1-or-later |
| `libavcodec/qsv_api.c` | `libavcodec/qsv_api.c` | LGPL-2.1-or-later |
| `libavcodec/raw.c` | `libavcodec/raw.c` | LGPL-2.1-or-later |
| `libavcodec/raw.h` | `libavcodec/raw.h` | LGPL-2.1-or-later |
| `libavcodec/raw_pix_fmt_tags.h` | `libavcodec/raw_pix_fmt_tags.h` | LGPL-2.1-or-later |
| `libavcodec/rnd_avg.h` | `libavcodec/rnd_avg.h` | LGPL-2.1-or-later |
| `libavcodec/simple_idct.c` | `libavcodec/simple_idct.c` | LGPL-2.1-or-later |
| `libavcodec/simple_idct.h` | `libavcodec/simple_idct.h` | LGPL-2.1-or-later |
| `libavcodec/simple_idct_template.c` | `libavcodec/simple_idct_template.c` | LGPL-2.1-or-later |
| `libavcodec/sinewin.c` | `libavcodec/sinewin.c` | LGPL-2.1-or-later |
| `libavcodec/sinewin.h` | `libavcodec/sinewin.h` | LGPL-2.1-or-later |
| `libavcodec/sinewin_tablegen.h` | `libavcodec/sinewin_tablegen.h` | LGPL-2.1-or-later |
| `libavcodec/startcode.h` | `libavcodec/startcode.h` | LGPL-2.1-or-later |
| `libavcodec/thread.h` | `libavcodec/thread.h` | LGPL-2.1-or-later |
| `libavcodec/threadframe.h` | `libavcodec/threadframe.h` | LGPL-2.1-or-later |
| `libavcodec/threadprogress.c` | `libavcodec/threadprogress.c` | LGPL-2.1-or-later |
| `libavcodec/threadprogress.h` | `libavcodec/threadprogress.h` | LGPL-2.1-or-later |
| `libavcodec/tiff_common.c` | `libavcodec/tiff_common.c` | LGPL-2.1-or-later |
| `libavcodec/tiff_common.h` | `libavcodec/tiff_common.h` | LGPL-2.1-or-later |
| `libavcodec/utils.c` | `libavcodec/utils.c` | LGPL-2.1-or-later |
| `libavcodec/version.h` | `libavcodec/version.h` | LGPL-2.1-or-later |
| `libavcodec/version_major.h` | `libavcodec/version_major.h` | LGPL-2.1-or-later |
| `libavcodec/vlc.c` | `libavcodec/vlc.c` | LGPL-2.1-or-later |
| `libavcodec/vlc.h` | `libavcodec/vlc.h` | LGPL-2.1-or-later |
| `libavcodec/vorbis_parser.c` | `libavcodec/vorbis_parser.c` | LGPL-2.1-or-later |
| `libavcodec/vorbis_parser.h` | `libavcodec/vorbis_parser.h` | LGPL-2.1-or-later |
| `libavcodec/vorbis_parser_internal.h` | `libavcodec/vorbis_parser_internal.h` | LGPL-2.1-or-later |
| `libavcodec/wma.c` | `libavcodec/wma.c` | LGPL-2.1-or-later |
| `libavcodec/wma.h` | `libavcodec/wma.h` | LGPL-2.1-or-later |
| `libavcodec/wma_common.c` | `libavcodec/wma_common.c` | LGPL-2.1-or-later |
| `libavcodec/wma_common.h` | `libavcodec/wma_common.h` | LGPL-2.1-or-later |
| `libavcodec/wma_freqs.c` | `libavcodec/wma_freqs.c` | LGPL-2.1-or-later |
| `libavcodec/wma_freqs.h` | `libavcodec/wma_freqs.h` | LGPL-2.1-or-later |
| `libavcodec/wmadata.h` | `libavcodec/wmadata.h` | LGPL-2.1-or-later |
| `libavcodec/wmadec.c` | `libavcodec/wmadec.c` | LGPL-2.1-or-later |
| `libavcodec/wmaprodata.h` | `libavcodec/wmaprodata.h` | LGPL-2.1-or-later |
| `libavcodec/wmaprodec.c` | `libavcodec/wmaprodec.c` | LGPL-2.1-or-later |
| `libavcodec/x86/constants.c` | `libavcodec/x86/constants.c` | LGPL-2.1-or-later |
| `libavcodec/x86/constants.h` | `libavcodec/x86/constants.h` | LGPL-2.1-or-later |
| `libavcodec/x86/fdct.c` | `libavcodec/x86/fdct.c` | LGPL-2.1-or-later |
| `libavcodec/x86/fdct.h` | `libavcodec/x86/fdct.h` | LGPL-2.1-or-later |
| `libavcodec/x86/fdctdsp_init.c` | `libavcodec/x86/fdctdsp_init.c` | LGPL-2.1-or-later |
| `libavcodec/x86/idctdsp.h` | `libavcodec/x86/idctdsp.h` | LGPL-2.1-or-later |
| `libavcodec/x86/idctdsp_init.c` | `libavcodec/x86/idctdsp_init.c` | LGPL-2.1-or-later |
| `libavcodec/x86/mathops.h` | `libavcodec/x86/mathops.h` | LGPL-2.1-or-later |
| `libavcodec/x86/mpegaudiodsp.c` | `libavcodec/x86/mpegaudiodsp.c` | LGPL-2.1-or-later |
| `libavcodec/x86/simple_idct.h` | `libavcodec/x86/simple_idct.h` | LGPL-2.1-or-later |
| `libavcodec/xiph.c` | `libavcodec/xiph.c` | LGPL-2.1-or-later |
| `libavcodec/xiph.h` | `libavcodec/xiph.h` | LGPL-2.1-or-later |
| `libavcodec/xvididct.h` | `libavcodec/xvididct.h` | LGPL-2.1-or-later |
| `libavformat/apetag.h` | `libavformat/apetag.h` | LGPL-2.1-or-later |
| `libavformat/avformat.h` | `libavformat/avformat.h` | LGPL-2.1-or-later |
| `libavformat/avio.h` | `libavformat/avio.h` | LGPL-2.1-or-later |
| `libavformat/id3v1.h` | `libavformat/id3v1.h` | LGPL-2.1-or-later |
| `libavformat/version.h` | `libavformat/version.h` | LGPL-2.1-or-later |
| `libavformat/version_major.h` | `libavformat/version_major.h` | LGPL-2.1-or-later |
| `libavutil/adler32.c` | `libavutil/adler32.c` | zlib |
| `libavutil/adler32.h` | `libavutil/adler32.h` | LGPL-2.1-or-later |
| `libavutil/aes.c` | `libavutil/aes.c` | LGPL-2.1-or-later |
| `libavutil/aes.h` | `libavutil/aes.h` | LGPL-2.1-or-later |
| `libavutil/aes_ctr.c` | `libavutil/aes_ctr.c` | LGPL-2.1-or-later |
| `libavutil/aes_ctr.h` | `libavutil/aes_ctr.h` | LGPL-2.1-or-later |
| `libavutil/aes_internal.h` | `libavutil/aes_internal.h` | LGPL-2.1-or-later |
| `libavutil/attributes.h` | `libavutil/attributes.h` | LGPL-2.1-or-later |
| `libavutil/attributes_internal.h` | `libavutil/attributes_internal.h` | LGPL-2.1-or-later |
| `libavutil/audio_fifo.c` | `libavutil/audio_fifo.c` | LGPL-2.1-or-later |
| `libavutil/audio_fifo.h` | `libavutil/audio_fifo.h` | LGPL-2.1-or-later |
| `libavutil/avassert.h` | `libavutil/avassert.h` | LGPL-2.1-or-later |
| `libavutil/avconfig.h` | `third_party/ffmpeg-xenia/libavutil/avconfig.h` (xenia-canary @e6cd2e4) | LGPL-2.1-or-later (no header; xenia-canary overlay configuration) |
| `libavutil/avsscanf.c` | `libavutil/avsscanf.c` | MIT (musl) |
| `libavutil/avstring.c` | `libavutil/avstring.c` | LGPL-2.1-or-later |
| `libavutil/avstring.h` | `libavutil/avstring.h` | LGPL-2.1-or-later |
| `libavutil/avutil.h` | `libavutil/avutil.h` | LGPL-2.1-or-later |
| `libavutil/base64.c` | `libavutil/base64.c` | LGPL-2.1-or-later |
| `libavutil/base64.h` | `libavutil/base64.h` | LGPL-2.1-or-later |
| `libavutil/blowfish.c` | `libavutil/blowfish.c` | LGPL-2.1-or-later |
| `libavutil/blowfish.h` | `libavutil/blowfish.h` | LGPL-2.1-or-later |
| `libavutil/bprint.c` | `libavutil/bprint.c` | LGPL-2.1-or-later |
| `libavutil/bprint.h` | `libavutil/bprint.h` | LGPL-2.1-or-later |
| `libavutil/bswap.h` | `libavutil/bswap.h` | LGPL-2.1-or-later |
| `libavutil/buffer.c` | `libavutil/buffer.c` | LGPL-2.1-or-later |
| `libavutil/buffer.h` | `libavutil/buffer.h` | LGPL-2.1-or-later |
| `libavutil/buffer_internal.h` | `libavutil/buffer_internal.h` | LGPL-2.1-or-later |
| `libavutil/camellia.c` | `libavutil/camellia.c` | LGPL-2.1-or-later |
| `libavutil/camellia.h` | `libavutil/camellia.h` | LGPL-2.1-or-later |
| `libavutil/cast5.c` | `libavutil/cast5.c` | LGPL-2.1-or-later |
| `libavutil/cast5.h` | `libavutil/cast5.h` | LGPL-2.1-or-later |
| `libavutil/channel_layout.c` | `libavutil/channel_layout.c` | LGPL-2.1-or-later |
| `libavutil/channel_layout.h` | `libavutil/channel_layout.h` | LGPL-2.1-or-later |
| `libavutil/common.h` | `libavutil/common.h` | LGPL-2.1-or-later |
| `libavutil/container_fifo.c` | `libavutil/container_fifo.c` | LGPL-2.1-or-later |
| `libavutil/container_fifo.h` | `libavutil/container_fifo.h` | LGPL-2.1-or-later |
| `libavutil/cpu.c` | `libavutil/cpu.c` | LGPL-2.1-or-later |
| `libavutil/cpu.h` | `libavutil/cpu.h` | LGPL-2.1-or-later |
| `libavutil/cpu_internal.h` | `libavutil/cpu_internal.h` | LGPL-2.1-or-later |
| `libavutil/crc.c` | `libavutil/crc.c` | LGPL-2.1-or-later |
| `libavutil/crc.h` | `libavutil/crc.h` | LGPL-2.1-or-later |
| `libavutil/csp.c` | `libavutil/csp.c` | LGPL-2.1-or-later |
| `libavutil/csp.h` | `libavutil/csp.h` | LGPL-2.1-or-later |
| `libavutil/des.c` | `libavutil/des.c` | LGPL-2.1-or-later |
| `libavutil/des.h` | `libavutil/des.h` | LGPL-2.1-or-later |
| `libavutil/dict.c` | `libavutil/dict.c` | LGPL-2.1-or-later |
| `libavutil/dict.h` | `libavutil/dict.h` | LGPL-2.1-or-later |
| `libavutil/display.c` | `libavutil/display.c` | LGPL-2.1-or-later |
| `libavutil/display.h` | `libavutil/display.h` | LGPL-2.1-or-later |
| `libavutil/dovi_meta.c` | `libavutil/dovi_meta.c` | LGPL-2.1-or-later |
| `libavutil/dovi_meta.h` | `libavutil/dovi_meta.h` | LGPL-2.1-or-later |
| `libavutil/downmix_info.c` | `libavutil/downmix_info.c` | LGPL-2.1-or-later |
| `libavutil/downmix_info.h` | `libavutil/downmix_info.h` | LGPL-2.1-or-later |
| `libavutil/dynarray.h` | `libavutil/dynarray.h` | LGPL-2.1-or-later |
| `libavutil/emms.h` | `libavutil/emms.h` | LGPL-2.1-or-later |
| `libavutil/encryption_info.c` | `libavutil/encryption_info.c` | LGPL-2.1-or-later |
| `libavutil/encryption_info.h` | `libavutil/encryption_info.h` | LGPL-2.1-or-later |
| `libavutil/error.c` | `libavutil/error.c` | LGPL-2.1-or-later |
| `libavutil/error.h` | `libavutil/error.h` | LGPL-2.1-or-later |
| `libavutil/eval.c` | `libavutil/eval.c` | LGPL-2.1-or-later |
| `libavutil/eval.h` | `libavutil/eval.h` | LGPL-2.1-or-later |
| `libavutil/ffmath.h` | `libavutil/ffmath.h` | LGPL-2.1-or-later |
| `libavutil/ffversion.h` | `third_party/ffmpeg-xenia/libavutil/ffversion.h` (xenia-canary @e6cd2e4) | LGPL-2.1-or-later (no header; xenia-canary overlay configuration) |
| `libavutil/fifo.c` | `libavutil/fifo.c` | LGPL-2.1-or-later |
| `libavutil/fifo.h` | `libavutil/fifo.h` | LGPL-2.1-or-later |
| `libavutil/file.c` | `libavutil/file.c` | LGPL-2.1-or-later |
| `libavutil/file.h` | `libavutil/file.h` | LGPL-2.1-or-later |
| `libavutil/file_open.c` | `libavutil/file_open.c` | LGPL-2.1-or-later |
| `libavutil/file_open.h` | `libavutil/file_open.h` | LGPL-2.1-or-later |
| `libavutil/film_grain_params.c` | `libavutil/film_grain_params.c` | LGPL-2.1-or-later |
| `libavutil/film_grain_params.h` | `libavutil/film_grain_params.h` | LGPL-2.1-or-later |
| `libavutil/fixed_dsp.c` | `libavutil/fixed_dsp.c` | LGPL-2.1-or-later |
| `libavutil/fixed_dsp.h` | `libavutil/fixed_dsp.h` | LGPL-2.1-or-later |
| `libavutil/float_dsp.c` | `libavutil/float_dsp.c` | LGPL-2.1-or-later |
| `libavutil/float_dsp.h` | `libavutil/float_dsp.h` | LGPL-2.1-or-later |
| `libavutil/float_scalarproduct.c` | `libavutil/float_scalarproduct.c` | LGPL-2.1-or-later |
| `libavutil/frame.c` | `libavutil/frame.c` | LGPL-2.1-or-later |
| `libavutil/frame.h` | `libavutil/frame.h` | LGPL-2.1-or-later |
| `libavutil/hash.c` | `libavutil/hash.c` | LGPL-2.1-or-later |
| `libavutil/hash.h` | `libavutil/hash.h` | LGPL-2.1-or-later |
| `libavutil/hdr_dynamic_metadata.c` | `libavutil/hdr_dynamic_metadata.c` | LGPL-2.1-or-later |
| `libavutil/hdr_dynamic_metadata.h` | `libavutil/hdr_dynamic_metadata.h` | LGPL-2.1-or-later |
| `libavutil/hmac.c` | `libavutil/hmac.c` | LGPL-2.1-or-later |
| `libavutil/hmac.h` | `libavutil/hmac.h` | LGPL-2.1-or-later |
| `libavutil/hwcontext.c` | `libavutil/hwcontext.c` | LGPL-2.1-or-later |
| `libavutil/hwcontext.h` | `libavutil/hwcontext.h` | LGPL-2.1-or-later |
| `libavutil/hwcontext_internal.h` | `libavutil/hwcontext_internal.h` | LGPL-2.1-or-later |
| `libavutil/iamf.c` | `libavutil/iamf.c` | LGPL-2.1-or-later |
| `libavutil/iamf.h` | `libavutil/iamf.h` | LGPL-2.1-or-later |
| `libavutil/imgutils.c` | `libavutil/imgutils.c` | LGPL-2.1-or-later |
| `libavutil/imgutils.h` | `libavutil/imgutils.h` | LGPL-2.1-or-later |
| `libavutil/imgutils_internal.h` | `libavutil/imgutils_internal.h` | LGPL-2.1-or-later |
| `libavutil/integer.c` | `libavutil/integer.c` | LGPL-2.1-or-later |
| `libavutil/integer.h` | `libavutil/integer.h` | LGPL-2.1-or-later |
| `libavutil/internal.h` | `libavutil/internal.h` | LGPL-2.1-or-later |
| `libavutil/intfloat.h` | `libavutil/intfloat.h` | LGPL-2.1-or-later |
| `libavutil/intmath.c` | `libavutil/intmath.c` | LGPL-2.1-or-later |
| `libavutil/intmath.h` | `libavutil/intmath.h` | LGPL-2.1-or-later |
| `libavutil/intreadwrite.h` | `libavutil/intreadwrite.h` | LGPL-2.1-or-later |
| `libavutil/lfg.c` | `libavutil/lfg.c` | LGPL-2.1-or-later |
| `libavutil/lfg.h` | `libavutil/lfg.h` | LGPL-2.1-or-later |
| `libavutil/libm.h` | `libavutil/libm.h` | LGPL-2.1-or-later |
| `libavutil/lls.c` | `libavutil/lls.c` | LGPL-2.1-or-later |
| `libavutil/lls.h` | `libavutil/lls.h` | LGPL-2.1-or-later |
| `libavutil/log.c` | `libavutil/log.c` | LGPL-2.1-or-later |
| `libavutil/log.h` | `libavutil/log.h` | LGPL-2.1-or-later |
| `libavutil/log2_tab.c` | `libavutil/log2_tab.c` | LGPL-2.1-or-later |
| `libavutil/macros.h` | `libavutil/macros.h` | LGPL-2.1-or-later |
| `libavutil/mastering_display_metadata.c` | `libavutil/mastering_display_metadata.c` | LGPL-2.1-or-later |
| `libavutil/mastering_display_metadata.h` | `libavutil/mastering_display_metadata.h` | LGPL-2.1-or-later |
| `libavutil/mathematics.c` | `libavutil/mathematics.c` | LGPL-2.1-or-later |
| `libavutil/mathematics.h` | `libavutil/mathematics.h` | LGPL-2.1-or-later |
| `libavutil/md5.c` | `libavutil/md5.c` | LGPL-2.1-or-later |
| `libavutil/md5.h` | `libavutil/md5.h` | LGPL-2.1-or-later |
| `libavutil/mem.c` | `libavutil/mem.c` | LGPL-2.1-or-later |
| `libavutil/mem.h` | `libavutil/mem.h` | LGPL-2.1-or-later |
| `libavutil/mem_internal.h` | `libavutil/mem_internal.h` | LGPL-2.1-or-later |
| `libavutil/murmur3.c` | `libavutil/murmur3.c` | LGPL-2.1-or-later |
| `libavutil/murmur3.h` | `libavutil/murmur3.h` | LGPL-2.1-or-later |
| `libavutil/opt.c` | `libavutil/opt.c` | LGPL-2.1-or-later |
| `libavutil/opt.h` | `libavutil/opt.h` | LGPL-2.1-or-later |
| `libavutil/parseutils.c` | `libavutil/parseutils.c` | LGPL-2.1-or-later |
| `libavutil/parseutils.h` | `libavutil/parseutils.h` | LGPL-2.1-or-later |
| `libavutil/pixdesc.c` | `libavutil/pixdesc.c` | LGPL-2.1-or-later |
| `libavutil/pixdesc.h` | `libavutil/pixdesc.h` | LGPL-2.1-or-later |
| `libavutil/pixelutils.c` | `libavutil/pixelutils.c` | LGPL-2.1-or-later |
| `libavutil/pixelutils.h` | `libavutil/pixelutils.h` | LGPL-2.1-or-later |
| `libavutil/pixfmt.h` | `libavutil/pixfmt.h` | LGPL-2.1-or-later |
| `libavutil/qsort.h` | `libavutil/qsort.h` | LGPL-2.1-or-later |
| `libavutil/random_seed.c` | `libavutil/random_seed.c` | LGPL-2.1-or-later |
| `libavutil/random_seed.h` | `libavutil/random_seed.h` | LGPL-2.1-or-later |
| `libavutil/rational.c` | `libavutil/rational.c` | LGPL-2.1-or-later |
| `libavutil/rational.h` | `libavutil/rational.h` | LGPL-2.1-or-later |
| `libavutil/rc4.c` | `libavutil/rc4.c` | LGPL-2.1-or-later |
| `libavutil/rc4.h` | `libavutil/rc4.h` | LGPL-2.1-or-later |
| `libavutil/refstruct.c` | `libavutil/refstruct.c` | LGPL-2.1-or-later |
| `libavutil/refstruct.h` | `libavutil/refstruct.h` | LGPL-2.1-or-later |
| `libavutil/reverse.c` | `libavutil/reverse.c` | LGPL-2.1-or-later |
| `libavutil/reverse.h` | `libavutil/reverse.h` | LGPL-2.1-or-later |
| `libavutil/ripemd.c` | `libavutil/ripemd.c` | LGPL-2.1-or-later |
| `libavutil/ripemd.h` | `libavutil/ripemd.h` | LGPL-2.1-or-later |
| `libavutil/samplefmt.c` | `libavutil/samplefmt.c` | LGPL-2.1-or-later |
| `libavutil/samplefmt.h` | `libavutil/samplefmt.h` | LGPL-2.1-or-later |
| `libavutil/sfc64.h` | `libavutil/sfc64.h` | LGPL-2.1-or-later |
| `libavutil/sha.c` | `libavutil/sha.c` | LGPL-2.1-or-later |
| `libavutil/sha.h` | `libavutil/sha.h` | LGPL-2.1-or-later |
| `libavutil/sha512.c` | `libavutil/sha512.c` | LGPL-2.1-or-later |
| `libavutil/sha512.h` | `libavutil/sha512.h` | LGPL-2.1-or-later |
| `libavutil/side_data.c` | `libavutil/side_data.c` | LGPL-2.1-or-later |
| `libavutil/side_data.h` | `libavutil/side_data.h` | LGPL-2.1-or-later |
| `libavutil/slicethread.c` | `libavutil/slicethread.c` | LGPL-2.1-or-later |
| `libavutil/slicethread.h` | `libavutil/slicethread.h` | LGPL-2.1-or-later |
| `libavutil/spherical.c` | `libavutil/spherical.c` | LGPL-2.1-or-later |
| `libavutil/spherical.h` | `libavutil/spherical.h` | LGPL-2.1-or-later |
| `libavutil/stereo3d.c` | `libavutil/stereo3d.c` | LGPL-2.1-or-later |
| `libavutil/stereo3d.h` | `libavutil/stereo3d.h` | LGPL-2.1-or-later |
| `libavutil/tea.c` | `libavutil/tea.c` | LGPL-2.1-or-later |
| `libavutil/tea.h` | `libavutil/tea.h` | LGPL-2.1-or-later |
| `libavutil/thread.h` | `libavutil/thread.h` | LGPL-2.1-or-later |
| `libavutil/threadmessage.c` | `libavutil/threadmessage.c` | LGPL-2.1-or-later |
| `libavutil/threadmessage.h` | `libavutil/threadmessage.h` | LGPL-2.1-or-later |
| `libavutil/time.c` | `libavutil/time.c` | LGPL-2.1-or-later |
| `libavutil/time.h` | `libavutil/time.h` | LGPL-2.1-or-later |
| `libavutil/time_internal.h` | `libavutil/time_internal.h` | LGPL-2.1-or-later |
| `libavutil/timecode.c` | `libavutil/timecode.c` | LGPL-2.1-or-later |
| `libavutil/timecode.h` | `libavutil/timecode.h` | LGPL-2.1-or-later |
| `libavutil/timecode_internal.c` | `libavutil/timecode_internal.c` | LGPL-2.1-or-later |
| `libavutil/timecode_internal.h` | `libavutil/timecode_internal.h` | LGPL-2.1-or-later |
| `libavutil/timer.h` | `libavutil/timer.h` | LGPL-2.1-or-later |
| `libavutil/timestamp.c` | `libavutil/timestamp.c` | LGPL-2.1-or-later |
| `libavutil/timestamp.h` | `libavutil/timestamp.h` | LGPL-2.1-or-later |
| `libavutil/tree.c` | `libavutil/tree.c` | LGPL-2.1-or-later |
| `libavutil/tree.h` | `libavutil/tree.h` | LGPL-2.1-or-later |
| `libavutil/twofish.c` | `libavutil/twofish.c` | LGPL-2.1-or-later |
| `libavutil/twofish.h` | `libavutil/twofish.h` | LGPL-2.1-or-later |
| `libavutil/tx.c` | `libavutil/tx.c` | LGPL-2.1-or-later |
| `libavutil/tx.h` | `libavutil/tx.h` | LGPL-2.1-or-later |
| `libavutil/tx_double.c` | `libavutil/tx_double.c` | LGPL-2.1-or-later |
| `libavutil/tx_float.c` | `libavutil/tx_float.c` | LGPL-2.1-or-later |
| `libavutil/tx_int32.c` | `libavutil/tx_int32.c` | LGPL-2.1-or-later |
| `libavutil/tx_priv.h` | `libavutil/tx_priv.h` | LGPL-2.1-or-later |
| `libavutil/tx_template.c` | `libavutil/tx_template.c` | LGPL-2.1-or-later |
| `libavutil/utils.c` | `libavutil/utils.c` | LGPL-2.1-or-later |
| `libavutil/version.h` | `libavutil/version.h` | LGPL-2.1-or-later |
| `libavutil/video_enc_params.c` | `libavutil/video_enc_params.c` | LGPL-2.1-or-later |
| `libavutil/video_enc_params.h` | `libavutil/video_enc_params.h` | LGPL-2.1-or-later |
| `libavutil/wchar_filename.h` | `libavutil/wchar_filename.h` | LGPL-2.1-or-later |
| `libavutil/x86/asm.h` | `libavutil/x86/asm.h` | LGPL-2.1-or-later |
| `libavutil/x86/bswap.h` | `libavutil/x86/bswap.h` | LGPL-2.1-or-later |
| `libavutil/x86/cpu.c` | `libavutil/x86/cpu.c` | LGPL-2.1-or-later |
| `libavutil/x86/cpu.h` | `libavutil/x86/cpu.h` | LGPL-2.1-or-later |
| `libavutil/x86/crc.h` | `libavutil/x86/crc.h` | LGPL-2.1-or-later |
| `libavutil/x86/fixed_dsp_init.c` | `libavutil/x86/fixed_dsp_init.c` | LGPL-2.1-or-later |
| `libavutil/x86/float_dsp_init.c` | `libavutil/x86/float_dsp_init.c` | LGPL-2.1-or-later |
| `libavutil/x86/imgutils_init.c` | `libavutil/x86/imgutils_init.c` | LGPL-2.1-or-later |
| `libavutil/x86/intmath.h` | `libavutil/x86/intmath.h` | LGPL-2.1-or-later |
| `libavutil/x86/intreadwrite.h` | `libavutil/x86/intreadwrite.h` | LGPL-2.1-or-later |
| `libavutil/x86/lls_init.c` | `libavutil/x86/lls_init.c` | LGPL-2.1-or-later |
| `libavutil/x86/pixelutils.h` | `libavutil/x86/pixelutils.h` | LGPL-2.1-or-later |
| `libavutil/x86/timer.h` | `libavutil/x86/timer.h` | LGPL-2.1-or-later |
| `libavutil/x86/tx_float_init.c` | `libavutil/x86/tx_float_init.c` | LGPL-2.1-or-later |
| `libavutil/xga_font_data.c` | `libavutil/xga_font_data.c` | LGPL-2.1-or-later |
| `libavutil/xga_font_data.h` | `libavutil/xga_font_data.h` | LGPL-2.1-or-later |
| `libavutil/xtea.c` | `libavutil/xtea.c` | LGPL-2.1-or-later |
| `libavutil/xtea.h` | `libavutil/xtea.h` | LGPL-2.1-or-later |
