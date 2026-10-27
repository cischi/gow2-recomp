"""Input driver line of gow2.exe: `sdl` between `xinput` and `keyboard`.

Command: gow2.exe --data game_data --user-data <tmp> --run-seconds 5 --audio null.
The `input: driver ...` line of the log must contain, in order, xinput (or its
absence), sdl, keyboard. If SDL does not start the program carries on and logs
`[input] SDL unavailable: <error>`: in that case the test is skipped.
(Both strings are emitted by runtime/main.cpp.)

Skipped without game_data/default.xex, without gow2.exe or without a hardware
D3D12 adapter (`gow2.exe --gpu-probe`). The log stays in tmp_path.
"""

import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
GAME_DATA = ROOT / "game_data"
XEX = GAME_DATA / "default.xex"
TIMEOUT_S = 90


def _gow2_exe() -> Path:
    override = os.environ.get("GOW2_EXE")
    exe = Path(override) if override else ROOT / "build" / "clang-release" / "runtime" / "gow2.exe"
    if not exe.is_file():
        pytest.skip(f"gow2.exe not built ({exe}): run `python tools/gow2.py build --iso <iso>`")
    return exe


def _require_adapter(exe: Path) -> None:
    proc = subprocess.run([str(exe), "--gpu-probe"], capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=60)
    if proc.returncode != 0:
        pytest.skip(f"no hardware D3D12 adapter (--gpu-probe exit {proc.returncode})")


def test_input_driver_line_has_sdl(tmp_path):
    if not XEX.is_file():
        pytest.skip(f"{XEX} missing: extract the disc into game_data/")
    exe = _gow2_exe()
    _require_adapter(exe)
    run_dir = tmp_path / "run"
    run_dir.mkdir()
    args = [str(exe), "--data", str(GAME_DATA), "--user-data", str(run_dir / "user_data"),
            "--run-seconds", "5", "--audio", "null"]
    log_path = run_dir / "gow2.log"
    try:
        proc = subprocess.run(args, cwd=run_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        partial = log_path.read_text(encoding="utf-8", errors="replace") if log_path.is_file() else "(gow2.log missing)"
        pytest.fail(f"gow2.exe did not exit within {TIMEOUT_S} s:\n{partial[-4000:]}")
    assert log_path.is_file(), f"gow2.log missing (exit {proc.returncode})"
    log = log_path.read_text(encoding="utf-8", errors="replace")
    lines = log.splitlines()
    unavailable = [l for l in lines if "[input] SDL unavailable:" in l]
    if unavailable:
        pytest.skip(unavailable[0])
    driver_lines = [l for l in lines if "input: driver" in l]
    assert len(driver_lines) == 1, f"`input: driver` line missing or repeated:\n{log[-4000:]}"
    tokens = driver_lines[0].split("input: driver", 1)[1].split()
    print(driver_lines[0])
    assert "sdl" in tokens, driver_lines[0]
    assert "keyboard" in tokens or "keyboard(windowless)" in tokens, driver_lines[0]
    sdl = tokens.index("sdl")
    assert sdl + 1 < len(tokens) and tokens[sdl + 1].startswith("keyboard"), driver_lines[0]
    if "xinput" in tokens:
        assert tokens.index("xinput") + 1 == sdl, driver_lines[0]
