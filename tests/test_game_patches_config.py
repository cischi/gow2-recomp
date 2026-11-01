"""Consistency between the [[midasm_hook]] entries of games/<id>/<id>.toml and the C++ patch table of each game."""
import re
import tomllib
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent

# Hook name prefix per game and the jumps beyond +4, verified on the disassembly; exact target.
GAMES = {
    "gow2": ("Gow2Patch", {"Gow2PatchNoConstrainAspect": 0x8252D69C, "Gow2PatchUwUiHeight": 0x826E372C}),
    "gow3": ("Gow3Patch", {"Gow3PatchNoConstrainAspect": 0x8261BEF0}),
}


def _hooks(game):
    return tomllib.loads((ROOT / "games" / game / f"{game}.toml").read_text(encoding="utf-8")).get("midasm_hook", [])


@pytest.mark.parametrize("game", GAMES)
def test_midasm_hooks_match_the_table(game):
    prefix, jumps = GAMES[game]
    hooks = {h["name"]: h for h in _hooks(game)}
    cpp = (ROOT / "games" / game / "patches.cpp").read_text(encoding="utf-8")
    pairs = re.findall(r'\{"(' + prefix + r'\w+)",\s*(0x[0-9A-Fa-f]+)', cpp)
    assert pairs, "C++ table empty or format changed"
    table = {}
    for n, a in pairs:  # a repeated name (hook shared by several entries) always has the same address
        assert table.setdefault(n, int(a, 16)) == int(a, 16), n
    assert {n: h["address"] for n, h in hooks.items()} == table
    for n, h in hooks.items():
        assert n.startswith(prefix), n
        if h.get("return_on_true"):  # blr at the start of the function
            continue
        assert h["jump_address_on_true"] == jumps.get(n, h["address"] + 4), n
    # Every hook is defined in the game's patches.cpp (the generated code declares it extern).
    for n in hooks:
        assert re.search(r"\bbool " + n + r"\(", cpp), n


@pytest.mark.parametrize("game", GAMES)
def test_one_hook_per_address(game):
    # XenonRecomp allows only one hook per address: that is why the scene height is a shared hook.
    hooks = _hooks(game)
    assert len({h["address"] for h in hooks}) == len(hooks)


def test_aspect_hooks():
    text = (ROOT / "games" / "gow2" / "gow2.toml").read_text(encoding="utf-8")
    hooks = tomllib.loads(text)["midasm_hook"]
    names = [h["name"] for h in hooks]
    for n in ("Gow2PatchNoConstrainAspect", "Gow2PatchUwFov", "Gow2PatchSceneHeight", "Gow2PatchUwUiHeight"):
        assert names.count(n) == 1, n
    # Removed together with the tile tables.
    for n in ("Gow2PatchUwNoTiling", "Gow2PatchUwSceneHeight", "Gow2PatchSceneWidth1610", "Gow2PatchFov1610"):
        assert n not in text, n
    assert next(h for h in hooks if h["name"] == "Gow2PatchUwFov")["registers"] == ["f0", "f30"]
    assert "Gow2Spike" not in text


def test_gow3_aspect_hooks():
    hooks = {h["name"]: h for h in _hooks("gow3")}
    # FOV in f31 at this site (f2 = f31 * f0 right after the lfs).
    assert hooks["Gow3PatchUwFov"]["registers"] == ["f0", "f31"]
    for n in ("Gow3PatchNoConstrainAspect", "Gow3PatchSceneHeight", "Gow3PatchWidthReference",
              "Gow3PatchUwBufferHeight", "Gow3PatchUwUiScaleMode"):
        assert n in hooks, n
