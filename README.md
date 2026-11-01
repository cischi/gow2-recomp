# gow2-recomp: Gears of War 2 and 3 PC port (static recompilation)

An unofficial, fan-made PC port of Gears of War 2 and Gears of War 3 (Xbox 360). Each game's executable (`default.xex`)
is translated ahead of time into C++ by [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and compiled into a
native Windows program, `gow2.exe` and `gow3.exe`; `gears.exe` is a small window that lets you pick one of them. A runtime written for this project implements the Xbox 360 kernel, GPU, audio and input on top of
components from [Xenia](https://github.com/xenia-canary/xenia-canary), Direct3D 12, SDL3 and Dear ImGui.

**This project is not affiliated with or endorsed by Microsoft, Xbox Game Studios or Epic Games.** "Gears of War" is a
trademark of Microsoft; it is used here only to identify the game the port works with.

**You need your own copy of the game:** a disc image (ISO) for Xbox 360 of Gears of War 2 (**PAL Europe edition**)
and/or Gears of War 3 (**World or Italian edition**).
No game files are included in this repository or in the releases. Other editions are rejected: each exe checks the
SHA-256 of the decrypted `default.xex` image against the version it was recompiled from, so a dump from another region
is accepted only if it contains exactly the same code (for Gears of War 3 the World and Italian discs both qualify).

## Contents

- [System requirements](#system-requirements)
- [Installing and playing](#installing-and-playing)
- [Settings](#settings)
- [Controls](#controls)
- [Status and known limits](#status-and-known-limits)
- [Command-line options](#command-line-options)
- [Files and folders](#files-and-folders)
- [Building from source](#building-from-source)
- [Tests](#tests)
- [Legal](#legal)
- [Credits](#credits)
- [License](#license)

## System requirements

- Windows 10 version 1803 or later (the physical memory views use `VirtualAlloc2`/`MapViewOfFile3`).
- A CPU with AVX2 (the vendored Xenia code is compiled with `/arch:AVX2`; `gow2.exe` does not start without it).
- A GPU with a Direct3D 12 driver and feature level 11_0 (a hardware adapter; `gow2.exe --gpu-probe` prints the one
  it finds).
- Free disk space for the extracted game data: about 6.5 GB for Gears of War 2, about 8 GB for Gears of War 3.
- Optional: an NVIDIA RTX GPU for DLSS (RTX 40 or later for DLSS frame generation); AMD FSR works on any GPU.

## Installing and playing

1. Download the release zip and extract it to a folder of your choice.
2. Run `gears.exe` and pick the game, or run `gow2.exe` / `gow3.exe` directly. The game's launcher opens.
3. The launcher reports "Game data missing". Click **Choose ISO...** and select your ISO (Gears of War 2: PAL Europe;
   Gears of War 3: World or Italian).
4. The launcher checks the ISO (valid Xbox 360 image, `default.xex` of the supported edition, enough free space) and
   extracts it to `game_data` (Gears of War 3: `game_data_gow3`) next to the exe, with a progress bar. Extraction can be cancelled; an interrupted
   extraction is never mistaken for a complete one. After it finishes the ISO is no longer needed.
5. Click **Play**. It stays disabled until the game data is ready.

To extract again (for example from another ISO after an error), use the same **Choose ISO...** button in the launcher.

## Settings

Settings are stored in `gow2.toml` next to `gow2.exe`. They can be changed in two places that share the same panel:

- **Launcher**: opens when `gow2.exe` is started without arguments (double click). **Play** saves `gow2.toml` and starts
  the game (comments written by hand in the file are not kept), **Quit** closes without touching the file, **Defaults**
  resets the controls. Mouse, keyboard or an XInput pad work; Enter or A starts the game. With an invalid `gow2.toml`
  the launcher shows the error and starts from the defaults.
- **F1 overlay**: during the game, **F1** opens the same panel over the game. The game keeps running; while the panel
  is open, keyboard and mouse go to the panel only (the pad stays with the game). **Apply** saves `gow2.toml` and
  applies immediately what can change at runtime (V-Sync, VRR, scaling, sharpness, anisotropic filtering,
  anti-aliasing, game patches, window mode and resolution); the panel marks the options that take effect at the next
  launch. **Close**, Esc or F1 again close it without applying.
- **Screenshots**: **F12** saves the next frame, as shown on screen (overlay included), to
  `screenshots\gow2\` or `screenshots\gow3\` next to the executable, named `<game>_YYYY-MM-DD_HH-MM-SS-mmm.bmp`
  (24-bit BMP at the output resolution).

The menus are in Italian or English: Italian if `[game] language` is `it`, English for the other languages; without
`[game] language` they follow the Windows display language. **UI scale** (75-150%) resizes the launcher and the overlay.

### Display and image

- **Mode** (windowed, borderless, fullscreen), **Resolution**, **Scaling** (fit with black bars, or stretch),
  **V-Sync** and **VRR** (G-Sync/FreeSync: keeps the fps below the refresh rate to stay in the VRR window; "auto"
  detects it on NVIDIA through NVAPI, with FreeSync choose "on").
- **Frame rate**: 30 (as on Xbox 360, the default), 60, 120, 144, the monitor's refresh rate, or unlimited. Rates
  above about 190 fps have not been tested.
- **Aspect ratio**: 16:9, 21:9 (wider field of view, Hor+) or 16:10 (Vert+). On a monitor of a different shape the
  image has black bars or is stretched; movies stay 16:9.
- **Internal resolution**: 1x (1280x720, the original), 2x (2560x1440) or 3x (3840x2160).
- **Upscaler**: off, AMD FSR (FSR 4 on Radeon RX 7000/9000, FSR 3.1 on other GPUs, NVIDIA included) or NVIDIA DLSS
  Super Resolution (RTX GPUs), with DLSS mode (auto, DLAA, Quality, Balanced, Performance, Ultra Performance) and preset.
  The game renders at the internal resolution and the upscaler rebuilds the image at the window size, using depth,
  motion vectors and sub-pixel jitter.
- **Frame generation**: off, AMD FSR 3 or NVIDIA DLSS (RTX 40 or later): one generated frame between two real ones.
  Costs about one frame of latency; exclusive fullscreen becomes borderless.
- **HDR**: HDR10 output with peak brightness and paper white settings. Needs Windows HDR turned on for the display.
  Only verified by measuring screenshots, not yet on a real HDR monitor.
- **Anti-aliasing** (FXAA), **sharpness** (AMD CAS, or FSR's RCAS with FSR), **anisotropic filtering** (default 16x),
  **enhanced shaders** (replacement shaders for some effects at internal resolution above 1x).
- **Engine**: shadow quality (low, original, high) and dynamic shadows on/off. They patch copies of the game's
  `Coalesced_*.bin` written to `user_data/engine`; the game data is never modified.
- **Game patches**: FPS counter, no motion blur, no lens flares, no bloom (see [Credits](#credits)).

### NVIDIA DLSS

The release contains `gow2_dlss.dll`, the project's DLSS bridge (MIT), but not NVIDIA's DLLs, whose license does not
allow them to be bundled here. To use DLSS or DLSS frame generation, click **Download NVIDIA DLSS** in the launcher or
in the F1 overlay: it downloads `nvngx_dlss.dll` and `nvngx_dlssg.dll` (version 310.9.16) from
[NVIDIA's official DLSS repository on GitHub](https://github.com/NVIDIA/DLSS), checks their SHA-256 and places them
next to `gow2.exe`. These DLLs are covered by NVIDIA's license, not by this project's. Until they are present, the
DLSS entries stay disabled.

### OptiScaler (neural rendering)

The **Neural rendering** row of the launcher and of the F1 overlay can install
[OptiScaler](https://github.com/optiscaler/OptiScaler) on request: a third-party proxy (GPL-3.0), in the version pinned
by this project (v0.1.34 of the [DLSSNR-F5 fork by janblade](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass)).
The launcher downloads the zip from GitHub, checks its SHA-256 and puts `dxgi.dll`, `OptiScaler.ini`,
`nvngx.dll_dlssnr.dll` and the `OptiScaler\` folder next to `gow2.exe`; it never touches a `dxgi.dll` that belongs to
another program. Neural rendering needs `upscaler = "dlss"` and `nvngx_dlssnr.dll`, which is not provided by this
project. With OptiScaler installed, the game's own frame generation is turned off (choose it in OptiScaler's menu,
INSERT). To remove OptiScaler, delete `dxgi.dll`, `OptiScaler.ini`, `nvngx.dll_dlssnr.dll` and `OptiScaler\`.
Antivirus programs may report false positives on OptiScaler (documented by the fork).

### Log console

**Show log console** (section Game, takes effect on the next launch; `[debug] console = true` in `gow2.toml`) opens a
console window with the log. Without it `gow2.exe` shows no console when double-clicked. `gow2.log` is always written
to the current directory. Started from a terminal, `gow2.exe` prints to that terminal. It is a GUI-subsystem program, so cmd
and PowerShell return to the prompt immediately: use `start /wait gow2.exe --help` (cmd) or
`Start-Process -Wait` / `.\gow2.exe --help | Out-Host` (PowerShell) to wait for it and see the exit code.

### Recommended settings

Measured on 2026-10-26: RTX 4080, borderless fullscreen at 2560x1440, frame rate `unlimited`, same game scene, 1 to 3
runs each (runs differ by up to 10%):

| Internal resolution | Upscaler | fps |
|---|---|---|
| 1x | DLSS (Performance, 720p -> 1440p) | ~137 |
| 2x | off | ~102 |
| 2x | DLSS (DLAA) | ~100 |
| 3x | DLSS (DLAA at 2160p, then downscaled) | ~53 |

For a high refresh rate monitor, 1x with DLSS; for the best quality at 1440p, 2x with DLAA, which stays above 60 fps.
3x on a 1440p output costs about half the fps of 2x for a small visual gain: it makes sense with a 2160p output.

## Controls

- Xbox pads (360, One, Series) through XInput; DualSense, DualShock 4, Switch Pro and the other gamepads recognized by
  SDL3, with no Steam Input or DS4Windows needed. Close DS4Windows (or other ViGEm tools) or hide the real pad with
  HidHide, otherwise the same pad shows up twice. Rumble is forwarded to the pads.
- Keyboard fallback (window focused, no pad on user 0): Enter = START, Esc = BACK, Space = A, WASD = left stick,
  arrows = d-pad.
- Alt+Enter toggles windowed and fullscreen.

## Status and known limits

The single-player campaign of both games is playable, with movies and saves. The settings, files and options below
are written for `gow2.exe`; `gow3.exe` works the same way with `gow3` in the names (`gow3.toml`, `gow3.log`,
`game_data_gow3/`, `user_data_gow3/`).

- No multiplayer and no co-op: network and Xbox Live services are not implemented.
- No voice chat.
- The first launch with D3D12 can be slow (cold driver and shader caches); later launches are faster.
- Not checked with the 21:9 and 16:10 aspect ratios: pause, options, map, split screen, zoomed aim.
- Rumble has not been verified with a physical pad.

## Command-line options

Without arguments `gow2.exe` opens the launcher; with any argument it starts the game directly (add `--launcher` to
open the launcher anyway). Command-line options override `gow2.toml` (precedence: defaults, `gow2.toml`, command line).
An invalid value is a usage error (exit 2).

| Option | Meaning |
|---|---|
| `--data <folder>` | Folder with `default.xex` (default: `.\game_data` in the current directory, otherwise `game_data` next to the executable). |
| `--user-data <folder>` | User data folder (default: `user_data` next to the data folder). |
| `--config <file>` | Settings file (default: `gow2.toml` next to `gow2.exe`, or the `GOW2_CONFIG` environment variable). |
| `--launcher` | Open the launcher even with other options. |
| `--window windowed\|borderless\|fullscreen` | Window mode. |
| `--resolution WxH` | Window or fullscreen resolution (320-7680 x 240-4320, physical pixels). |
| `--vsync on\|off` | V-Sync. |
| `--frame-rate 30\|60\|120\|144\|monitor\|unlimited` | Game frame rate. |
| `--vrr auto\|on\|off` | Keep the fps inside the VRR window. |
| `--frame-generation off\|fsr\|dlss` | Frame generation. |
| `--aspect 16:9\|21:9\|16:10` | Scene aspect ratio. |
| `--hdr on\|off\|force` | HDR10 output (`force`: HDR10 even on an SDR output, for testing; not saved). |
| `--hdr-peak-nits <n>` | Peak brightness: 0 = auto, or 100-10000. |
| `--hdr-paper-white-nits <n>` | Paper white: 0 = auto, or 80-1000. |
| `--internal-resolution 1\|2\|3` | Render resolution multiplier. |
| `--upscaler off\|fsr\|dlss` | Temporal upscaler. |
| `--dlss-mode auto\|dlaa\|quality\|balanced\|performance\|ultra_performance` | DLSS mode (if not `auto`, it decides the internal resolution). |
| `--dlss-preset default\|e\|f\|j\|k\|l\|m` | DLSS preset. |
| `--aa off\|fxaa\|fxaa_extreme` | Anti-aliasing. |
| `--sharpening off\|0.0..1.0` | Sharpness (CAS, or RCAS with FSR). |
| `--anisotropic 0\|2\|4\|8\|16` | Forced anisotropic filtering (0 = the game's own). |
| `--enhanced-shaders on\|off` | Replacement shaders. |
| `--shadow-quality low\|original\|high` | Shadow map resolution (256, 800, 2048). |
| `--dynamic-shadows on\|off` | Dynamic shadows. |
| `--language <code>` | Game language: `en ja de fr es it ko zh zh-cn pt pl ru` (default `en`). |
| `--gpu fake\|d3d12` | GPU backend (default `d3d12`; `fake` has no window and no renderer, for tests). |
| `--gpu-probe` | Print the first hardware D3D12 adapter and exit. |
| `--gpu-cvar name=value` | Set a cvar of Xenia's GPU code (repeatable). |
| `--audio xaudio2\|null` | Audio output (default `xaudio2`, falls back to the null sink). |
| `--mute` | No sound, same pacing. |
| `--audio-capture <file.wav>` | Write the game's audio to a 48 kHz 5.1 float WAV. |
| `--xma-decoder new\|old` | XMA decoder of xenia-canary (default `new`). |
| `--run-seconds N` | Write the summary and exit with 0 after N seconds (1-86400). |
| `--screenshot S:file` | Save the first frame presented after S seconds as a 24-bit BMP (repeatable). |
| `--input-script "<s>:<BUTTON>[+<ms>],..."` | Scripted presses of virtual pad 0, e.g. `"40:START,45.5:A+250"`. |
| `--skip-movie <name>` | Replace `<name>.bik` with an almost empty movie (repeatable; for tests). |
| `--trace` | Log every call to the kernel imports. |
| `--help` | Print the usage line and exit (exit code 2). |

Exit codes (wait for the process to read them: `start /wait` in cmd, `Start-Process -Wait` in PowerShell, see
[Log console](#log-console)):

| Code | Meaning |
|---|---|
| 0 | Normal exit (window closed, `--run-seconds` elapsed, game returned) |
| 1 | Runtime error (for example `user_data` cannot be created) |
| 2 | Incorrect usage, or `default.xex` missing or of an unsupported edition |
| 3 | Crash (unhandled exception; `crash host=...` line in the log) |
| 4 | `KeBugCheck` raised by the game |
| 5 | GPU error (D3D12 device not created or removed, Xenia fatal error) |

### gow2.toml

The launcher writes this file; it can also be edited by hand. An invalid file prints
`gow2.toml:<line>: <key>: <reason>` and exits with 2. Example:

```toml
[video]
mode = "borderless"          # "windowed", "borderless", "fullscreen"
width = 2560                 # 0 = the mode's default
height = 1440
scaling = "fit"              # "fit", "stretch"
vsync = true
resizable = true
antialiasing = "off"         # "off", "fxaa", "fxaa_extreme"
sharpening = false
sharpness = 0.0              # 0.0-1.0
internal_resolution = 2      # 1, 2, 3
anisotropic = 16             # 0, 2, 4, 8, 16
enhanced_shaders = true
upscaler = "dlss"            # "off", "fsr", "dlss"
dlss_preset = "default"      # "default", "e", "f", "j", "k", "l", "m"
dlss_mode = "auto"           # "auto", "dlaa", "quality", "balanced", "performance", "ultra_performance"
vrr = "auto"                 # "auto", "on", "off"
frame_generation = "off"     # "off", "fsr", "dlss"
hdr = false
hdr_peak_nits = 0            # 0 = auto, otherwise 100-10000
hdr_paper_white_nits = 0     # 0 = auto, otherwise 80-1000
aspect = "16:9"              # "16:9", "21:9", "16:10"
ui_scale = 100               # 75, 90, 100, 110, 125, 150

[game]
frame_rate = 60              # 30, 60, 120, 144, "monitor", "unlimited"
# language = "en"            # codes of --language

[engine]
shadow_quality = "original"  # "low", "original", "high"
dynamic_shadows = true

[patches]
show_fps = false
disable_motion_blur = false
disable_lens_flares = false
disable_bloom = false

[debug]
console = false              # log console window

# [gpu]
# cvars = { name = "value" } # like --gpu-cvar
```

## Files and folders

- `game_data/`: the extracted game (read-only for the game). Never share it.
- `user_data/`: created next to the data folder, or where `--user-data` points. `content/` holds the saves, `cache/` the
  game's cache device, `engine/` the patched engine settings, `shaders/` the shader and pipeline cache (rebuilt when
  deleted; it removes the first-use hitches on later runs). Delete it to start over (this deletes your saves).
- `gow2.toml`: settings. `gow2.log`: log of the last run, written to the current directory.

## Building from source

Building needs your own copy of the game too: the recompiled C++ is generated from your `default.xex` and never leaves
your machine.

### Prerequisites

- Visual Studio 2022 (MSVC 14.4x and Windows SDK 10.0.26100)
- LLVM `clang-cl` 19 or later, with `lld-link`, installed in `C:\Program Files\LLVM` (otherwise edit
  `CMakePresets.json`)
- CMake 3.25 or later and Ninja in `PATH`
- Python 3.11 or later, with `pytest` for the tests
- git, with submodules
- Network access at configure time, to download the AMD FidelityFX DLLs (optional, see below)

### One command

From the repository root:

```
git submodule update --init --recursive
python tools/gow2.py build --iso "path\to\your\GoW2.iso"
```

The script runs, in order: configure, build of the `extract` tool, extraction to `game_data/` (skipped if
`game_data/.extract_ok` exists, unless `--force`), the `recompile` target (XenonRecomp), a reconfigure and the build of
`gow2.exe`. It stops at the first failing step. The result is `build\clang-release\runtime\gow2.exe`; run from the
repository root it uses `.\game_data`.

`gow2.py` finds `vcvars64.bat` (`vswhere -latest`) and passes the MSVC environment to every step, so no "x64 Native
Tools" prompt is needed. For manual commands, use `python tools/gow2.py shell` (a `cmd.exe` with the same environment)
or `python tools/gow2.py shell <command>`. Do not mix MSVC installations in the same `build/clang-release`: the
configure step refuses it; delete `build/clang-release` in that case.

### Manual build

```
cmake --preset clang-release
cmake --build --preset clang-release --target recompile   # needs game_data/default.xex
cmake --preset clang-release                              # the gow2 target exists only after recompile
cmake --build --preset clang-release --target gow2
```

Always build `gow2.exe` with `--target gow2`: the default build does not relink it. The `recompile` target writes the
C++ to `build/generated/` and the log to `build/recomp.log`, and fails if `tools/check_recomp_log.py` finds errors,
an incomplete log or unresolved calls.

### Optional components

- **DLSS**: the bridge `gow2_dlss.dll` (MIT sources in `runtime/gpu/dlss_bridge/`) is built only with the NVIDIA DLSS
  SDK, which is not in the repository. Clone https://github.com/NVIDIA/DLSS at tag `v310.9.16` (`git clone --depth 1
  --branch v310.9.16`, about 880 MB) and pass it with `-DGOW2_DLSS_SDK_DIR=<checkout>` or the `GOW2_DLSS_SDK_DIR`
  environment variable. The build then copies `gow2_dlss.dll`, `nvngx_dlss.dll`, `nvngx_dlssg.dll` and the licenses
  next to `gow2.exe`. If the variable is not set the build succeeds without the bridge and DLSS is unavailable; if it
  points to something that is not the SDK, configure prints a warning and does the same. The bridge keeps NVIDIA's
  SDK out of the GPL-3.0 executable: `gow2.exe` loads it at runtime through a C ABI with no NVIDIA types.
- **AMD FidelityFX (FSR)**: only the MIT API headers are in `thirdparty/fidelityfx/`. The configure step downloads the
  signed AMD DLLs of FidelityFX SDK v2.3.0, checks their SHA-256 and the build copies them next to `gow2.exe`. Offline:
  `-DGOW2_FIDELITYFX_DIR=<folder with the DLLs>`. Without them the build succeeds and FSR is unavailable.

### Third-party code in the tree

- `thirdparty/XenonRecomp` is a submodule pinned at upstream commit `ddd128b`. The patches in `patches/xenonrecomp/`
  (missing PPC instructions, the `PPC_HOST` address macro, a 50 MHz `mftb` timebase, conditional `bclr`/`bcctr`
  returns) are applied by the configure step (`cmake/XenonRecompPatches.cmake`), so `git status` shows the submodule
  as modified: that is expected. If a patch neither applies nor is already applied, reset the submodule with
  `git -C thirdparty/XenonRecomp checkout -- .` and configure again.
- `thirdparty/xenia/`: GPU and APU files of xenia-canary (commit `e6cd2e4`), copied unchanged by
  `tools/vendor_xenia.py`; the only changes are the patches in `thirdparty/xenia/patches/` (each hunk marked `gow2:`).
  See `thirdparty/xenia/VENDORED.md`.
- `thirdparty/ffmpeg/`: the libavcodec/libavutil subset needed by the XMA decoder, from the has207/FFmpeg fork used by
  xenia-canary (commit `d980192`), copied by `tools/vendor_ffmpeg.py`. See `thirdparty/ffmpeg/VENDORED.md`.
- `thirdparty/nvapi/`: a minimal NVAPI extract to read the monitor's VRR state; `nvapi64.dll` of the driver is loaded at
  runtime only if present.

## Tests

```
ctest --preset clang-release-fast   # short suite, about 70 s
ctest --preset clang-release        # long suite, about 35 minutes
```

- The short suite runs all C++ tests and the Python tests without the `gioco` marker (`gioco` is Italian for "game";
  `lungo`, below, means "long").
- The long suite adds the tests labelled `lungo`: `pytest_gioco` (Python tests that run `gow2.exe` with the game data
  for 20-120 s each) and "real ISO". Use it before a merge or a release.
- By hand: `python -m pytest tests -m "not gioco"` (short) and `python -m pytest tests -m gioco` (game tests only).

Tests that need the game data, the ISO, a D3D12 adapter or a Xenia checkout are skipped when those are missing.

## Legal

- This repository contains no copyrighted material of the game: no ISO, XEX, assets or recompiled code. The TOML/JSON files in
  `games/<id>/` (`gow2.toml`, `gow2_switch_tables.toml`, `expected_xex.json` and the same for GoW3) contain only addresses and metadata obtained by analysing the executable (function boundaries, switch
  tables, the expected hash), a common practice in recompilation projects such as Unleashed Recomp.
- The **release** contains `gow2.exe`, which includes code recompiled from the game's executable. It contains no game
  assets: to play you need your own copy of Gears of War 2 (Xbox 360, PAL Europe), from which the launcher extracts
  the game data on your computer.
- The source code is licensed under GPL-3.0. The released binary links that GPL-3.0 code (part of it derived from
  Unleashed Recomp) with the recompiled game code, whose source is not published. This is the same situation as
  Unleashed Recomp's releases; it is stated here openly.
- NVIDIA's DLSS DLLs and OptiScaler are not distributed by this project: you download them yourself from their
  official sources, under their own licenses. Whether a proprietary DLL loaded at runtime by a GPL program is allowed
  is an interpretation, not legal advice.
- Requests from the rights holders will be honoured: if Microsoft, Xbox Game Studios or Epic Games ask for the project
  or the release to be changed or taken down, it will be.

## Credits

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (hedge-dev): the static recompiler.
- [Unleashed Recomp](https://github.com/hedge-dev/UnleashedRecomp) (hedge-dev): parts of the runtime are derived from
  it or follow its design.
- [Xenia](https://github.com/xenia-project/xenia) and [xenia-canary](https://github.com/xenia-canary/xenia-canary):
  GPU command processor, shader translation, XMA decoding, and the reference for the kernel semantics.
- [FFmpeg](https://ffmpeg.org/) (has207 fork), [SDL](https://www.libsdl.org/), [Dear ImGui](https://github.com/ocornut/imgui),
  [toml++](https://github.com/marzer/tomlplusplus), [o1heap](https://github.com/pavel-kirienko/o1heap),
  [{fmt}](https://github.com/fmtlib/fmt), [xxHash](https://github.com/Cyan4973/xxHash).
- AMD FidelityFX (FSR, CAS), NVIDIA DLSS and NVAPI, NVIDIA FXAA.
- [OptiScaler](https://github.com/optiscaler/OptiScaler) and janblade's DLSSNR-F5 fork.
- Game patches from [xenia-canary/game-patches](https://github.com/xenia-canary/game-patches)
  (`4D53082D - Gears of War 2.patch.toml`), rewritten in this project: FPS counter (Sowa_95), no motion blur
  (illusion), no lens flares and no bloom (boma), FPS unlock (illusion and boma), 21:9 and 16:10 aspect ratio
  (boma and TudorHorse), Coalesced hash check bypass (Sowa_95 and boma).

## License

The project is licensed under the **GNU General Public License v3.0** (`LICENSE`). Exceptions:
`runtime/gpu/dlss_bridge/` is MIT (`runtime/gpu/dlss_bridge/LICENSE.txt`); third-party code keeps its own license.
`NOTICE` lists the files derived from other projects; `THIRD_PARTY_NOTICES.md` lists every third-party component, its
license and its attribution. Each copied or adapted file states its origin project, path and commit in its header.
