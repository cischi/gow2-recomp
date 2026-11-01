"""Milestone of the GoW3 bring-up: a new Campaign (Act 1) from the main menu.

Command: gow3.exe --data game_data_gow3 --user-data <tmp>/user_data --run-seconds 110
--audio null --input-script "30:START+200,40:A+200,46:A+200,50:A+200,54:A+200,58:A+200"
--screenshot 56:<tmp>/difficulty.bmp --screenshot 95:<tmp>/act1.bmp, cwd = tmp.
Sequence measured on 2026-10-27: START (30 s) -> brightness calibration, A (40 s)
-> main menu, A (46 s) CAMPAIGN, A (50 s) STANDARD, A (54 s) NEW -> "SELECT
DIFFICULTY, Act I: Troubled Past", A (58 s) Normal -> loading screen (Marcus,
tip) at ~64 s -> the Act 1 opening cinematic (Marcus in his cell on the ship)
rendered from ~70 s to the end of the run.
Criterion: exit 0; no `crash` line; no `device removed`; host_frames_presented >= 2200
(~3000 measured); packages_opened >= 28 (35 measured with the campaign loading its
characters, e.g. GUDSP_COGMarcusG_SF.xxx; title and menu alone open 14); both
screenshots 1280x720 24-bit with luminance standard deviation > 8; the replacements of
the 2x2 depth-of-field downsample (F820776A22228E1A, runtime/gpu/shaders/
dof_downsample_2x2_ss.hlsl), of the blur downsample (363BCAEF3EB3423B,
dof_blur_downsample_weighted.hlsl), of the final post-process (2F284B68A112ED3E,
dof_composite_joint.hlsl), and of the shadow projection (3C5E875D17B49A36,
shadow_projection_pcf.hlsl) logged active, never "layout differs" (their expected layouts
in gpu/shader_overrides.cpp must match Xenia's translations of the game's shaders).
Skipped like test_milestone_gow3_logos.py.
"""

import subprocess

import pytest

from test_milestone_gow3_logos import GAME_DATA, XEX, _gow3_exe
from test_milestone_gpu import _require_adapter, _summary, luminance_stddev

RUN_SECONDS = 110
TIMEOUT_S = 220
INPUT_SCRIPT = "30:START+200,40:A+200,46:A+200,50:A+200,54:A+200,58:A+200"
SHOTS = {"difficulty": 56, "act1": 95}
MIN_FRAMES = 2200
MENU_PACKAGES = 14  # title and main menu (test_milestone_gow3_menu.py)
MIN_PACKAGES = 28   # campaign loaded: 35 measured

pytestmark = pytest.mark.skipif(not XEX.is_file(), reason=f"{XEX} missing (extract the GoW3 disc first)")


@pytest.mark.gioco
def test_milestone_gow3_campaign(tmp_path):
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
    frames = _summary(log, "host_frames_presented")
    packages = _summary(log, "packages_opened")
    print(f"exit={proc.returncode} host_frames_presented={frames} packages_opened={packages}")
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{tail}"
    assert not [l for l in lines if l.startswith("crash")], f"crash line in the log:\n{tail}"
    assert not [l for l in lines if "device removed" in l], f"device removed in the log:\n{tail}"
    assert frames >= MIN_FRAMES, f"frames presented {frames} < {MIN_FRAMES}:\n{tail}"
    assert packages >= MIN_PACKAGES, (
        f"packages opened {packages} < {MIN_PACKAGES} (title and menu open {MENU_PACKAGES}): "
        f"did the script fail to start the campaign?\n{tail}")
    for shader in ("F820776A22228E1A", "363BCAEF3EB3423B", "2F284B68A112ED3E", "3C5E875D17B49A36"):
        assert f"[gpu] replacement shader {shader} active" in log, (
            f"replacement {shader} not active:\n{[l for l in lines if 'replacement shader' in l]}")
    assert not [l for l in lines if "layout differs" in l], f"replacement layout mismatch:\n{tail}"
    for name in SHOTS:
        data = (tmp_path / (name + ".bmp")).read_bytes()
        assert len(data) == 54 + 1280 * 720 * 3, f"{name}.bmp missing or not 1280x720 24-bit:\n{tail}"
        stddev = luminance_stddev(data)
        print(f"{name}: luminance_stddev={stddev:.2f}")
        assert stddev > 8, f"{name}.bmp almost uniform: luminance stddev {stddev:.2f}"
