"""Checks xexdump on the real default.xex against games/gow2/expected_xex.json.

The expected values were derived independently of XenonUtils (XEX2 optional
headers 0x00010100 and 0x000103FF).

xexdump.exe is looked for in build/clang-release/tools/xexdump/ (clang-release
preset); the GOW2_XEXDUMP environment variable can point to another
executable. The test is skipped if game_data/default.xex is missing.
"""

import json
import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
XEX = ROOT / "game_data" / "default.xex"
EXPECTED = ROOT / "games" / "gow2" / "expected_xex.json"


def _xexdump_exe() -> Path:
    override = os.environ.get("GOW2_XEXDUMP")
    if override:
        return Path(override)
    return ROOT / "build" / "clang-release" / "tools" / "xexdump" / "xexdump.exe"


@pytest.fixture(scope="module")
def dump(tmp_path_factory):
    if not XEX.is_file():
        pytest.skip(f"{XEX} missing (extract the disc first)")
    exe = _xexdump_exe()
    assert exe.is_file(), f"xexdump not found: {exe} (build the clang-release preset)"
    out = tmp_path_factory.mktemp("xex out [x]")
    proc = subprocess.run([str(exe), str(XEX), str(out)], capture_output=True, text=True)
    assert proc.returncode == 0, f"xexdump exit {proc.returncode}: {proc.stderr}"
    info = json.loads((out / "info.json").read_text(encoding="utf-8"))
    return info, out / "image.bin"


def test_entry_matches_expected(dump):
    info, _ = dump
    expected = json.loads(EXPECTED.read_text(encoding="utf-8"))
    assert info["entry"] == expected["entry"]


def test_import_count_matches_expected(dump):
    info, _ = dump
    expected = json.loads(EXPECTED.read_text(encoding="utf-8"))
    assert len(info["imports"]) == expected["import_count"]
    for imp in info["imports"]:
        assert isinstance(imp["module"], str) and imp["module"]
        assert isinstance(imp["name"], str) and imp["name"]
        assert info["base"] <= imp["thunk"] < info["base"] + info["size"]


def test_entry_inside_image(dump):
    info, _ = dump
    assert info["base"] <= info["entry"] < info["base"] + info["size"]


def test_image_bin_is_pe_of_declared_size(dump):
    info, image = dump
    data = image.read_bytes()
    assert len(data) == info["size"]
    assert data[:2] == b"MZ"


def test_missing_input_fails_cleanly(tmp_path):
    exe = _xexdump_exe()
    if not exe.is_file():
        pytest.skip(f"xexdump not found: {exe} (cmake --build --preset clang-release --target xexdump)")
    proc = subprocess.run([str(exe), str(tmp_path / "does not exist.xex"), str(tmp_path / "out")],
                          capture_output=True, text=True)
    assert proc.returncode == 1
    assert proc.stderr.strip()


def test_garbage_input_fails_cleanly(tmp_path):
    exe = _xexdump_exe()
    if not exe.is_file():
        pytest.skip(f"xexdump not found: {exe} (cmake --build --preset clang-release --target xexdump)")
    bad = tmp_path / "bad.xex"
    bad.write_bytes(b"XEX2" + b"\0" * 60)
    proc = subprocess.run([str(exe), str(bad), str(tmp_path / "out")], capture_output=True, text=True)
    assert proc.returncode == 1
    assert proc.stderr.strip()
