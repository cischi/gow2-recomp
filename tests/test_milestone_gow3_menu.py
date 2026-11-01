"""Milestone of the GoW3 bring-up: PRESS START leads to the main menu.

Command: gow3.exe --data game_data_gow3 --user-data <tmp>/user_data --run-seconds 66
--audio null --input-script "36:START+200,46:A+200" --screenshot 42:<tmp>/calibration.bmp
--screenshot 58:<tmp>/menu.bmp, cwd = tmp.
Sequence measured on 2026-10-27: the title screen ("PRESS START") from ~26 s; START
(36 s, 10 s of margin) opens the first-boot brightness calibration ("VIDEO
SETTINGS", five cogs) within ~4 s; A (46 s) accepts it and the main menu
(CAMPAIGN, VERSUS, HORDE, BEAST...) is on screen ~3 s later.
Only the path past the title screen produces:
- the log line `XexGetProcedureAddress: xam.xex XamContentAggregateCreateEnumerator -> `
  (the XContent aggregate enumeration START triggers, which crashed before);
- two different screens: calibration.bmp (mostly black, cogs) and menu.bmp (menu
  over the bright background) differ by a mean luminance gap > 10 (measured 3 vs 66).
Criterion: exit 0; no `crash` line; no `device removed`; host_frames_presented >= 1300;
the log line above; both screenshots 1280x720 24-bit with luminance standard
deviation > 8; their mean luminance differs by more than 10.
Skipped like test_milestone_gow3_logos.py.
"""

import subprocess

import pytest

from test_milestone_gow3_logos import GAME_DATA, XEX, _gow3_exe
from test_milestone_gpu import _require_adapter, _summary, luminance_stddev

RUN_SECONDS = 66
TIMEOUT_S = 160
INPUT_SCRIPT = "36:START+200,46:A+200"
SHOTS = {"calibration": 42, "menu": 58}
MIN_FRAMES = 1300  # ~1900 expected in 66 s
MENU_PATH_LINE = "XexGetProcedureAddress: xam.xex XamContentAggregateCreateEnumerator -> "
MIN_MEAN_GAP = 10

pytestmark = pytest.mark.skipif(not XEX.is_file(), reason=f"{XEX} missing (extract the GoW3 disc first)")


def _mean_luminance(bmp: bytes) -> float:
    """Mean Rec. 601 luminance of a 24-bit BMP (BGR), every 7th pixel."""
    px = bmp[54:]
    total = n = 0
    for i in range(0, len(px) - 2, 21):
        total += 0.114 * px[i] + 0.587 * px[i + 1] + 0.299 * px[i + 2]
        n += 1
    return total / n


@pytest.mark.gioco
def test_milestone_gow3_menu(tmp_path):
    exe = _gow3_exe()
    _require_adapter(exe)
    args = [str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "user_data"),
            "--run-seconds", str(RUN_SECONDS), "--audio", "null", "--input-script", INPUT_SCRIPT]
    for name, at in SHOTS.items():
        args += ["--screenshot", f"{at}:{tmp_path / (name + '.bmp')}"]
    proc = subprocess.run(args, cwd=tmp_path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                          timeout=TIMEOUT_S)
    log = (tmp_path / "gow3.log").read_text(encoding="utf-8", errors="replace")
    tail = log[-4000:]
    lines = log.splitlines()
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{tail}"
    assert not [l for l in lines if l.startswith("crash")], f"crash line in the log:\n{tail}"
    assert not [l for l in lines if "device removed" in l], f"device removed in the log:\n{tail}"
    frames = _summary(log, "host_frames_presented")
    assert frames >= MIN_FRAMES, f"frames presented {frames} < {MIN_FRAMES}:\n{tail}"
    assert MENU_PATH_LINE in log, f"no '{MENU_PATH_LINE}': START did not leave the title screen?\n{tail}"
    means = {}
    for name in SHOTS:
        data = (tmp_path / (name + ".bmp")).read_bytes()
        assert len(data) == 54 + 1280 * 720 * 3, f"{name}.bmp missing or not 1280x720 24-bit:\n{tail}"
        stddev = luminance_stddev(data)
        means[name] = _mean_luminance(data)
        print(f"{name}: luminance_stddev={stddev:.2f} mean={means[name]:.2f}")
        assert stddev > 8, f"{name}.bmp almost uniform: luminance stddev {stddev:.2f}"
    gap = abs(means["menu"] - means["calibration"])
    assert gap > MIN_MEAN_GAP, f"calibration and menu look alike (mean gap {gap:.2f}): stuck on one screen?"
