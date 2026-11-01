"""Milestone of the multi-game sub-project: Gears of War 3 boots to its opening logos.

Command: gow3.exe --data game_data_gow3 --user-data <tmp>/user_data --run-seconds 40
--screenshot <T>:<tmp>/s.bmp, cwd = tmp.
Criterion: exit 0; no line that starts with `crash` or contains `device removed`;
host_frames_presented >= 60; s.bmp is a 24-bit 1280x720 BMP with luminance
standard deviation > 8. T = 16 s: the Open_Logos.bik logos (Epic Games, Microsoft
Studios) are on screen from about 12 to 22 s, measured at the first successful
boot (the title screen follows from about 26 s).
Skipped without game_data_gow3/default.xex, without gow3.exe (override: GOW3_EXE)
or without a hardware D3D12 adapter. Screenshot and log stay in the tmp_path.
"""

import os
import struct
import subprocess
from pathlib import Path

import pytest

from test_milestone_gpu import _require_adapter, _summary, luminance_stddev

ROOT = Path(__file__).resolve().parent.parent
GAME_DATA = ROOT / "game_data_gow3"
XEX = GAME_DATA / "default.xex"
RUN_SECONDS = 40
SCREENSHOT_AT_S = 16  # Open_Logos.bik on screen from ~12 s to ~22 s (measured 2026-10-27)
TIMEOUT_S = 120
WIDTH, HEIGHT = 1280, 720

pytestmark = pytest.mark.skipif(not XEX.is_file(), reason=f"{XEX} missing (extract the GoW3 disc first)")


def _gow3_exe() -> Path:
    override = os.environ.get("GOW3_EXE")
    exe = Path(override) if override else ROOT / "build" / "clang-release" / "runtime" / "gow3.exe"
    if not exe.is_file():
        pytest.skip(f"gow3.exe not built ({exe}): cmake --build --preset clang-release --target gow3")
    return exe


@pytest.mark.gioco
def test_milestone_gow3_logos(tmp_path):
    exe = _gow3_exe()
    _require_adapter(exe)
    shot = tmp_path / "s.bmp"
    proc = subprocess.run([str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "user_data"),
                           "--run-seconds", str(RUN_SECONDS), "--screenshot", f"{SCREENSHOT_AT_S}:{shot}"],
                          cwd=tmp_path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=TIMEOUT_S)
    log = (tmp_path / "gow3.log").read_text(encoding="utf-8", errors="replace")
    tail = log[-4000:]
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{tail}"
    lines = log.splitlines()
    assert not [l for l in lines if l.startswith("crash")], f"crash line in the log:\n{tail}"
    assert not [l for l in lines if "device removed" in l], f"device removed in the log:\n{tail}"
    assert _summary(log, "host_frames_presented") >= 60, tail

    assert shot.is_file(), f"screenshot missing:\n{tail}"
    data = shot.read_bytes()
    assert data[:2] == b"BM"
    assert struct.unpack_from("<ii", data, 18) == (WIDTH, HEIGHT)
    assert struct.unpack_from("<H", data, 28)[0] == 24
    assert len(data) == 54 + WIDTH * HEIGHT * 3
    stddev = luminance_stddev(data)
    print(f"luminance_stddev={stddev:.2f} host_frames_presented={_summary(log, 'host_frames_presented')}")
    assert stddev > 8, f"screenshot almost uniform: luminance stddev {stddev:.2f}"
