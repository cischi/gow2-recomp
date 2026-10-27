"""Milestone of the 0xE view sub-project: "New Game" up to loading.

Command: gow2.exe --run-seconds 120 --audio null --input-script
"40:START+200,46:A+200,50:A+200,54:A+200,58:A+200,62:A+200,66:A+200"
(title, menu, campaign, difficulty), with --data on the game data and
--user-data in a temporary folder, timeout 200 s.

Criteria: exit 0; no line that starts with `crash`; no `device removed`;
no `INDIRECT RINGBUFFER: Failed`; `[summary] host_frames_presented` >= 1800;
`[summary] cooked_packages_opened` >= 28 (a sign that the difficulty was chosen
and the campaign level is loading: title and menu open 11 cooked packages,
both without a script and with START alone). With local sockets (2026-10-20) the
game opens 34 packages in 120 s, among them the character
content (e.g. GUD_COGMarcusG6_SF.xxx), and at 90 and 115 s it shows the campaign's
opening movie; with socket() failing ("Error initializing network
layer.") it opened 18 and the scene stayed black. The threshold 28 is 6 below the
measured value (stable over three runs) and 10 above the broken case. A
threshold on the count instead of package names or addresses: it tolerates small
changes in order and timing.

Skipped without game_data/default.xex, without gow2.exe or without a hardware D3D12 adapter
(`gow2.exe --gpu-probe`). Log and user_data stay in the tmp_path.
The test prints the measured values.
"""

import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
GAME_DATA = ROOT / "game_data"
XEX = GAME_DATA / "default.xex"
RUN_SECONDS = 120
TIMEOUT_S = 200
INPUT_SCRIPT = "40:START+200,46:A+200,50:A+200,54:A+200,58:A+200,62:A+200,66:A+200"
MIN_FRAMES = 1800
MENU_COOKED_PACKAGES = 11  # title and menu (control without a script and with START only)
MIN_COOKED_PACKAGES = 28   # campaign loaded: 34 measured; without sockets 18


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


def _summary(log: str, key: str) -> int:
    prefix = f"[summary] {key}="
    values = [line[len(prefix):] for line in log.splitlines() if line.startswith(prefix)]
    assert len(values) == 1, f"{prefix} missing or repeated:\n{log[-4000:]}"
    return int(values[0])


@pytest.mark.gioco
def test_milestone_new_game(tmp_path):
    if not XEX.is_file():
        pytest.skip(f"{XEX} missing: extract the disc into game_data/")
    exe = _gow2_exe()
    _require_adapter(exe)
    run_dir = tmp_path / "run"
    run_dir.mkdir()
    args = [str(exe), "--data", str(GAME_DATA), "--user-data", str(run_dir / "user_data"),
            "--run-seconds", str(RUN_SECONDS), "--audio", "null", "--input-script", INPUT_SCRIPT]
    log_path = run_dir / "gow2.log"
    try:
        proc = subprocess.run(args, cwd=run_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        partial = log_path.read_text(encoding="utf-8", errors="replace") if log_path.is_file() else "(gow2.log missing)"
        pytest.fail(f"gow2.exe did not exit within {TIMEOUT_S} s:\n{partial[-4000:]}")
    assert log_path.is_file(), f"gow2.log missing (exit {proc.returncode})"
    log = log_path.read_text(encoding="utf-8", errors="replace")
    tail = log[-4000:]
    lines = log.splitlines()

    frames = _summary(log, "host_frames_presented")
    cooked = _summary(log, "cooked_packages_opened")
    last = [l for l in lines if l.startswith("[summary] last_package ")]
    print(f"exit={proc.returncode} host_frames_presented={frames} cooked_packages_opened={cooked} {last}")

    assert proc.returncode == 0, f"exit {proc.returncode}:\n{tail}"
    assert not [l for l in lines if l.startswith("crash")], f"crash line in the log:\n{tail}"
    assert not [l for l in lines if "device removed" in l], f"device removed in the log:\n{tail}"
    assert not [l for l in lines if "INDIRECT RINGBUFFER: Failed" in l], f"INDIRECT RINGBUFFER: Failed:\n{tail}"
    assert frames >= MIN_FRAMES, f"frames presented {frames} < {MIN_FRAMES}:\n{tail}"
    assert cooked >= MIN_COOKED_PACKAGES, (
        f"cooked packages opened {cooked} < {MIN_COOKED_PACKAGES} (title and menu open {MENU_COOKED_PACKAGES}): "
        f"did the script fail to get past the difficulty choice?\n{tail}")
