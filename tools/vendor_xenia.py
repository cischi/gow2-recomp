#!/usr/bin/env python3
"""Reproducible vendoring of Xenia files into thirdparty/xenia/ (or --out).

    python tools/vendor_xenia.py --xenia <checkout> [--out thirdparty/xenia] [--check]

- Repository and commit are at the top of <out>/manifest.txt (lines
  `# repo: <url>` and `# commit: <sha40>`, read_manifest_header); without the
  header exit 2. The checkout must be at that commit
  (`git rev-parse HEAD`), otherwise exit 2. Files are read from the commit's git objects (not from the
  working tree), so the bytes do not depend on the checkout's core.autocrlf.
- ONLY the paths of <out>/manifest.txt are copied (one per line, sorted;
  empty lines and `#` ignored), plus LICENSE, plus the sources of the D3D12
  shaders (shader_sources): for every `#include` of a header
  `xenia/<gpu|ui>/shaders/bytecode/d3d12_5_1/<id>.h` in the copied files or in
  runtime/gpu/, the source `<base>.<stage>.xesl` (or `.hlsl`) and the files it
  includes. The bytecode headers are not vendored: the build generates them with
  fxc (cmake/XeniaShaders.cmake), as xenia-canary does.
- The patches <out>/patches/*.patch are applied in name order with
  `git apply -p1`, in a temporary folder outside any repository (git
  apply inside a subtree of a repository would ignore the paths), with
  core.autocrlf=false: files stay LF like the blobs.
- Without --check the result replaces src/ and third_party/ in <out>
  (files no longer produced are deleted) and LICENSE.
- With --check it rebuilds in a temporary folder and compares byte
  by byte with <out>, excluding VENDORED.md and LICENSE: exit 0 if identical,
  1 with the list of differences.
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

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = ROOT / "thirdparty" / "xenia"
# Folders produced by the script inside <out>; the rest (manifest, patches,
# VENDORED.md) is written by hand.
TREE_DIRS = ("src", "third_party")
CHECK_EXCLUDE = {"VENDORED.md", "LICENSE"}

_BYTECODE_INCLUDE = re.compile(
    r'^[ \t]*#[ \t]*include[ \t]*"xenia/(gpu|ui)/shaders/bytecode/d3d12_5_1/([^"/]+)\.h"',
    re.MULTILINE)
_QUOTED_INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', re.MULTILINE)
# C/C++ comments (FXAA3_11.h has an example #include in a comment).
_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)
SHADER_STAGES = ("vs", "hs", "ds", "gs", "ps", "cs")
# Sources of Xenia's bytecode headers (see cmake/XeniaShaders.cmake).
RUNTIME_SHADER_USERS = ROOT / "runtime" / "gpu"


def shader_stage(shader_id: str) -> str:
    """Stage of a Xenia shader identifier (`<base>_<stage>`,
    as in xenia-build); ValueError if it has no valid one."""
    base, sep, stage = shader_id.rpartition("_")
    if not sep or not base or stage not in SHADER_STAGES:
        raise ValueError(f"{shader_id}: shader identifier without a stage ({' '.join(SHADER_STAGES)})")
    return stage


def shader_sources(sources: dict[str, str], read) -> dict[str, bytes]:
    """Sources of the D3D12 shaders (path `src/...` -> bytes) for the bytecode
    headers included with `#include "..."` by the given sources (path ->
    text): `<base>.<stage>.xesl` or `.hlsl` and, transitively, the files they include
    (paths relative to the file). `read(rel)` gives the bytes or None if absent."""
    found: dict[str, bytes] = {}
    pending = []
    for text in sources.values():
        for m in _BYTECODE_INCLUDE.finditer(text):
            component, shader_id = m.groups()
            stage = shader_stage(shader_id)
            base = f"src/xenia/{component}/shaders/{shader_id[:-len(stage) - 1]}.{stage}"
            for rel in (base + ".xesl", base + ".hlsl"):
                data = read(rel)
                if data is not None:
                    pending.append((rel, data))
                    break
            else:
                raise SystemExit(f"{shader_id}: source missing ({base}.xesl/.hlsl)")
    while pending:
        rel, data = pending.pop()
        if rel in found:
            continue
        found[rel] = data
        for m in _QUOTED_INCLUDE.finditer(_COMMENT.sub("", data.decode("utf-8", errors="replace"))):
            inc = posixpath.normpath(posixpath.join(posixpath.dirname(rel), m.group(1)))
            if inc not in found:
                inc_data = read(inc)
                if inc_data is None:
                    raise SystemExit(f"{rel}: include missing: {inc}")
                pending.append((inc, inc_data))
    return dict(sorted(found.items()))


_HEADER = re.compile(r"^#[ \t]*(repo|commit):[ \t]*(\S+)[ \t]*$")


def read_manifest_header(path: Path) -> tuple[str, str]:
    """(repository, commit) from the `# repo:` and `# commit:` lines of the manifest;
    ValueError if they are missing or if the commit is not a 40-digit sha."""
    found = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = _HEADER.match(line.strip())
        if m:
            found.setdefault(m.group(1), m.group(2))
    if "repo" not in found or "commit" not in found:
        raise ValueError(f"{path}: `# repo:`/`# commit:` header missing")
    if not re.fullmatch(r"[0-9a-f]{40}", found["commit"]):
        raise ValueError(f"{path}: `# commit:` header is not a 40-digit sha")
    return found["repo"], found["commit"]


def read_manifest(path: Path) -> list[str]:
    entries = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            entries.append(line)
    if entries != sorted(set(entries)):
        raise SystemExit(f"{path}: the manifest must be sorted and without duplicates")
    for e in entries:
        if e.startswith("/") or ".." in Path(e).parts or "\\" in e:
            raise SystemExit(f"{path}: invalid path: {e}")
    return entries


def _git(xenia: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(xenia), *args], capture_output=True, check=False)


def head_commit(xenia: Path) -> str | None:
    proc = _git(xenia, "rev-parse", "HEAD")
    if proc.returncode != 0:
        return None
    return proc.stdout.decode().strip()


def _submodule(checkout: Path, rel: str, commit: str) -> tuple[Path, str, str] | None:
    """(submodule checkout, gitlink commit, path inside) if
    `rel` is in a submodule registered in `commit`, otherwise None."""
    parts = rel.split("/")
    for i in range(1, len(parts)):
        prefix = "/".join(parts[:i])
        proc = _git(checkout, "ls-tree", commit, prefix)
        fields = proc.stdout.decode().split()
        if proc.returncode != 0 or len(fields) < 3:
            return None
        if fields[1] == "commit":
            return checkout / prefix, fields[2], "/".join(parts[i:])
        if fields[1] != "tree":
            return None
    return None


def read_blob(checkout: Path, rel: str, commit: str, missing_ok: bool = False) -> bytes | None:
    proc = _git(checkout, "show", f"{commit}:{rel}")
    if proc.returncode != 0:
        # File of a submodule (the FidelityFX headers of the CAS shaders): it is
        # read from its checkout, at the gitlink commit.
        sub = _submodule(checkout, rel, commit)
        if sub is not None and (sub[0] / ".git").exists():
            return read_blob(sub[0], sub[2], sub[1], missing_ok)
        if missing_ok:
            return None
        raise SystemExit(f"{rel}: missing in commit {commit}: "
                         f"{proc.stderr.decode(errors='replace').strip()}")
    return proc.stdout


def write_blobs(dest: Path, blobs: dict[str, bytes]) -> None:
    for rel, data in blobs.items():
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)


def build_tree(xenia: Path, out: Path, dest: Path, commit: str) -> list[str]:
    """Writes into `dest` (empty folder) the manifest, patches, LICENSE and the
    vendored files with the patches applied. Returns the list of copied files."""
    manifest = read_manifest(out / "manifest.txt")
    blobs = {rel: read_blob(xenia, rel, commit) for rel in manifest}
    texts = {rel: data.decode("utf-8", errors="replace") for rel, data in blobs.items()}
    # gow2's presenter uses the xenia/ui shaders: they only matter for the
    # GPU folder (the APU one has no gpu/ files).
    if any(rel.startswith("src/xenia/gpu/") for rel in manifest):
        for user in sorted(RUNTIME_SHADER_USERS.glob("*.cpp")):
            texts[user.relative_to(ROOT).as_posix()] = user.read_text(encoding="utf-8")
    shaders = shader_sources(texts, lambda rel: read_blob(xenia, rel, commit, missing_ok=True))
    for rel, data in shaders.items():
        blobs.setdefault(rel, data)
    write_blobs(dest, blobs)
    (dest / "LICENSE").write_bytes(read_blob(xenia, "LICENSE", commit))
    apply_patches(out, dest)
    return sorted(blobs)


def apply_patches(out: Path, dest: Path) -> None:
    """Copies the manifest and patches of `out` into `dest` and applies the patches, in
    name order, to the files already written in `dest`."""
    shutil.copy2(out / "manifest.txt", dest / "manifest.txt")
    patches = sorted((out / "patches").glob("*.patch")) if (out / "patches").is_dir() else []
    if patches:
        (dest / "patches").mkdir()
    for patch in patches:
        shutil.copy2(patch, dest / "patches" / patch.name)
        # core.autocrlf=false: patched files stay LF like the blobs (with
        # Git for Windows' default git apply would write them CRLF).
        proc = subprocess.run(["git", "-c", "core.autocrlf=false", "apply", "-p1",
                               "--whitespace=nowarn", str(patch)],
                              cwd=dest, capture_output=True, text=True, check=False)
        if proc.returncode != 0:
            raise SystemExit(f"{patch.name}: git apply failed:\n{proc.stderr}")


def _files(base: Path, exclude: set[str]) -> dict[str, Path]:
    out = {}
    for p in base.rglob("*"):
        if p.is_file():
            rel = p.relative_to(base).as_posix()
            if rel not in exclude:
                out[rel] = p
    return out


def compare_trees(expected: Path, actual: Path, exclude: set[str] = CHECK_EXCLUDE) -> list[str]:
    exp, act = _files(expected, exclude), _files(actual, exclude)
    diffs = []
    for rel in sorted(exp.keys() | act.keys()):
        if rel not in act:
            diffs.append(f"missing: {rel}")
        elif rel not in exp:
            diffs.append(f"extra: {rel}")
        elif exp[rel].read_bytes() != act[rel].read_bytes():
            diffs.append(f"different: {rel}")
    return diffs


def install_tree(built: Path, out: Path) -> None:
    for top in TREE_DIRS:
        if (out / top).exists():
            shutil.rmtree(out / top)
        if (built / top).exists():
            shutil.copytree(built / top, out / top)
    shutil.copy2(built / "LICENSE", out / "LICENSE")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--xenia", required=True, type=Path, help="Xenia checkout")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="default: thirdparty/xenia")
    ap.add_argument("--check", action="store_true",
                    help="rebuilds in a temporary folder and compares with --out")
    args = ap.parse_args(argv)

    out = args.out.resolve()
    if not (out / "manifest.txt").is_file():
        print(f"{out / 'manifest.txt'}: manifest missing", file=sys.stderr)
        return 2
    try:
        _, commit = read_manifest_header(out / "manifest.txt")
    except UnicodeDecodeError as e:
        print(f"{out / 'manifest.txt'}: not UTF-8 ({e.reason})", file=sys.stderr)
        return 2
    except ValueError as e:
        print(e, file=sys.stderr)
        return 2
    head = head_commit(args.xenia)
    if head != commit:
        print(f"{args.xenia}: HEAD={head or '?'}, expected {commit}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="gow2_vendor_xenia_") as tmp:
        built = Path(tmp)
        try:
            copied = build_tree(args.xenia, out, built, commit)
        except (ValueError, UnicodeDecodeError) as e:  # invalid data (shader without a stage, non-UTF-8 file)
            print(e, file=sys.stderr)
            return 2
        if args.check:
            diffs = compare_trees(built, out)
            for d in diffs:
                print(d)
            if diffs:
                print(f"{len(diffs)} differences: rerun without --check", file=sys.stderr)
                return 1
            print(f"ok: {len(copied)} files identical to {commit[:7]} + patches")
            return 0
        install_tree(built, out)
        print(f"vendored {len(copied)} files in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
