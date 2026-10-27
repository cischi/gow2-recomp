"""Consistency between the [[midasm_hook]] entries of config/gow2.toml and the C++ patch table."""
import re
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Jumps beyond +4 verified on the disassembly; exact target.
JUMPS = {"Gow2PatchNoConstrainAspect": 0x8252D69C, "Gow2PatchUwUiHeight": 0x826E372C}


def test_midasm_hooks_match_the_table():
    toml = tomllib.loads((ROOT / "config" / "gow2.toml").read_text(encoding="utf-8"))
    hooks = {h["name"]: h for h in toml.get("midasm_hook", [])}
    cpp = (ROOT / "runtime" / "game_patches" / "game_patches.cpp").read_text(encoding="utf-8")
    pairs = re.findall(r'\{"(Gow2Patch\w+)",\s*(0x[0-9A-Fa-f]+)', cpp)
    assert pairs, "C++ table empty or format changed"
    table = {}
    for n, a in pairs:  # a repeated name (hook shared by several entries) always has the same address
        assert table.setdefault(n, int(a, 16)) == int(a, 16), n
    assert {n: h["address"] for n, h in hooks.items()} == table
    for n, h in hooks.items():
        if h.get("return_on_true"):  # blr at the start of the function
            continue
        assert h["jump_address_on_true"] == JUMPS.get(n, h["address"] + 4), n


def test_aspect_hooks():
    text = (ROOT / "config" / "gow2.toml").read_text(encoding="utf-8")
    hooks = tomllib.loads(text)["midasm_hook"]
    names = [h["name"] for h in hooks]
    for n in ("Gow2PatchNoConstrainAspect", "Gow2PatchUwFov", "Gow2PatchSceneHeight", "Gow2PatchUwUiHeight"):
        assert names.count(n) == 1, n
    # Removed together with the tile tables.
    for n in ("Gow2PatchUwNoTiling", "Gow2PatchUwSceneHeight", "Gow2PatchSceneWidth1610", "Gow2PatchFov1610"):
        assert n not in text, n
    # XenonRecomp allows only one hook per address: that is why the scene height is a shared hook.
    assert len({h["address"] for h in hooks}) == len(hooks)
    assert next(h for h in hooks if h["name"] == "Gow2PatchUwFov")["registers"] == ["f0", "f30"]
    assert "Gow2Spike" not in text
