"""Milestone of the FSR and DLSS upscalers on GoW3.

130 s runs with the menu/BACK script of test_milestone_gow3_gameplay.py (gameplay from ~110 s, LS_UP walk at
118-123 s), a 1920x1080 window, render 1280x720 and an empty temporary gow3.toml: `--upscaler fsr`, `dlss`, and `dlss`
with `[patches] disable_motion_blur = true` (motion blur off through the constants of its draw: the capture must not
lose its shader). Screenshot at 121 s (walking).

Criteria: exit 0; no `crash` or `device removed` line; a `[gpu] upscaler: FSR 3.` (or `DLSS `) line with an output
larger than the render; at least two `[gpu] upscaler: last 10 s: upscaled U of F frames` windows with F >= 250 and
U >= 0.95 F (gameplay frames reconstructed: GoW3's split post chain is captured every frame; without the capture U is 0
and the creation line never appears); the screenshot 1920 wide with luminance stddev > 8. dlss_no_mb also needs
`[patch] disable_motion_blur: turned on (uber post constants)`.

Skipped like test_milestone_gow3_logos.py; dlss variants also without gow2_dlss.dll next to gow3.exe or when the
runtime refuses DLSS (`[gpu] DLSS: ` line without the creation line). The test prints the measured windows.
"""

import re
import struct
import subprocess

import pytest

from test_milestone_gow3_logos import GAME_DATA, XEX, _gow3_exe
from test_milestone_gpu import _require_adapter, luminance_stddev

RUN_SECONDS = 130
TIMEOUT_S = 260
INPUT_SCRIPT = ("36:START+200,46:A+200,52:A+200,58:A+200,64:A+200,70:A+200,"
                "95:BACK+200,101:BACK+200,107:BACK+200,118:LS_UP+5000")
SHOT_AT_S = 121
MIN_WINDOW_FRAMES = 250
MIN_UPSCALED = 0.95
NAME = {"fsr": r"FSR 3\.\S*", "dlss": r"DLSS \S+"}

pytestmark = pytest.mark.skipif(not XEX.is_file(), reason=f"{XEX} missing (extract the GoW3 disc first)")


def run_gow3(tmp_path, config: str, args: list, run_seconds: int = RUN_SECONDS, shots=(SHOT_AT_S,)):
    """gow3.exe with the gameplay script and a temporary gow3.toml; returns (exit code, log, lines, tail)."""
    exe = _gow3_exe()
    _require_adapter(exe)
    cfg = tmp_path / "gow3.toml"
    cfg.write_text(config, encoding="utf-8")
    cmd = [str(exe), "--config", str(cfg), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "user_data"),
           "--run-seconds", str(run_seconds), "--audio", "null", "--window", "windowed", "--resolution", "1920x1080",
           "--input-script", INPUT_SCRIPT] + args
    for s in shots:
        cmd += ["--screenshot", f"{s}:{tmp_path / ('s%d.bmp' % s)}"]
    try:
        proc = subprocess.run(cmd, cwd=tmp_path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        pytest.fail(f"gow3.exe did not exit within {TIMEOUT_S} s")
    log_path = tmp_path / "gow3.log"
    assert log_path.is_file(), f"gow3.log missing (exit {proc.returncode})"
    log = log_path.read_text(encoding="utf-8", errors="replace")
    lines = log.splitlines()
    tail = log[-4000:]
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{tail}"
    assert not [l for l in lines if l.startswith("crash")], f"crash line in the log:\n{tail}"
    assert not [l for l in lines if "device removed" in l], f"device removed in the log:\n{tail}"
    return log, lines, tail


def test_upscaled_window_pattern():
    m = re.search(r"\[gpu\] upscaler: last 10 s: upscaled (\d+) of (\d+) frames",
                  "[gpu] upscaler: last 10 s: upscaled 299 of 300 frames")
    assert m and (int(m.group(1)), int(m.group(2))) == (299, 300)


@pytest.mark.gioco
@pytest.mark.parametrize("variant", ["fsr", "dlss", "dlss_no_mb"])
def test_milestone_gow3_upscaler(tmp_path, variant):
    upscaler = variant.removesuffix("_no_mb")
    if upscaler == "dlss" and not (_gow3_exe().parent / "gow2_dlss.dll").is_file():
        pytest.skip("gow2_dlss.dll not next to gow3.exe (build without GOW2_DLSS_SDK_DIR)")
    no_mb = variant.endswith("_no_mb")
    log, lines, tail = run_gow3(tmp_path, "[patches]\ndisable_motion_blur = true\n" if no_mb else "",
                                ["--upscaler", upscaler])
    if upscaler == "dlss" and [l for l in lines if "[gpu] DLSS: " in l] and \
            not [l for l in lines if "upscaler: DLSS" in l]:
        pytest.skip("DLSS refused on this machine: " + [l for l in lines if "[gpu] DLSS: " in l][0])
    pattern = r"\[gpu\] upscaler: " + NAME[upscaler] + r" \((\d+)x(\d+) -> (\d+)x(\d+)\)"
    created = [m for l in lines if (m := re.search(pattern, l))]
    windows = [(int(m.group(1)), int(m.group(2))) for l in lines
               if (m := re.search(r"\[gpu\] upscaler: last 10 s: upscaled (\d+) of (\d+) frames", l))]
    print(f"{variant}: {[m.group(0) for m in created]} windows (upscaled, presented) {windows}")
    assert created, f"{upscaler} never created (no capture of the post chain?):\n{tail}"
    rw, rh, ow, oh = map(int, created[-1].groups())
    assert ow > rw and oh > rh, f"no upscaling: {created[-1].group(0)}"
    good = [w for w in windows if w[1] >= MIN_WINDOW_FRAMES and w[0] >= MIN_UPSCALED * w[1]]
    assert len(good) >= 2, f"fewer than 2 windows with >= {MIN_UPSCALED:.0%} of the frames upscaled: {windows}"
    if no_mb:
        assert "[patch] disable_motion_blur: turned on (uber post constants)" in lines, tail
    bmp = (tmp_path / f"s{SHOT_AT_S}.bmp").read_bytes()
    width, height = struct.unpack_from("<ii", bmp, 18)
    stddev = luminance_stddev(bmp)
    print(f"screenshot {width}x{abs(height)} stddev {stddev:.2f}")
    assert width == 1920, f"screenshot {width}x{abs(height)}, expected the 1920x1080 window"
    assert stddev > 8, f"screenshot almost uniform: luminance stddev {stddev:.2f}"
