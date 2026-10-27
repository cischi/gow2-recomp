"""Tests of tools/find_missing_functions.py on a synthetic image (no game bytes)."""
import importlib.util
import json
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / "tools" / "find_missing_functions.py"

BASE = 0x82000000
RDATA = BASE + 0x400
PDATA = BASE + 0x600
TEXT = BASE + 0x1000

BLR = 0x4E800020
BCTR = 0x4E800420
LBZ = 0x88630000      # lbz r3,0(r3)
CLRLWI = 0x5463063E   # clrlwi r3,r3,24
MR = 0x7C631B78       # mr r3,r3
ADDI = 0x38630001     # addi r3,r3,1
SLWI_R0_R3 = 0x5460103A
LWZX = 0x7C0C002E
MTCTR = 0x7C0903A6


def _b(src, dst):
    return 0x48000000 | ((dst - src) & 0x03FFFFFC)


def _load():
    spec = importlib.util.spec_from_file_location("find_missing_functions", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _section(name, vsize, va, flags):
    return struct.pack("<8sIIIIIIHHI", name, vsize, va, vsize, va, 0, 0, 0, 0, flags)


def _image(switch_pdata_size=0):
    """Minimal PE image: .rdata and .pdata (data), .text (code).

    With switch_pdata_size, .pdata also has an entry for the inline switch's
    function (+0x78) that is switch_pdata_size bytes long.

    .text at TEXT:
      +0x00 table of 3 pointers to the getters (+0x0C, +0x1C, +0x2C)
      +0x0C, +0x1C, +0x2C lbz/clrlwi/mr/blr getters of 16 bytes
      +0x3C known function: addi; b +0x4C; [+0x44 switch case] addi; blr;
            +0x4C addi; [+0x50 middle of the function] addi; blr
      +0x58 known function: an instruction whose encoding points to +0x60; blr;
            [+0x60] addi; blr
      +0x68 known function covered by .pdata up to +0x78: addi; blr;
            [+0x70 pointed to by .rdata] addi; blr
      +0x78 known function with an unbounded inline switch: lis/addi/slwi/
            lwzx/mtctr/bctr, table at +0x90 with 2 cases (+0x98, +0xA0)
      +0xA8 known function: blr
    .rdata contains pointers to the case (+0x44), to the middle (+0x50), to the inside of
    .pdata (+0x70) and to a getter (+0x1C).
    """
    img = bytearray(0x2000)
    img[0:2] = b"MZ"
    struct.pack_into("<I", img, 0x3C, 0x40)
    img[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", img, 0x44, 0x1F2, 3, 0, 0, 0, 0, 0)
    img[0x58:0x58 + 40] = _section(b".rdata", 0x100, 0x400, 0x40000040)
    img[0x80:0x80 + 40] = _section(b".pdata", 0x10 if switch_pdata_size else 0x8, 0x600, 0x40000040)
    img[0xA8:0xA8 + 40] = _section(b".text", 0x200, 0x1000, 0x60000020)

    code = [TEXT + 0x0C, TEXT + 0x1C, TEXT + 0x2C]
    for _ in range(3):
        code += [LBZ, CLRLWI, MR, BLR]
    code += [ADDI, _b(TEXT + 0x40, TEXT + 0x4C), ADDI, BLR, ADDI, ADDI, BLR]
    code += [TEXT + 0x60, BLR, ADDI, BLR]
    code += [ADDI, BLR, ADDI, BLR]
    code += [0x3D808200, 0x398C0000 | ((TEXT + 0x90) & 0xFFFF), SLWI_R0_R3, LWZX, MTCTR, BCTR,
             TEXT + 0x98, TEXT + 0xA0, ADDI, BLR, ADDI, BLR]
    code += [BLR]
    struct.pack_into(">%dI" % len(code), img, 0x1000, *code)
    struct.pack_into(">4I", img, 0x400, TEXT + 0x44, TEXT + 0x50, TEXT + 0x70, TEXT + 0x1C)
    struct.pack_into(">II", img, 0x600, TEXT + 0x68, (4 << 8) | 2)
    if switch_pdata_size:
        struct.pack_into(">II", img, 0x608, TEXT + 0x78, ((switch_pdata_size // 4) << 8) | 2)
    return bytes(img)


KNOWN = {TEXT, TEXT + 0x3C, TEXT + 0x58, TEXT + 0x68, TEXT + 0x78, TEXT + 0xA8}
SWITCHES = {TEXT + 0x40: [TEXT + 0x44]}


def test_reports_getters_after_pointer_table():
    inline, found = _load().analyze(_image(), BASE, KNOWN, SWITCHES)
    assert [(c.address, c.size) for c in found] == [
        (TEXT + 0x0C, 0x10), (TEXT + 0x1C, 0x10), (TEXT + 0x2C, 0x10)]


def test_switch_case_is_reported_without_switch_table():
    # control: without the switch table the case (preceded by b) looks like a start
    _, found = _load().analyze(_image(), BASE, KNOWN, {})
    assert TEXT + 0x44 in [c.address for c in found]


def test_middle_of_function_after_plain_instruction_is_not_reported():
    _, found = _load().analyze(_image(), BASE, KNOWN, {})
    assert TEXT + 0x50 not in [c.address for c in found]


def test_lone_instruction_word_and_pdata_interior_are_not_reported():
    _, found = _load().analyze(_image(), BASE, KNOWN, SWITCHES)
    addrs = [c.address for c in found]
    assert TEXT + 0x60 not in addrs  # pointed to only by the encoding of an instruction
    assert TEXT + 0x70 not in addrs  # inside a .pdata function


def test_inline_switch_without_bound_check():
    inline, found = _load().analyze(_image(), BASE, KNOWN, SWITCHES)
    assert [(s.base, s.r, s.labels, s.function, s.end) for s in inline] == [
        (TEXT + 0x78, 3, [TEXT + 0x98, TEXT + 0xA0], TEXT + 0x78, TEXT + 0xA8)]
    assert not {TEXT + 0x98, TEXT + 0xA0} & {c.address for c in found}


def test_switch_function_covered_by_pdata_needs_no_entry():
    # the descent from +0x78 ends (exclusive) at +0xA8; .pdata absent, equal or longer
    mod = _load()
    for size, covered in ((0, False), (0x30, True), (0x34, True), (0x2C, False)):
        img = mod.Image(_image(size), BASE)
        inline, _ = mod.analyze(img, BASE, KNOWN, SWITCHES)
        assert [(s.function, s.end) for s in inline] == [(TEXT + 0x78, TEXT + 0xA8)], size
        assert mod.covered_by_pdata(img, inline[0].function, inline[0].end) == covered, size


def test_cli_prints_toml_lines(tmp_path):
    (tmp_path / "image.bin").write_bytes(_image())
    (tmp_path / "info.json").write_text(json.dumps({"base": BASE, "size": 0x2000}), encoding="utf-8")
    mapping = "PPCFuncMapping PPCFuncMappings[] = {\n" + "".join(
        "\t{ 0x%X, sub_%X },\n" % (a, a) for a in sorted(KNOWN)) + "\t{ 0, nullptr }\n};\n"
    (tmp_path / "map.cpp").write_text(mapping, encoding="utf-8")
    (tmp_path / "sw.toml").write_text(
        "[[switch]]\nbase = 0x%X\nr = 11\ndefault = 0x%X\nlabels = [\n    0x%X,\n]\n"
        % (TEXT + 0x40, TEXT + 0x4C, TEXT + 0x44), encoding="utf-8")
    r = subprocess.run([sys.executable, str(SCRIPT), str(tmp_path / "image.bin"),
                        str(tmp_path / "info.json"), str(tmp_path / "map.cpp"),
                        "--switch-tables", str(tmp_path / "sw.toml")],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    lines = [l for l in r.stdout.splitlines() if l.startswith("    { address")]
    assert lines == [
        "    { address = 0x%X, size = 0x10 }, # after a table of 3 pointers into code at 0x%X"
        % (TEXT + 0x0C, TEXT),
        "    { address = 0x%X, size = 0x10 }, # pointed to by 0x%X, 0x%X" % (TEXT + 0x1C, RDATA + 0xC, TEXT + 0x4),
        "    { address = 0x%X, size = 0x10 }, # pointed to by 0x%X" % (TEXT + 0x2C, TEXT + 0x8),
        "    { address = 0x%X, size = 0x30 }, # inline switch loaded at 0x%X: the function ends"
        " (exclusive) at 0x%X" % (TEXT + 0x78, TEXT + 0x78, TEXT + 0xA8),
    ]
    assert "base = 0x%X\nr = 3\nlabels = [\n    0x%X, 0x%X,\n]" % (TEXT + 0x78, TEXT + 0x98, TEXT + 0xA0) in r.stdout
    assert "inline switches: 1, functions reached only through pointers: 3" in r.stderr
