"""Tests of tools/vendor_xenia.py and of the vendored tree thirdparty/xenia/.

The tests on the tree always run; the one that rebuilds the tree (--check)
needs a Xenia checkout @95a5c3e (env GOW2_XENIA_REF, default
../_refs/xenia relative to the repository root) and is skipped otherwise.
"""
import importlib.util
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / "tools" / "vendor_xenia.py"
VENDOR = REPO / "thirdparty" / "xenia"
# A single vendored tree (xenia-canary, GPU and APU); the structural tests are
# parametrized on the tree so that others can be added.
TREES = pytest.mark.parametrize("vendor", [VENDOR], ids=["xenia"])
SHIMS = REPO / "runtime" / "gpu" / "shims"
APU_SHIMS = REPO / "runtime" / "apu" / "shims"


def _load():
    spec = importlib.util.spec_from_file_location("vendor_xenia", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _manifest(vendor: Path = VENDOR) -> list[str]:
    lines = (vendor / "manifest.txt").read_text(encoding="utf-8").splitlines()
    return [l.strip() for l in lines if l.strip() and not l.strip().startswith("#")]


def _vendored_files(vendor: Path = VENDOR) -> list[str]:
    out = []
    for top in ("src", "third_party"):
        for p in (vendor / top).rglob("*"):
            if p.is_file():
                out.append(p.relative_to(vendor).as_posix())
    return sorted(out)


@TREES
def test_manifest_is_sorted_and_unique(vendor):
    m = _manifest(vendor)
    assert m == sorted(set(m))


@TREES
def test_tree_matches_manifest_except_derived_shaders(vendor):
    # The shader sources are added by the tool following the bytecode
    # includes (shader_sources); no bytecode is vendored.
    files = set(_vendored_files(vendor))
    assert {p for p in files if "/shaders/bytecode/" in p} == set()
    manifest = set(_manifest(vendor))
    texts = {rel: (vendor / rel).read_text(encoding="utf-8", errors="replace") for rel in manifest}
    if any(rel.startswith("src/xenia/gpu/") for rel in manifest):
        texts.update({p.name: p.read_text(encoding="utf-8") for p in (REPO / "runtime" / "gpu").glob("*.cpp")})
    read = lambda rel: (vendor / rel).read_bytes() if (vendor / rel).is_file() else None
    derived = set(_load().shader_sources(texts, read))
    assert files == manifest | derived


NO_HEADER_UPSTREAM = {
    "src/xenia/gpu/pm4_command_processor_declare.h",
    "src/xenia/gpu/pm4_command_processor_implement.h",
}


@TREES
def test_vendored_sources_keep_the_bsd_header(vendor):
    missing = []
    for rel in _vendored_files(vendor):
        # third_party/: own licenses (see VENDORED.md); canary's PM4 fragments,
        # included inside classes, have no header even
        # in canary.
        if rel.startswith("third_party/") or rel in NO_HEADER_UPSTREAM:
            continue
        if not rel.endswith((".h", ".cc")):
            continue
        if "Released under the BSD license" not in (vendor / rel).read_text(encoding="utf-8"):
            missing.append(rel)
    assert missing == []


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


@TREES
def test_every_patch_is_documented_and_every_hunk_is_marked(vendor):
    vendored_md = (vendor / "VENDORED.md").read_text(encoding="utf-8")
    for patch in sorted((vendor / "patches").glob("*.patch")):
        assert patch.name in vendored_md, patch.name
        hunks = _hunks(patch.read_text(encoding="utf-8"))
        assert hunks, patch.name
        for h in hunks:
            assert "gow2:" in h, f"{patch.name}: hunk without 'gow2:'\n{h}"


@TREES
def test_vendored_tree_and_patches_have_lf_line_endings(vendor):
    # Xenia @95a5c3e's blobs are LF; git apply with core.autocrlf=true
    # (Git for Windows' default) would write CRLF in the patched files and the
    # tree would depend on the configuration of whoever regenerates it.
    assert (vendor / "manifest.txt").is_file()
    with_cr = [p.relative_to(vendor).as_posix()
               for top in ("src", "third_party", "patches")
               for p in (vendor / top).rglob("*")
               if p.is_file() and b"\r" in p.read_bytes()]
    assert with_cr == []


def test_hunk_splitter():
    text = ("diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b // gow2: y\n"
            "@@ -5 +5 @@\n-c\n+d\n")
    assert _hunks(text) == ["@@ -1 +1 @@\n-a\n+b // gow2: y", "@@ -5 +5 @@\n-c\n+d"]


def _tree_paths(vendor: Path) -> set[str]:
    """Vendored files as seen by an #include (src/ removed)."""
    return {rel.removeprefix("src/") for rel in _vendored_files(vendor)}


def test_apu_is_in_the_canary_tree():
    # The APU is in the same tree as the GPU (same xenia-canary commit): the old
    # thirdparty/xenia-apu folder (Xenia @95a5c3e) no longer exists.
    assert not (REPO / "thirdparty" / "xenia-apu").exists()
    paths = _tree_paths(VENDOR)
    for rel in ("xenia/apu/xma_context.cc", "xenia/apu/xma_context_new.cc", "xenia/apu/xma_context_old.cc",
                "xenia/apu/xma_register_file.cc", "xenia/base/bit_stream.cc"):
        assert rel in paths, rel


def test_no_path_is_both_vendored_and_shimmed():
    vendored = _tree_paths(VENDOR)
    gpu_shims = {p.relative_to(SHIMS).as_posix() for p in SHIMS.rglob("*") if p.is_file()}
    apu_shims = {p.relative_to(APU_SHIMS).as_posix() for p in APU_SHIMS.rglob("*") if p.is_file()}
    assert gpu_shims, "no GPU shim found"
    assert apu_shims, "no APU shim found"
    assert vendored & gpu_shims == set()
    assert vendored & apu_shims == set()
    # An APU shim does not shadow a GPU shim (runtime/apu/shims comes first in the includes).
    assert gpu_shims & apu_shims == set()


def test_shader_sources_follow_bytecode_includes():
    mod = _load()
    sources = {
        "src/xenia/gpu/d3d12/a.cc": (
            '#include "xenia/gpu/d3d12/a.h"\n'
            '#include "xenia/gpu/shaders/bytecode/d3d12_5_1/resolve_full_32bpp_cs.h"\n'
            '  #include  "xenia/gpu/shaders/bytecode/d3d12_5_1/adaptive_quad_hs.h"\n'
            '// #include "xenia/gpu/shaders/bytecode/vulkan_spirv/x_cs.h"\n'
            '#include <xenia/gpu/shaders/bytecode/d3d12_5_1/angle_ps.h>\n'),
        "runtime/gpu/presenter.cpp": (
            '#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_bilinear_ps.h"\n'),
    }
    files = {
        "src/xenia/gpu/shaders/resolve_full_32bpp.cs.xesl": b'#include "resolve.xesli"\n',
        "src/xenia/gpu/shaders/resolve.xesli": b'#include "../../ui/shaders/xesl.xesli"\n',
        "src/xenia/ui/shaders/xesl.xesli": b"// base\n",
        "src/xenia/gpu/shaders/adaptive_quad.hs.hlsl": b"// hs\n",
        "src/xenia/ui/shaders/guest_output_bilinear.ps.xesl": b'#include "xesl.xesli"\n',
        "src/xenia/gpu/shaders/unused.xesli": b"// not included\n",
    }
    assert mod.shader_sources(sources, files.get) == {
        rel: files[rel] for rel in (
            "src/xenia/gpu/shaders/adaptive_quad.hs.hlsl",
            "src/xenia/gpu/shaders/resolve.xesli",
            "src/xenia/gpu/shaders/resolve_full_32bpp.cs.xesl",
            "src/xenia/ui/shaders/guest_output_bilinear.ps.xesl",
            "src/xenia/ui/shaders/xesl.xesli",
        )
    }


def test_shader_sources_reject_a_missing_source():
    mod = _load()
    sources = {"a.cc": '#include "xenia/gpu/shaders/bytecode/d3d12_5_1/missing_cs.h"\n'}
    with pytest.raises(SystemExit, match="missing_cs"):
        mod.shader_sources(sources, {}.get)


def _xenia_ref(vendor: Path = VENDOR) -> Path:
    """Checkout of the repository in the manifest header of `vendor`:
    ../_refs/<name> (xenia-canary), or GOW2_XENIA_REF / GOW2_XENIA_CANARY_REF."""
    repo, _ = _load().read_manifest_header(vendor / "manifest.txt")
    name = repo.rstrip("/").rsplit("/", 1)[-1]
    env = "GOW2_XENIA_CANARY_REF" if name == "xenia-canary" else "GOW2_XENIA_REF"
    return Path(os.environ.get(env, str(REPO.parent / "_refs" / name)))


@TREES
def test_check_rebuilds_the_tree_byte_for_byte(vendor):
    ref = _xenia_ref(vendor)
    if not (ref / ".git").exists():
        pytest.skip(f"Xenia checkout missing: {ref}")
    proc = subprocess.run([sys.executable, str(SCRIPT), "--xenia", str(ref), "--out", str(vendor), "--check"],
                          capture_output=True, text=True, check=False)
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_wrong_commit_exits_2(tmp_path):
    repo = tmp_path / "fake"
    repo.mkdir()
    git = ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t"]
    subprocess.run(["git", "init", "-q", str(repo)], check=True)
    (repo / "f").write_text("x")
    subprocess.run(git + ["add", "f"], check=True)
    subprocess.run(git + ["commit", "-q", "-m", "x"], check=True)
    proc = subprocess.run([sys.executable, str(SCRIPT), "--xenia", str(repo), "--check"],
                          capture_output=True, text=True, check=False)
    assert proc.returncode == 2, proc.stdout + proc.stderr
    _, commit = _load().read_manifest_header(VENDOR / "manifest.txt")
    assert commit in proc.stderr


def test_check_reports_a_modified_file(tmp_path):
    ref = _xenia_ref()
    if not (ref / ".git").exists():
        pytest.skip(f"Xenia checkout missing: {ref}")
    import shutil
    out = tmp_path / "xenia"
    shutil.copytree(VENDOR, out)
    target = out / "src" / "xenia" / "gpu" / "xenos.h"
    target.write_bytes(target.read_bytes() + b"// manual edit\n")
    (out / "src" / "xenia" / "gpu" / "extra.h").write_text("x")
    proc = subprocess.run([sys.executable, str(SCRIPT), "--xenia", str(ref), "--out", str(out), "--check"],
                          capture_output=True, text=True, check=False)
    assert proc.returncode == 1, proc.stdout + proc.stderr
    assert "different: src/xenia/gpu/xenos.h" in proc.stdout
    assert "extra: src/xenia/gpu/extra.h" in proc.stdout


def test_manifest_header_names_repo_and_commit():
    mod = _load()
    assert mod.read_manifest_header(VENDOR / "manifest.txt") == (
        "https://github.com/xenia-canary/xenia-canary", "e6cd2e4bc998609ca25e24a24c8e32b66c5908ad")


def test_manifest_without_header_exits_2(tmp_path, capsys):
    out = tmp_path / "xenia"
    out.mkdir()
    (out / "manifest.txt").write_text("src/xenia/gpu/xenos.h\n", encoding="utf-8")
    assert _load().main(["--xenia", str(tmp_path), "--out", str(out), "--check"]) == 2
    assert "header" in capsys.readouterr().err


def test_manifest_not_utf8_exits_2(tmp_path, capsys):
    out = tmp_path / "xenia"
    out.mkdir()
    (out / "manifest.txt").write_bytes(b"# repo: x\n# commit: \xff\xfe\n")
    assert _load().main(["--xenia", str(tmp_path), "--out", str(out), "--check"]) == 2
    assert "manifest.txt" in capsys.readouterr().err


def test_shader_without_stage_exits_2(tmp_path, capsys, monkeypatch):
    """A data error (shader without a stage) during the rebuild: exit 2 with the message, no traceback."""
    mod = _load()
    out = tmp_path / "xenia"
    out.mkdir()
    (out / "manifest.txt").write_text("# repo: x\n# commit: " + "a" * 40 + "\n", encoding="utf-8")
    monkeypatch.setattr(mod, "head_commit", lambda _: "a" * 40)

    def bad_tree(*_):
        mod.shader_stage("xenos_wrong")

    monkeypatch.setattr(mod, "build_tree", bad_tree)
    assert mod.main(["--xenia", str(tmp_path), "--out", str(out), "--check"]) == 2
    assert "without a stage" in capsys.readouterr().err


def test_read_blob_follows_submodules():
    # The presenter's CAS shaders include headers of the
    # third_party/FidelityFX-CAS submodule: they are read at the commit recorded in the gitlink.
    ref = _xenia_ref()
    if not (ref / "third_party" / "FidelityFX-CAS" / ".git").exists():
        pytest.skip(f"FidelityFX-CAS submodule missing in {ref}")
    _, commit = _load().read_manifest_header(VENDOR / "manifest.txt")
    data = _load().read_blob(ref, "third_party/FidelityFX-CAS/ffx-cas/ffx_a.h", commit)
    assert b"A_GPU" in data
