"""Tests of tools/find_helpers.py on synthetic images (no game bytes)."""
import importlib.util
import json
import struct
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / "tools" / "find_helpers.py"

BASE = 0x82000000

# big-endian words for each helper; stvx128 v64,r11,r12 / lvx128 v64,r11,r12
STVX128 = 0x100B61CB
LVX128 = 0x100B60CB
PATTERNS = {
    "savegprlr_14_address": [0xF9C1FF68],
    "restgprlr_14_address": [0xE9C1FF68],
    # the FPR saves address through r12, not r1 (verified on the real image)
    "savefpr_14_address": [0xD9CCFF70],
    "restfpr_14_address": [0xC9CCFF70],
    "savevmx_14_address": [0x3960FEE0, 0x7DCB61CE],
    "restvmx_14_address": [0x3960FEE0, 0x7DCB60CE],
    "savevmx_64_address": [0x3960FC00, STVX128],
    "restvmx_64_address": [0x3960FC00, LVX128],
}


def _load():
    spec = importlib.util.spec_from_file_location("find_helpers", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _image(extra=()):
    img = bytearray(0x1000)
    offsets = {}
    off = 0x100
    for key, words in PATTERNS.items():
        offsets[key] = off
        struct.pack_into(">%dI" % len(words), img, off, *words)
        off += 0x100
    for off, words in extra:
        struct.pack_into(">%dI" % len(words), img, off, *words)
    return bytes(img), offsets


def test_finds_every_helper():
    img, offsets = _image()
    found = _load().find_helpers(img, BASE)
    assert found == {k: BASE + v for k, v in offsets.items()}


def test_duplicate_pattern_raises_with_key():
    img, _ = _image(extra=[(0xF00, [0xF9C1FF68])])
    with pytest.raises(ValueError, match="savegprlr_14_address"):
        _load().find_helpers(img, BASE)


def test_missing_pattern_raises_with_key():
    img, offsets = _image()
    img = bytearray(img)
    struct.pack_into(">I", img, offsets["restfpr_14_address"], 0)
    with pytest.raises(ValueError, match="restfpr_14_address"):
        _load().find_helpers(bytes(img), BASE)


def test_unaligned_match_is_ignored():
    # the same word at an offset not aligned to 4 is not an instruction
    img, offsets = _image()
    img = bytearray(img)
    img[0xF01:0xF05] = struct.pack(">I", 0xF9C1FF68)
    found = _load().find_helpers(bytes(img), BASE)
    assert found["savegprlr_14_address"] == BASE + offsets["savegprlr_14_address"]


def test_vmx64_requires_vx128_store_or_load():
    # 3960FC00 followed by an arbitrary word does not count
    img, _ = _image(extra=[(0xF00, [0x3960FC00, 0x60000000])])
    found = _load().find_helpers(img, BASE)
    assert found["savevmx_64_address"] == BASE + 0x700


def test_cli_prints_toml_lines(tmp_path):
    img, offsets = _image()
    image_path = tmp_path / "dir [x] ü" / "image.bin"
    image_path.parent.mkdir()
    image_path.write_bytes(img)
    info_path = image_path.parent / "info.json"
    info_path.write_text(json.dumps({"base": BASE, "size": len(img)}))
    r = subprocess.run([sys.executable, str(SCRIPT), str(image_path), str(info_path)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    lines = r.stdout.strip().splitlines()
    assert lines == [f"{k} = 0x{BASE + offsets[k]:X}" for k in PATTERNS]


def test_cli_missing_pattern_exits_1(tmp_path):
    image_path = tmp_path / "image.bin"
    image_path.write_bytes(bytes(0x100))
    info_path = tmp_path / "info.json"
    info_path.write_text(json.dumps({"base": BASE, "size": 0x100}))
    r = subprocess.run([sys.executable, str(SCRIPT), str(image_path), str(info_path)],
                       capture_output=True, text=True)
    assert r.returncode == 1
    assert "savegprlr_14_address" in r.stderr


CONFIG = REPO / "games" / "gow2" / "gow2.toml"
REAL_IMAGE = REPO / "build" / "xex" / "image.bin"
REAL_INFO = REPO / "build" / "xex" / "info.json"


def test_config_is_complete():
    import tomllib
    main = tomllib.loads(CONFIG.read_text(encoding="utf-8"))["main"]
    for key in PATTERNS:
        assert main[key] != 0, key
    switch_path = CONFIG.parent / main["switch_table_file_path"]
    switches = tomllib.loads(switch_path.read_text(encoding="utf-8"))["switch"]
    assert switches and all(s["labels"] for s in switches)


@pytest.mark.skipif(not (REAL_IMAGE.exists() and REAL_INFO.exists()),
                    reason="build/xex/image.bin missing (run xexdump)")
def test_config_matches_real_image():
    import tomllib
    main = tomllib.loads(CONFIG.read_text(encoding="utf-8"))["main"]
    base = json.loads(REAL_INFO.read_text(encoding="utf-8"))["base"]
    found = _load().find_helpers(REAL_IMAGE.read_bytes(), base)
    assert found == {k: main[k] for k in PATTERNS}
