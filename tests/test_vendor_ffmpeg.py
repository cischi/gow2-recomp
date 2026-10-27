"""Tests of tools/vendor_ffmpeg.py and of the vendored tree thirdparty/ffmpeg/.

The tests on the tree always run; those that rebuild the tree
(--check) need the checkout of the has207/FFmpeg fork @d980192 (env
GOW2_FFMPEG_REF, default ../_refs/ffmpeg-has207 relative to the repository
root) and the one of xenia-canary @e6cd2e4 with the third_party/ffmpeg-xenia
configuration overlay (env GOW2_XENIA_CANARY_REF, default
../_refs/xenia-canary); otherwise they are skipped.
"""
import importlib.util
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / "tools" / "vendor_ffmpeg.py"
VENDOR = REPO / "thirdparty" / "ffmpeg"
COMMIT = "d980192e175e6ff95bcd287af77e16fcb6597974"
OVERLAY_COMMIT = "e6cd2e4bc998609ca25e24a24c8e32b66c5908ad"
HAND_WRITTEN = {"manifest.txt", "VENDORED.md"}


def _load():
    spec = importlib.util.spec_from_file_location("vendor_ffmpeg", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _manifest() -> list[str]:
    lines = (VENDOR / "manifest.txt").read_text(encoding="utf-8").splitlines()
    return [l.strip() for l in lines if l.strip() and not l.strip().startswith("#")]


def _vendored_files() -> list[str]:
    out = []
    for p in VENDOR.rglob("*"):
        if not p.is_file():
            continue
        rel = p.relative_to(VENDOR).as_posix()
        if rel in HAND_WRITTEN or rel.startswith("patches/"):
            continue
        out.append(rel)
    return sorted(out)


def test_manifest_is_sorted_and_unique():
    m = _manifest()
    assert m == sorted(set(m))


def test_tree_matches_manifest():
    assert _vendored_files() == _manifest()


def test_manifest_has_the_expected_shape():
    m = set(_manifest())
    for required in ("COPYING.LGPLv2.1", "LICENSE.md", "config.h", "config_components.h",
                     "compat/atomics/win32/stdatomic.h", "libavcodec/wmaprodec.c",
                     "libavcodec/codec_list.c", "libavutil/tx.c"):
        assert required in m, required
    # No assembly (x86asm is disabled), no other architectures.
    assert not [e for e in m if e.endswith((".asm", ".S"))]
    assert not [e for e in m if re.search(r"/(aarch64|arm|mips|ppc|loongarch|alpha)/", e)]
    assert not [e for e in m if e.startswith("config_") and e != "config_components.h"]


def test_license_files_are_present():
    assert (VENDOR / "COPYING.LGPLv2.1").is_file()
    assert (VENDOR / "LICENSE.md").is_file()


def test_config_disables_gpl_and_nonfree():
    # config.h of canary's overlay: LGPL license, no GPL or non-free part enabled
    # (CONFIG_GPL/NONFREE/GPLV3 left undefined count as 0 in #if).
    config = (VENDOR / "config.h").read_text(encoding="utf-8")
    config += (VENDOR / "config_components.h").read_text(encoding="utf-8")
    assert '#define FFMPEG_LICENSE "LGPL version 2.1 or later"' in config
    for flag in ("CONFIG_GPL", "CONFIG_NONFREE", "CONFIG_GPLV3", "CONFIG_VERSION3"):
        assert not re.search(rf"#define {flag} 1\b", config), flag
    assert re.search(r"#define CONFIG_XMAFRAMES_DECODER 1\b", config)
    # x86asm off (no nasm in the build).
    assert re.search(r"#define HAVE_X86ASM 0\b", config)


def test_vendored_sources_are_lgpl_only():
    missing_lgpl, gpl_only = [], []
    for rel in _vendored_files():
        if not rel.endswith((".c", ".h")):
            continue
        text = (VENDOR / rel).read_text(encoding="utf-8", errors="replace")
        flat = " ".join(text.split())
        if "GNU General Public License as published" in flat:
            gpl_only.append(rel)
        if "General Public License" in flat and "GNU Lesser General Public" not in flat:
            missing_lgpl.append(rel)
    assert gpl_only == []
    assert missing_lgpl == []


def _hunks(patch_text: str) -> list[str]:
    hunks, cur = [], None
    for line in patch_text.splitlines():
        if line.startswith("@@"):
            if cur is not None:
                hunks.append("\n".join(cur))
            cur = [line]
        elif line.startswith("diff --git"):
            if cur is not None:
                hunks.append("\n".join(cur))
            cur = None
        elif cur is not None:
            cur.append(line)
    if cur is not None:
        hunks.append("\n".join(cur))
    return hunks


def test_every_patch_is_documented_with_a_date_and_every_hunk_is_marked():
    vendored_md = (VENDOR / "VENDORED.md").read_text(encoding="utf-8")
    patches = sorted((VENDOR / "patches").glob("*.patch")) if (VENDOR / "patches").is_dir() else []
    assert len(patches) <= 10
    for patch in patches:
        rows = [l for l in vendored_md.splitlines() if patch.name in l]
        assert rows, patch.name
        assert any(re.search(r"\d{4}-\d{2}-\d{2}", r) for r in rows), patch.name
        hunks = _hunks(patch.read_text(encoding="utf-8"))
        assert hunks, patch.name
        for h in hunks:
            assert "gow2:" in h, f"{patch.name}: hunk without 'gow2:'\n{h}"


def test_vendored_md_names_origin_and_license():
    md = (VENDOR / "VENDORED.md").read_text(encoding="utf-8")
    assert "https://github.com/has207/FFmpeg" in md
    assert OVERLAY_COMMIT in md
    assert COMMIT in md
    assert "LGPL-2.1" in md


def test_notice_has_the_ffmpeg_entry():
    notice = (REPO / "NOTICE").read_text(encoding="utf-8")
    assert "has207/FFmpeg" in notice
    assert OVERLAY_COMMIT in notice
    assert COMMIT in notice
    assert "LGPL-2.1-or-later" in notice


def test_gitattributes_keeps_vendored_bytes():
    attrs = (REPO / ".gitattributes").read_text(encoding="utf-8")
    assert "thirdparty/ffmpeg/** -text" in attrs


def test_parse_premake_files_keeps_windows_c_sources():
    mod = _load()
    text = (
        'project("libavcodec")\n'
        '  filter("files:not wmaprodec.c")\n    warnings "Off"\n  filter({})\n'
        '  files({\n    "avcodec.h",\n  })\n'
        '  files({\n    "a.c",\n    "b.c",\n  })\n'
        '  filter({"platforms:Windows"})\n  files({\n    "file_open.c",\n  })\n  filter({})\n'
        '  filter({"platforms:Android-ARM64"})\n  files({\n    "aarch64/x.c",\n'
        '    "aarch64/y.S",\n  })\n  filter({})\n'
        '  filter({"platforms:Android-x86_64 or platforms:Linux or platforms:Windows"})\n'
        '  files({\n    "x86/cpu.c",\n  })\n  filter({})\n'
        '  filter({"platforms:Linux"})\n  files({\n    "linux.c",\n  })\n  filter({})\n')
    assert mod.premake_windows_sources(text) == ["a.c", "b.c", "file_open.c", "x86/cpu.c"]


def test_include_closure_follows_quoted_includes(tmp_path):
    mod = _load()
    files = {
        "libavcodec/a.c": '#include "config.h"\n#include "libavutil/b.h"\n#include "local.h"\n'
                          '#include <stdio.h>\n#if ARCH_ARM\n#include "arm/m.h"\n#endif\n',
        "libavcodec/local.h": '#include "libavutil/b.h"\n',
        "libavutil/b.h": '#include "c.h"\n#include "missing.h"\n',
        "libavutil/c.h": "",
        "libavcodec/arm/m.h": "",
        "config.h": "",
    }
    closure = mod.include_closure(["libavcodec/a.c"], lambda rel: files.get(rel))
    assert closure == ["config.h", "libavcodec/a.c", "libavcodec/local.h",
                       "libavutil/b.h", "libavutil/c.h"]


def _ffmpeg_ref() -> Path:
    default = REPO.parent / "_refs" / "ffmpeg-has207"
    return Path(os.environ.get("GOW2_FFMPEG_REF", str(default)))


def _overlay_ref() -> Path:
    default = REPO.parent / "_refs" / "xenia-canary"
    return Path(os.environ.get("GOW2_XENIA_CANARY_REF", str(default)))


def _require_ref() -> list[str]:
    """--ffmpeg/--overlay arguments for the script, or skip if the checkouts are missing."""
    ref, overlay = _ffmpeg_ref(), _overlay_ref()
    for r in (ref, overlay):
        if not (r / ".git").exists():
            pytest.skip(f"reference checkout missing: {r}")
    return ["--ffmpeg", str(ref), "--overlay", str(overlay)]


def test_check_rebuilds_the_tree_byte_for_byte():
    ref = _require_ref()
    proc = subprocess.run([sys.executable, str(SCRIPT), *ref, "--check"],
                          capture_output=True, text=True, check=False)
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_manifest_matches_premake_lists_and_their_includes():
    ref = _require_ref()
    proc = subprocess.run([sys.executable, str(SCRIPT), *ref, "--print-manifest"],
                          capture_output=True, text=True, check=False)
    assert proc.returncode == 0, proc.stderr
    printed = [l for l in proc.stdout.splitlines() if l and not l.startswith("#")]
    assert printed == _manifest()


def test_wrong_commit_exits_2(tmp_path):
    repo = tmp_path / "fake"
    repo.mkdir()
    git = ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t"]
    subprocess.run(["git", "init", "-q", str(repo)], check=True)
    (repo / "f").write_text("x")
    subprocess.run(git + ["add", "f"], check=True)
    subprocess.run(git + ["commit", "-q", "-m", "x"], check=True)
    proc = subprocess.run([sys.executable, str(SCRIPT), "--ffmpeg", str(repo), "--overlay", str(repo), "--check"],
                          capture_output=True, text=True, check=False)
    assert proc.returncode == 2, proc.stdout + proc.stderr
    assert "d980192e" in proc.stderr


def test_check_reports_a_modified_file(tmp_path):
    ref = _require_ref()
    out = tmp_path / "ffmpeg"
    shutil.copytree(VENDOR, out)
    target = out / "libavcodec" / "wmaprodec.c"
    target.write_bytes(target.read_bytes() + b"/* manual edit */\n")
    (out / "libavcodec" / "extra.h").write_text("x")
    proc = subprocess.run([sys.executable, str(SCRIPT), *ref, "--out", str(out), "--check"],
                          capture_output=True, text=True, check=False)
    assert proc.returncode == 1, proc.stdout + proc.stderr
    assert "different: libavcodec/wmaprodec.c" in proc.stdout
    assert "extra: libavcodec/extra.h" in proc.stdout


def test_vendored_tree_has_no_crlf_from_patches():
    # The fork's blobs can contain CR (some upstream files); the patched
    # files must stay like the blobs: with core.autocrlf=true git apply
    # would rewrite them as CRLF. The patches themselves are checked.
    patches = (VENDOR / "patches")
    with_cr = [p.name for p in patches.glob("*.patch") if b"\r" in p.read_bytes()] if patches.is_dir() else []
    assert with_cr == []


def test_overlay_files_come_from_canary():
    # The configuration (config.h, codec lists) is not in the fork: it comes from canary's overlay.
    args = _require_ref()
    overlay = Path(args[3])
    for rel in ("config.h", "config_components.h", "libavcodec/codec_list.c", "libavutil/avconfig.h"):
        blob = subprocess.run(["git", "-C", str(overlay), "show", f"{OVERLAY_COMMIT}:third_party/ffmpeg-xenia/{rel}"],
                              capture_output=True, check=True).stdout
        assert (VENDOR / rel).read_bytes() == blob, rel
        assert rel in _manifest()
