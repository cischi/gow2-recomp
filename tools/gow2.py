#!/usr/bin/env python3
"""Pipeline orchestrator: extract -> configure -> recompile -> gow2.

    python tools/gow2.py build --iso <DVD.iso> [--force]

MSVC environment: clang-cl needs INCLUDE/LIB. If they are already set
("x64 Native Tools" prompt) the current environment is used; otherwise
vcvars64.bat is located (vswhere -latest, then the standard VS 2022 Community path). The same
environment is available for manual commands with `python tools/gow2.py shell [command]`.
The environment is captured ONCE and passed to every subprocess.

Every step is a subprocess.run(check=False): if it fails the step name
and the exit code are printed and the script stops with exit != 0.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRESET = "clang-release"
BUILD_DIR = ROOT / "build" / PRESET
GAME_DATA = ROOT / "game_data"
EXTRACT_EXE = BUILD_DIR / "tools" / "extract" / "extract.exe"
GOW2_EXE = BUILD_DIR / "runtime" / "gow2.exe"
EXTRACT_MARKER = ".extract_ok"  # written by RunExtract as its last action
RUN_HINT = (f'Run (from the repository root): "{GOW2_EXE}" [--data <folder with default.xex>] [--user-data <folder>] [--trace] '
            "[--run-seconds N] [--language <code>] [--gpu fake|d3d12] [--screenshot S:file] "
            "[--gpu-cvar name=value]... [--gpu-probe] [--audio xaudio2|null] [--mute] "
            "[--audio-capture <file.wav>] [--xma-decoder new|old] "
            "[--input-script \"<s>:<BUTTON>[+<ms>],...\"] [--config <file>] "
            "[--window windowed|borderless|fullscreen] [--resolution WxH] [--vsync on|off] "
            "[--frame-rate 30|60|120|144|monitor|unlimited] [--vrr auto|on|off] "
            "[--frame-generation off|fsr|dlss] [--aspect 16:9|21:9|16:10] [--hdr on|off|force] "
            "[--hdr-peak-nits <n>] [--hdr-paper-white-nits <n>] [--shadow-quality low|original|high] "
            "[--dynamic-shadows on|off] [--aa off|fxaa|fxaa_extreme] [--sharpening off|0.0..1.0] "
            "[--internal-resolution 1|2|3] [--anisotropic 0|2|4|8|16] [--enhanced-shaders on|off] "
            "[--upscaler off|fsr|dlss] [--dlss-preset default|e|f|j|k|l|m] "
            "[--dlss-mode auto|dlaa|quality|balanced|performance|ultra_performance] "
            "[--skip-movie <name>]... [--launcher]")
DEFAULT_VCVARS = Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat")


def find_vcvars() -> Path | None:
    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.is_file():
        proc = subprocess.run([str(vswhere), "-latest", "-products", "*", "-property", "installationPath"],
                              capture_output=True, text=True, check=False)
        path = proc.stdout.strip()
        if proc.returncode == 0 and path:
            cand = Path(path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if cand.is_file():
                return cand
    return DEFAULT_VCVARS if DEFAULT_VCVARS.is_file() else None


def extraction_complete(game_data: Path) -> bool:
    """True only if the previous extraction finished: marker present and default.xex exists."""
    return (game_data / EXTRACT_MARKER).is_file() and (game_data / "default.xex").is_file()


def merge_env(base: dict[str, str], set_output: str) -> dict[str, str]:
    """Merges the output of `set` (after vcvars) into `base`.

    Keys are compared case-insensitively (Windows: `Path` and `PATH` are the
    same variable): the vcvars value wins. Pseudo-variables `=C:=...` are
    ignored; the value starts after the first `=` (it may contain more).
    """
    env = {k.upper(): v for k, v in base.items()}
    for line in set_output.splitlines():
        key, sep, value = line.partition("=")
        if sep and key:
            env[key.upper()] = value
    if not env.get("INCLUDE"):
        raise RuntimeError("INCLUDE is not set after vcvars64.bat")
    return env


def msvc_env() -> dict[str, str]:
    env = dict(os.environ)
    if env.get("INCLUDE") and env.get("LIB"):
        return env
    vcvars = find_vcvars()
    if vcvars is None:
        raise RuntimeError("vcvars64.bat not found: install Visual Studio 2022 "
                           "or start from the 'x64 Native Tools Command Prompt'")
    proc = subprocess.run(f'cmd /c "chcp 65001 >nul && call "{vcvars}" >nul && set"', capture_output=True,
                          text=True, encoding="utf-8", errors="replace", check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"vcvars64.bat failed (exit {proc.returncode}): {vcvars}")
    return merge_env(env, proc.stdout)


def run_step(name: str, cmd: list[str], env: dict[str, str]) -> int:
    print(f"\n=== {name} ===\n$ {' '.join(cmd)}", flush=True)
    rc = subprocess.run(cmd, cwd=ROOT, env=env, check=False).returncode
    if rc != 0:
        print(f"error: step '{name}' failed with exit code {rc}", file=sys.stderr)
    return rc


def cmd_build(args: argparse.Namespace) -> int:
    iso = Path(args.iso)
    if not iso.is_file():
        print(f"error: ISO not found: {iso}", file=sys.stderr)
        return 1
    try:
        env = msvc_env()
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    cmake = "cmake"
    build = [cmake, "--build", "--preset", PRESET]
    steps: list[tuple[str, list[str]]] = [
        ("configure", [cmake, "--preset", PRESET]),
        ("build tools (extract)", build + ["--target", "extract"]),
    ]
    for name, cmd in steps:
        if (rc := run_step(name, cmd, env)) != 0:
            return rc

    if extraction_complete(GAME_DATA) and not args.force:
        print(f"\n=== extract === skipped: game_data is complete ({EXTRACT_MARKER} present; use --force)")
    else:
        if (rc := run_step("extract", [str(EXTRACT_EXE), str(iso), str(GAME_DATA)], env)) != 0:
            return rc

    steps = [
        ("recompile", build + ["--target", "recompile"]),
        # gow2 only exists if build/generated is there at configure time: reconfigure
        ("reconfigure", [cmake, "--preset", PRESET]),
        ("build gow2", build + ["--target", "gow2"]),
    ]
    for name, cmd in steps:
        if (rc := run_step(name, cmd, env)) != 0:
            return rc

    if not GOW2_EXE.is_file():
        print(f"error: gow2.exe not found after the build: {GOW2_EXE}", file=sys.stderr)
        return 1
    print(f"\nDone. Executable: {GOW2_EXE}\n{RUN_HINT}")
    return 0


def cmd_shell(args: argparse.Namespace) -> int:
    """Runs a command (or opens cmd.exe) in the same MSVC environment used by `build`."""
    try:
        env = msvc_env()
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    cmd = args.command_line or [os.environ.get("COMSPEC", "cmd.exe")]
    return subprocess.run(cmd, cwd=ROOT, env=env, check=False).returncode


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="gow2.py", description="Gears of War 2 PC build pipeline")
    sub = parser.add_subparsers(dest="command", required=True)
    b = sub.add_parser("build", help="extract -> configure -> recompile -> build gow2.exe")
    b.add_argument("--iso", required=True, help="path to the Gears of War 2 disc (ISO)")
    b.add_argument("--force", action="store_true", help="repeat the extraction even if game_data is complete")
    b.set_defaults(func=cmd_build)
    sh = sub.add_parser("shell", help="run a command (default: cmd.exe) with the same MSVC environment as build")
    sh.add_argument("command_line", nargs=argparse.REMAINDER, help="command to run, e.g. cmake --preset clang-release")
    sh.set_defaults(func=cmd_shell)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
