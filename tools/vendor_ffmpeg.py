#!/usr/bin/env python3
"""Reproducible vendoring of xenia-canary's FFmpeg subset into thirdparty/ffmpeg/.

    python tools/vendor_ffmpeg.py --ffmpeg <fork> --overlay <xenia-canary> [--out thirdparty/ffmpeg] [--check]
    python tools/vendor_ffmpeg.py --ffmpeg <fork> --overlay <xenia-canary> --print-manifest

Sources: fork https://github.com/has207/FFmpeg (submodule third_party/FFmpeg
of xenia-canary, xmaframes branch) @FFMPEG_COMMIT; configuration and component
lists (config.h, config_components.h, *_list.c, libavutil/avconfig.h, ...)
and premake5.lua from xenia-canary's third_party/ffmpeg-xenia overlay
@OVERLAY_COMMIT. LGPL-2.1-or-later. Same rules as tools/vendor_xenia.py,
whose functions it reuses:

- The two checkouts must be at the commits FFMPEG_COMMIT and OVERLAY_COMMIT,
  otherwise exit 2. Files are read from the commits' git objects, not from the
  working tree (the bytes do not depend on the checkout's core.autocrlf). A
  path present in the overlay (except *.lua and libavformat/) comes
  from the overlay, the others from the fork.
- ONLY the paths of <out>/manifest.txt are copied; the patches
  <out>/patches/*.patch are applied in name order with `git apply -p1`
  (core.autocrlf=false) in a temporary folder.
- Without --check the result replaces everything in <out> except manifest.txt,
  VENDORED.md and patches/ (written by hand).
- With --check it rebuilds in a temporary folder and compares byte
  by byte with <out>, excluding VENDORED.md: exit 0 if identical, 1 with
  the list of differences.
- --print-manifest prints the expected manifest, derived from the commits: the `.c` files
  that the overlay's libavcodec/premake5.lua and libavutil/premake5.lua compile
  for Windows x86-64 (files() blocks without a filter or with platforms:Windows;
  no .S/.asm; ../../FFmpeg/ prefix removed), the headers reached by their
  `#include "..."` (include_closure: resolved relative to the file's folder,
  then to the root, then to compat/atomics/win32; paths of other
  architectures excluded), the overlay's configuration files, plus EXTRA_FILES.
"""

from __future__ import annotations

import argparse
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vendor_xenia as vx  # noqa: E402

FFMPEG_COMMIT = "d980192e175e6ff95bcd287af77e16fcb6597974"
OVERLAY_COMMIT = "e6cd2e4bc998609ca25e24a24c8e32b66c5908ad"
OVERLAY_DIR = "third_party/ffmpeg-xenia"
PREMAKE_PREFIX = "../../FFmpeg/"
ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = ROOT / "thirdparty" / "ffmpeg"
# Files written by hand inside <out>; everything else is produced by the script.
HAND_WRITTEN = {"manifest.txt", "VENDORED.md"}
HAND_WRITTEN_DIRS = {"patches"}
CHECK_EXCLUDE = {"VENDORED.md"}

PREMAKE_LIBS = ("libavcodec", "libavutil")
INCLUDE_DIRS = ("", "libavcodec", "compat/atomics/win32")
EXTRA_FILES = ("COPYING.LGPLv2.1", "LICENSE.md", "compat/atomics/win32/stdatomic.h")

# libavutil/macos_kperf.h is GPL (not LGPL): timer.h only includes it with CONFIG_MACOS_KPERF, which
# canary's configuration does not define (0).
_EXCLUDED = re.compile(
    r"(^|/)(aarch64|arm|mips|ppc|loongarch|alpha|bfin|sh4|sparc|avr32|tomi|riscv|e2k)/"
    r"|^config_(?!components\.h$)"
    r"|^libavutil/macos_kperf\.h$")
_QUOTED_INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', re.MULTILINE)
_FILTER = re.compile(r"^\s*filter\((.*)\)\s*$")
_FILES_ITEM = re.compile(r'^\s*"([^"]+)",?\s*$')


def premake_windows_sources(text: str) -> list[str]:
    """`.c` files of the files({...}) blocks of a fork premake5.lua that apply
    to Windows: outside a platform filter() or with platforms:Windows
    in the filter. Paths relative to the file's folder, in order."""
    out: list[str] = []
    platform_ok = True
    in_files = False
    for line in text.splitlines():
        if in_files:
            if line.strip().startswith("})"):
                in_files = False
                continue
            m = _FILES_ITEM.match(line)
            if m and platform_ok and m.group(1).endswith(".c"):
                out.append(m.group(1))
            continue
        m = _FILTER.match(line)
        if m:
            arg = m.group(1)
            platform_ok = "platforms:" not in arg or "platforms:Windows" in arg
            continue
        if line.strip().startswith("files({"):
            in_files = True
    return out


def include_closure(roots: list[str], read: Callable[[str], str | None]) -> list[str]:
    """Closure of `roots` over the `#include "..."` (also those under #if: it is
    an overestimate), with files read through `read(rel)` (None = absent).
    Unresolved or excluded (_EXCLUDED) includes are ignored. Sorted."""
    seen: set[str] = set()
    todo = list(roots)
    while todo:
        rel = todo.pop()
        if rel in seen:
            continue
        text = read(rel)
        if text is None:
            continue
        seen.add(rel)
        base = posixpath.dirname(rel)
        for name in _QUOTED_INCLUDE.findall(text):
            for d in (base, *INCLUDE_DIRS):
                cand = posixpath.normpath(posixpath.join(d, name)) if d else posixpath.normpath(name)
                if cand.startswith("..") or _EXCLUDED.search(cand):
                    continue
                if cand in seen or read(cand) is not None:
                    todo.append(cand)
                    break
    return sorted(seen)


