"""Milestone of the 21:9 and 16:10 aspect ratios.

60 s runs with a quick start (initial movies skipped, input up to the first cutscene) and the FSR upscaler, with a window of the
aspect's shape: 2560x1080 for 21:9, 1920x1200 for 16:10. Screenshot at 55 s (moving cutscene).

Criteria: exit 0; no `crash` or `device removed` line; `[patch] aspect.<aspect>: turned on` and `[aspect]` lines for the
aspect (with `MSAA 2x`); a `[gpu] upscaler: FSR 3.` line with input = the aspect's scene and output of the
same shape (within 1%); screenshot with a luminance standard deviation > 10.

Skipped without game_data/default.xex, without gow2.exe or without a hardware D3D12 adapter. Log and screenshot stay in the
tmp_path; the test prints the upscaler line and the screenshot size.
"""

import re
import struct
import subprocess

import pytest

from test_milestone_gpu import XEX, GAME_DATA, _gow2_exe, _require_adapter, luminance_stddev

RUN_SECONDS = 60
SCREENSHOT_AT_S = 55
TIMEOUT_S = 150
INPUT_SCRIPT = "8:START+200,14:A+200,18:A+200,22:A+200,26:A+200,30:A+200,34:A+200"
SKIP_MOVIES = ["EpicLogo", "MGSLogo", "Attract_Opening_Cinematic"]
MIN_STDDEV = 10


@pytest.mark.gioco
@pytest.mark.parametrize("aspect, window, scene", [("21:9", "2560x1080", (1280, 550)),
                                                   ("16:10", "1920x1200", (1280, 800))])
def test_milestone_aspect(tmp_path, aspect, window, scene):
    if not XEX.is_file():
        pytest.skip(f"{XEX} missing: extract the disc into game_data/")
    exe = _gow2_exe()
    _require_adapter(exe)
    shot = tmp_path / "s.bmp"
    args = [str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "user_data"),
            "--run-seconds", str(RUN_SECONDS), "--audio", "null", "--input-script", INPUT_SCRIPT,
            "--window", "windowed", "--resolution", window, "--aspect", aspect, "--upscaler", "fsr",
            "--screenshot", f"{SCREENSHOT_AT_S}:{shot}"]
    for movie in SKIP_MOVIES:
        args += ["--skip-movie", movie]
    log_path = tmp_path / "gow2.log"
    try:
        proc = subprocess.run(args, cwd=tmp_path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        partial = log_path.read_text(encoding="utf-8", errors="replace") if log_path.is_file() else "(gow2.log missing)"
        pytest.fail(f"gow2.exe ({aspect}) did not exit within {TIMEOUT_S} s:\n{partial[-4000:]}")
    assert log_path.is_file(), f"gow2.log missing (exit {proc.returncode})"
    log = log_path.read_text(encoding="utf-8", errors="replace")
    tail = log[-4000:]
    lines = log.splitlines()
    sw, sh = scene
    created = [m for l in lines if (m := re.search(
        rf"\[gpu\] upscaler: FSR 3\.\S* \({sw}x{sh} -> (\d+)x(\d+)\)", l))]
    print(f"{aspect}: exit={proc.returncode} {[m.group(0) for m in created]}")

    assert proc.returncode == 0, f"exit {proc.returncode}:\n{tail}"
    assert not [l for l in lines if l.startswith("crash")], f"crash line in the log:\n{tail}"
    assert not [l for l in lines if "device removed" in l], f"device removed in the log:\n{tail}"
    patch = "aspect." + aspect.replace(":", "_")
    assert [l for l in lines if f"[patch] {patch}: turned on" in l], f"{patch} not turned on:\n{tail}"
    aspect_lines = [l for l in lines if f"[aspect] {aspect}: scene {sw}x{sh}" in l]
    assert aspect_lines, f"[aspect] {aspect} line missing:\n{tail}"
    assert f"MSAA 2x ({sh}-row tiles)" in aspect_lines[-1], aspect_lines[-1]
    assert created, f"FSR not created with input {sw}x{sh}:\n{tail}"
    ow, oh = map(int, created[-1].groups())
    assert abs(ow / oh - sw / sh) <= 0.01 * sw / sh, f"output of a different shape: {created[-1].group(0)}"
    assert shot.is_file(), f"screenshot missing:\n{tail}"
    bmp = shot.read_bytes()
    width, height = struct.unpack_from("<ii", bmp, 18)
    stddev = luminance_stddev(bmp)
    print(f"screenshot {width}x{abs(height)} stddev {stddev:.2f}")
    assert stddev > MIN_STDDEV, f"screenshot almost uniform: luminance stddev {stddev:.2f}"
