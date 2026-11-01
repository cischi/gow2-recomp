"""Milestone of the GoW3 bring-up: controllable gameplay in Act 1 (the prison ship).

Command: gow3.exe --data game_data_gow3 --user-data <tmp>/user_data --run-seconds 142
--audio null --input-script "36:START+200,46:A+200,52:A+200,58:A+200,64:A+200,70:A+200,
95:BACK+200,101:BACK+200,107:BACK+200,124:LS_UP+4000,131:LS_DOWN+4000"
--screenshot 118:<tmp>/idle.bmp --screenshot 123:<tmp>/before.bmp
--screenshot 130:<tmp>/after.bmp --screenshot 137:<tmp>/back.bmp, cwd = tmp.
Sequence measured on 2026-10-27: the menu presses of test_milestone_gow3_campaign.py,
spaced 6 s apart (the menu accepts late presses), reach the in-engine Act 1 opening
cinematic at ~82 s. START would only pause it; BACK skips it -> gameplay: Marcus crouched
by the open cell door, objective "Follow Anya", then the "LS - WALK" tutorial. BACK is
pressed three times so a slower load still gets a skip; in gameplay BACK showed nothing
(screenshots 1 s after the extra presses are unchanged gameplay). LS_UP (124 s) walks
Marcus out of the cell into the hall; LS_DOWN (131 s) walks him back to the door.
Criterion: exit 0; no `crash` line; no `device removed`; host_frames_presented >= 2800
(~4100 expected at 30 fps); the four screenshots 1280x720 24-bit with luminance standard
deviation > 8; mean |dY| idle -> before (no input) < 10 (~2.5 measured), before -> after
(LS_UP) > 15 (~29), and back -> before (LS_DOWN undid the walk) < half of before -> after
(~8): the view follows the stick and returns with the opposite direction, which a
cinematic, a pause menu or a loading screen cannot do. The replacements of the shadow
projections of the hall's spot and point lights (857CECDEB64A657F, CA3F4A808B09D6F4,
runtime/gpu/shaders/shadow_local_light_pcf.hlsl) logged active, never "layout differs".
The non-empty content/GearsCheckpoint/GearsCheckpoint0.sav only shows that saving works:
the game writes it ~30 s after the campaign starts, with or without the skip, so it is
not proof of gameplay.
Skipped like test_milestone_gow3_logos.py.
"""

import subprocess

import pytest

from test_milestone_audio_input import mean_abs_luma_diff
from test_milestone_gow3_logos import GAME_DATA, XEX, _gow3_exe
from test_milestone_gpu import _require_adapter, _summary, luminance_stddev

RUN_SECONDS = 142
TIMEOUT_S = 260
INPUT_SCRIPT = ("36:START+200,46:A+200,52:A+200,58:A+200,64:A+200,70:A+200,"
                "95:BACK+200,101:BACK+200,107:BACK+200,124:LS_UP+4000,131:LS_DOWN+4000")
SHOTS = {"idle": 118, "before": 123, "after": 130, "back": 137}
MIN_FRAMES = 2800
MAX_IDLE_DIFF = 10
MIN_MOVE_DIFF = 15

pytestmark = pytest.mark.skipif(not XEX.is_file(), reason=f"{XEX} missing (extract the GoW3 disc first)")


@pytest.mark.gioco
def test_milestone_gow3_gameplay(tmp_path):
    exe = _gow3_exe()
    _require_adapter(exe)
    user_data = tmp_path / "user_data"
    args = [str(exe), "--data", str(GAME_DATA), "--user-data", str(user_data),
            "--run-seconds", str(RUN_SECONDS), "--audio", "null", "--input-script", INPUT_SCRIPT]
    for name, at in SHOTS.items():
        args += ["--screenshot", f"{at}:{tmp_path / (name + '.bmp')}"]
    proc = subprocess.run(args, cwd=tmp_path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                          timeout=TIMEOUT_S)
    log = (tmp_path / "gow3.log").read_text(encoding="utf-8", errors="replace")
    tail = log[-4000:]
    lines = log.splitlines()
    frames = _summary(log, "host_frames_presented")
    print(f"exit={proc.returncode} host_frames_presented={frames}")
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{tail}"
    assert not [l for l in lines if l.startswith("crash")], f"crash line in the log:\n{tail}"
    assert not [l for l in lines if "device removed" in l], f"device removed in the log:\n{tail}"
    assert frames >= MIN_FRAMES, f"frames presented {frames} < {MIN_FRAMES}:\n{tail}"
    for shader in ("857CECDEB64A657F", "CA3F4A808B09D6F4"):
        assert f"[gpu] replacement shader {shader} active" in log, (
            f"replacement {shader} not active:\n{[l for l in lines if 'replacement shader' in l]}")
    assert not [l for l in lines if "layout differs" in l], f"replacement layout mismatch:\n{tail}"
    shots = {}
    for name in SHOTS:
        data = (tmp_path / (name + ".bmp")).read_bytes()
        assert len(data) == 54 + 1280 * 720 * 3, f"{name}.bmp missing or not 1280x720 24-bit:\n{tail}"
        stddev = luminance_stddev(data)
        print(f"{name}: luminance_stddev={stddev:.2f}")
        assert stddev > 8, f"{name}.bmp almost uniform: luminance stddev {stddev:.2f}"
        shots[name] = data
    idle = mean_abs_luma_diff(shots["idle"], shots["before"])
    move = mean_abs_luma_diff(shots["before"], shots["after"])
    undo = mean_abs_luma_diff(shots["back"], shots["before"])
    print(f"idle diff={idle:.2f} move diff={move:.2f} back-vs-before diff={undo:.2f}")
    assert idle < MAX_IDLE_DIFF, f"view changed by {idle:.2f} without input: not in gameplay (cinematic?)"
    assert move > MIN_MOVE_DIFF, f"view changed by {move:.2f} with LS_UP held: player not controllable"
    assert undo < move / 2, (
        f"LS_DOWN did not bring the view back ({undo:.2f} vs {move:.2f}): the view does not follow the stick")
    # Save path only: the checkpoint is written with or without the skip (see the docstring).
    checkpoint = user_data / "content" / "GearsCheckpoint" / "GearsCheckpoint0.sav"
    assert checkpoint.is_file() and checkpoint.stat().st_size > 0, f"no checkpoint saved ({checkpoint})"
