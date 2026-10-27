"""Milestone of sub-project 2: gow2.exe --run-seconds 60 with the extracted data.

Criterion: exit 0 after 60 s without `crash`
lines in the log, at least one .xxx package opened from CookedXenon, at least 60 frames
presented. The user data (--user-data) lives in the test's tmp_path.
Skipped without game_data/default.xex or without gow2.exe.
"""

import os
import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
GAME_DATA = ROOT / "game_data"
XEX = GAME_DATA / "default.xex"
RUN_SECONDS = 60
TIMEOUT_S = 120

pytestmark = pytest.mark.skipif(not XEX.is_file(), reason=f"{XEX} missing (extract the disc first)")


def _gow2_exe() -> Path:
    override = os.environ.get("GOW2_EXE")
    exe = Path(override) if override else ROOT / "build" / "clang-release" / "runtime" / "gow2.exe"
    if not exe.is_file():
        pytest.skip(f"gow2.exe not built ({exe}): run `python tools/gow2.py build --iso <iso>`")
    return exe


@pytest.mark.gioco
def test_milestone_run_60_seconds(tmp_path):
    exe = _gow2_exe()
    # user_data in tmp_path: no saves/caches shared between runs
    proc = subprocess.run([str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "user_data"),
                           "--run-seconds", str(RUN_SECONDS), "--gpu", "fake"],
                          cwd=tmp_path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                          timeout=TIMEOUT_S)
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    tail = log[-4000:]
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{tail}"
    lines = log.splitlines()
    assert not any(l.startswith("crash") for l in lines), f"crash line in the log:\n{tail}"

    opened = next((int(m.group(1)) for l in lines if (m := re.fullmatch(r"\[summary\] packages_opened=(\d+)", l))), 0)
    assert opened >= 1, f"no package opened:\n{tail}"
    cooked = next((int(m.group(1)) for l in lines
                   if (m := re.fullmatch(r"\[summary\] cooked_packages_opened=(\d+)", l))), 0)
    assert cooked >= 1, f"no .xxx under GearGame/CookedXenon:\n{tail}"

    frames = next((int(m.group(1)) for l in lines if (m := re.fullmatch(r"\[summary\] frames_presented=(\d+)", l))), 0)
    assert frames >= 60, f"frames presented {frames} < 60:\n{tail}"