def _ls_tree(checkout: Path, commit: str, path: str = "") -> list[str]:
    proc = vx._git(checkout, "ls-tree", "-r", "--name-only", commit, *([path] if path else []))
    if proc.returncode != 0:
        raise SystemExit(f"git ls-tree failed: {proc.stderr.decode(errors='replace')}")
    return proc.stdout.decode().splitlines()


def overlay_files(overlay: Path) -> list[str]:
    """Configuration files of the overlay (paths relative to the FFmpeg root): all except the
    premake5.lua files and libavformat/ (not built)."""
    prefix = OVERLAY_DIR + "/"
    rels = [p[len(prefix):] for p in _ls_tree(overlay, OVERLAY_COMMIT, OVERLAY_DIR)]
    return sorted(r for r in rels if not r.endswith(".lua") and not r.startswith("libavformat/"))


class _Sources:
    """Blob reading: overlay first (overlay_files only), then fork."""

    def __init__(self, ffmpeg: Path, overlay: Path):
        self.ffmpeg, self.overlay = ffmpeg, overlay
        self.fork = set(_ls_tree(ffmpeg, FFMPEG_COMMIT))
        self.over = set(overlay_files(overlay))
        self.cache: dict[str, str | None] = {}

    def blob(self, rel: str) -> bytes:
        if rel in self.over:
            return vx.read_blob(self.overlay, f"{OVERLAY_DIR}/{rel}", OVERLAY_COMMIT)
        return vx.read_blob(self.ffmpeg, rel, FFMPEG_COMMIT)

    def lua(self, rel: str) -> str:
        return vx.read_blob(self.overlay, f"{OVERLAY_DIR}/{rel}", OVERLAY_COMMIT).decode("utf-8")

    def read(self, rel: str) -> str | None:
        if rel not in self.cache:
            known = rel in self.over or rel in self.fork
            self.cache[rel] = self.blob(rel).decode("utf-8", errors="replace") if known else None
        return self.cache[rel]


def expected_manifest(ffmpeg: Path, overlay: Path) -> list[str]:
    src = _Sources(ffmpeg, overlay)
    sources = []
    for lib in PREMAKE_LIBS:
        for rel in premake_windows_sources(src.lua(f"{lib}/premake5.lua")):
            sources.append(rel[len(PREMAKE_PREFIX):] if rel.startswith(PREMAKE_PREFIX) else f"{lib}/{rel}")
    closure = include_closure(sources + ["compat/atomics/win32/stdatomic.h"], src.read)
    return sorted(set(closure) | src.over | set(EXTRA_FILES))


def build_tree(ffmpeg: Path, overlay: Path, out: Path, dest: Path) -> list[str]:
    manifest = vx.read_manifest(out / "manifest.txt")
    src = _Sources(ffmpeg, overlay)
    vx.write_blobs(dest, {rel: src.blob(rel) for rel in manifest})
    vx.apply_patches(out, dest)
    return manifest


def install_tree(built: Path, out: Path) -> None:
    for p in out.iterdir():
        if p.name in HAND_WRITTEN or p.name in HAND_WRITTEN_DIRS:
            continue
        if p.is_dir():
            shutil.rmtree(p)
        else:
            p.unlink()
    for p in built.iterdir():
        if p.name in HAND_WRITTEN or p.name in HAND_WRITTEN_DIRS:
            continue
        if p.is_dir():
            shutil.copytree(p, out / p.name)
        else:
            shutil.copy2(p, out / p.name)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ffmpeg", required=True, type=Path, help="checkout of the has207/FFmpeg fork")
    ap.add_argument("--overlay", required=True, type=Path,
                    help="xenia-canary checkout (third_party/ffmpeg-xenia configuration overlay)")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="default: thirdparty/ffmpeg")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                      help="rebuilds in a temporary folder and compares with --out")
    mode.add_argument("--print-manifest", action="store_true",
                      help="prints the manifest derived from the fork's premake5.lua files")
    args = ap.parse_args(argv)

    for checkout, commit in ((args.ffmpeg, FFMPEG_COMMIT), (args.overlay, OVERLAY_COMMIT)):
        head = vx.head_commit(checkout)
        if head != commit:
            print(f"{checkout}: HEAD={head or '?'}, expected {commit}", file=sys.stderr)
            return 2
    if args.print_manifest:
        print(f"# Generated by tools/vendor_ffmpeg.py --print-manifest "
              f"(fork @{FFMPEG_COMMIT[:8]}, overlay xenia-canary @{OVERLAY_COMMIT[:8]})")
        for rel in expected_manifest(args.ffmpeg, args.overlay):
            print(rel)
        return 0
    out = args.out.resolve()
    if not (out / "manifest.txt").is_file():
        print(f"{out / 'manifest.txt'}: manifest missing", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="gow2_vendor_ffmpeg_") as tmp:
        built = Path(tmp)
        copied = build_tree(args.ffmpeg, args.overlay, out, built)
        if args.check:
            diffs = vx.compare_trees(built, out, CHECK_EXCLUDE)
            for d in diffs:
                print(d)
            if diffs:
                print(f"{len(diffs)} differences: rerun without --check", file=sys.stderr)
                return 1
            print(f"ok: {len(copied)} files identical to {FFMPEG_COMMIT[:7]} + patches")
            return 0
        install_tree(built, out)
        print(f"vendored {len(copied)} files in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
