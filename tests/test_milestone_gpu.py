"""Milestone of sub-project 3: the game drawn by Xenia's D3D12 backend.

Command: gow2.exe --run-seconds 60 --screenshot 45:<tmp>/s.bmp, user_data in tmp.
Criterion: exit 0; no line that starts with `crash` or contains
`device removed`; host_frames_presented >= 60; gpu_draws_skipped=0; s.bmp is a
24-bit 1280x720 BMP with a luminance standard deviation
(Y = 0.299 R + 0.587 G + 0.114 B, over all pixels) > 8.
Skipped without game_data/default.xex, without gow2.exe or without a hardware
D3D12 adapter (`gow2.exe --gpu-probe`). Screenshot and log stay in the tmp_path.
"""

import array
import math
import os
import struct
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
GAME_DATA = ROOT / "game_data"
XEX = GAME_DATA / "default.xex"
RUN_SECONDS = 60
SCREENSHOT_AT_S = 45
TIMEOUT_S = 150
WIDTH, HEIGHT = 1280, 720

pytestmark = pytest.mark.skipif(not XEX.is_file(), reason=f"{XEX} missing (extract the disc first)")


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


def luminance_stddev(bmp: bytes) -> float:
    """Standard deviation of Y over all pixels of a bottom-up 24-bit BMP without padding (width*3 % 4 == 0)."""
    offset = struct.unpack_from("<I", bmp, 10)[0]
    width, height = struct.unpack_from("<ii", bmp, 18)
    pixels = memoryview(bmp)[offset:offset + width * abs(height) * 3]
    b = array.array("B", pixels[0::3])
    g = array.array("B", pixels[1::3])
    r = array.array("B", pixels[2::3])
    n = len(b)
    total = 0.0
    total_sq = 0.0
    for rv, gv, bv in zip(r, g, b):
        y = 0.299 * rv + 0.587 * gv + 0.114 * bv
        total += y
        total_sq += y * y
    mean = total / n
    return math.sqrt(max(total_sq / n - mean * mean, 0.0))


def test_luminance_stddev_of_uniform_and_checker_images():
    header = b"BM" + struct.pack("<IHHI", 54 + 4 * 2 * 3, 0, 0, 54) + struct.pack(
        "<IiiHHIIiiII", 40, 4, 2, 1, 24, 0, 4 * 2 * 3, 0, 0, 0, 0)
    uniform = header + bytes([10, 20, 30]) * 8
    assert luminance_stddev(uniform) == pytest.approx(0.0, abs=1e-9)
    checker = header + (bytes([0, 0, 0]) + bytes([255, 255, 255])) * 4
    assert luminance_stddev(checker) == pytest.approx(127.5, abs=1e-6)


@pytest.mark.gioco
def test_milestone_gpu_title_rendered(tmp_path):
    exe = _gow2_exe()
    _require_adapter(exe)
    shot = tmp_path / "s.bmp"
    proc = subprocess.run([str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "user_data"),
                           "--run-seconds", str(RUN_SECONDS), "--screenshot", f"{SCREENSHOT_AT_S}:{shot}"],
                          cwd=tmp_path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=TIMEOUT_S)
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    tail = log[-4000:]
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{tail}"
    lines = log.splitlines()
    assert not [l for l in lines if l.startswith("crash")], f"crash line in the log:\n{tail}"
    assert not [l for l in lines if "device removed" in l], f"device removed in the log:\n{tail}"
    assert _summary(log, "host_frames_presented") >= 60, tail
    assert _summary(log, "gpu_draws_skipped") == 0, tail

    assert shot.is_file(), f"screenshot missing:\n{tail}"
    data = shot.read_bytes()
    assert data[:2] == b"BM"
    assert struct.unpack_from("<ii", data, 18) == (WIDTH, HEIGHT)
    assert struct.unpack_from("<H", data, 28)[0] == 24
    assert len(data) == 54 + WIDTH * HEIGHT * 3
    stddev = luminance_stddev(data)
    print(f"luminance_stddev={stddev:.2f} host_frames_presented={_summary(log, 'host_frames_presented')}")
    assert stddev > 8, f"screenshot almost uniform: luminance stddev {stddev:.2f}"
