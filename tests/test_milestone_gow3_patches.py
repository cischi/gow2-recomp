"""Milestone of the GoW3 graphics patches: two gameplay runs with the menu/BACK script of
test_milestone_gow3_gameplay.py (gameplay from ~110 s), `--vsync off --vrr off`, settings from a temporary gow3.toml.

Run "21:9": 2560x1080 window, --aspect 21:9, --frame-rate 60, [patches] show_fps. Criteria: the patches turned on;
host frames between the screenshots at 112 and 128 s >= 50 per second (30 with the original cap, 60 measured);
columns 40..300 of the 118 s shot not black (a 16:9 scene in this window has black bars 320 px wide there: mean
luminance 0.0 measured, 6 with 21:9); >= 2000 bright green pixels (the FPS counter: 0 without show_fps, ~8000 with).

Run "16:10": 1920x1200 window, --aspect 16:10, [patches] disable_motion_blur/disable_lens_flares/disable_bloom and
[engine] shadow_quality low, dynamic_shadows off. Criteria: the patches turned on, the 9 Coalesced modified,
coalesced.no_hash_check on and no "check patch not applied"; rows 0..55 and 1145..1199 of the 118 s shot not black
(16:9 has 60-row bars there; 6 and 12 measured with 16:10). The effects have no measurable pixel criterion in this
dark scene: their hooks were seen reached (bloom, motion blur) with a temporary log, see the report. Motion blur is
now off through the constants of its draw (game_profile::PostCapture::blur_off), like GoW2.

Both: exit 0, no crash / device removed, shots with luminance stddev > 8 (gameplay, not a black screen).
Skipped like test_milestone_gow3_logos.py.
"""

import array
import re
import struct
import subprocess

import pytest

from test_milestone_gow3_logos import GAME_DATA, XEX, _gow3_exe
from test_milestone_gpu import _require_adapter, luminance_stddev

RUN_SECONDS = 130
TIMEOUT_S = 260
INPUT_SCRIPT = ("36:START+200,46:A+200,52:A+200,58:A+200,64:A+200,70:A+200,"
                "95:BACK+200,101:BACK+200,107:BACK+200")
SHOTS = [112, 118, 128]

pytestmark = pytest.mark.skipif(not XEX.is_file(), reason=f"{XEX} missing (extract the GoW3 disc first)")


def region_mean(bmp: bytes, x0: int, x1: int, y0: int, y1: int) -> float:
    """Mean luminance of [x0, x1) x [y0, y1) (y from the top) of a bottom-up 24-bit BMP without padding."""
    offset = struct.unpack_from("<I", bmp, 10)[0]
    width, height = struct.unpack_from("<ii", bmp, 18)
    total, n = 0.0, 0
    for y in range(y0, y1):
        row = offset + (height - 1 - y) * width * 3
        px = memoryview(bmp)[row + x0 * 3:row + x1 * 3]
        b, g, r = array.array("B", px[0::3]), array.array("B", px[1::3]), array.array("B", px[2::3])
        total += sum(0.299 * rv + 0.587 * gv + 0.114 * bv for rv, gv, bv in zip(r, g, b))
        n += len(b)
    return total / n


def green_pixels(bmp: bytes) -> int:
    """Pixels with G > 180 and R, B < 120 (the green text of the game's FPS counter)."""
    offset = struct.unpack_from("<I", bmp, 10)[0]
    px = memoryview(bmp)[offset:]
    b, g, r = array.array("B", px[0::3]), array.array("B", px[1::3]), array.array("B", px[2::3])
    return sum(1 for rv, gv, bv in zip(r, g, b) if gv > 180 and rv < 120 and bv < 120)


def test_region_helpers():
    header = b"BM" + struct.pack("<IHHI", 54 + 4 * 2 * 3, 0, 0, 54) + struct.pack(
        "<IiiHHIIiiII", 40, 4, 2, 1, 24, 0, 4 * 2 * 3, 0, 0, 0, 0)
    # Bottom row black, top row green (BGR).
    bmp = header + bytes([0, 0, 0]) * 4 + bytes([0, 255, 0]) * 4
    assert region_mean(bmp, 0, 4, 1, 2) == 0.0
    assert abs(region_mean(bmp, 0, 4, 0, 1) - 0.587 * 255) < 1e-6
    assert green_pixels(bmp) == 4


def _run(tmp_path, config: str, args: list) -> tuple:
    exe = _gow3_exe()
    _require_adapter(exe)
    cfg = tmp_path / "gow3.toml"
    cfg.write_text(config, encoding="utf-8")
    cmd = [str(exe), "--config", str(cfg), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "user_data"),
           "--run-seconds", str(RUN_SECONDS), "--audio", "null", "--window", "windowed", "--vsync", "off",
           "--vrr", "off", "--input-script", INPUT_SCRIPT] + args
    for s in SHOTS:
        cmd += ["--screenshot", f"{s}:{tmp_path / ('s%d.bmp' % s)}"]
    proc = subprocess.run(cmd, cwd=tmp_path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=TIMEOUT_S)
    log = (tmp_path / "gow3.log").read_text(encoding="utf-8", errors="replace")
    tail = log[-4000:]
    lines = log.splitlines()
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{tail}"
    assert not [l for l in lines if l.startswith("crash")], f"crash line in the log:\n{tail}"
    assert not [l for l in lines if "device removed" in l], f"device removed in the log:\n{tail}"
    frames = {int(m.group(1)): int(m.group(2))
              for m in re.finditer(r"\[gpu\] screenshot \S*?s(\d+)\.bmp \d+x\d+ frame host (\d+)", log)}
    assert sorted(frames) == SHOTS, f"screenshots {sorted(frames)}:\n{tail}"
    shots = {s: (tmp_path / ("s%d.bmp" % s)).read_bytes() for s in SHOTS}
    for s, data in shots.items():
        stddev = luminance_stddev(data)
        print(f"s{s}: luminance_stddev={stddev:.2f}")
        assert stddev > 8, f"s{s}.bmp almost uniform: luminance stddev {stddev:.2f}"
    return lines, tail, frames, shots


def _turned_on(lines, tail, key):
    # disable_motion_blur goes through the motion blur draw's constants: "turned on (uber post constants)".
    assert [l for l in lines if l.startswith(f"[patch] {key}: turned on")], f"{key} not turned on:\n{tail}"


@pytest.mark.gioco
def test_milestone_gow3_patches_21_9_60fps_show_fps(tmp_path):
    lines, tail, frames, shots = _run(tmp_path, "[patches]\nshow_fps = true\n",
                                      ["--resolution", "2560x1080", "--aspect", "21:9", "--frame-rate", "60"])
    for key in ("show_fps", "frame_rate.interval", "frame_rate.unlock", "aspect.21_9"):
        _turned_on(lines, tail, key)
    assert [l for l in lines if l.startswith("[frame] frame rate 60: present interval 1")], tail
    fps = (frames[128] - frames[112]) / (128 - 112)
    side = region_mean(shots[118], 40, 300, 0, 1080)
    green = green_pixels(shots[118])
    print(f"fps={fps:.1f} side columns mean={side:.2f} green pixels={green}")
    assert fps >= 50, f"{fps:.1f} frames per second in gameplay: the 30 fps cap is still there"
    assert side > 2, f"columns 40..300 black ({side:.2f}): pillarboxed 16:9 scene, not 21:9"
    assert green >= 2000, f"{green} green pixels: no FPS counter on screen"


@pytest.mark.gioco
def test_milestone_gow3_patches_16_10_effects_engine(tmp_path):
    config = ("[patches]\ndisable_motion_blur = true\ndisable_lens_flares = true\ndisable_bloom = true\n"
              "[engine]\nshadow_quality = \"low\"\ndynamic_shadows = false\n")
    lines, tail, frames, shots = _run(tmp_path, config, ["--resolution", "1920x1200", "--aspect", "16:10"])
    for key in ("disable_motion_blur", "disable_lens_flares", "disable_bloom", "aspect.16_10",
                "coalesced.no_hash_check"):
        _turned_on(lines, tail, key)
    assert "[engine] Coalesced_INT.bin: MaxShadowResolution 1120 -> 256, DynamicShadows True -> False" in lines, tail
    assert not [l for l in lines if "check patch not applied" in l], tail
    top = region_mean(shots[118], 0, 1920, 0, 55)
    bottom = region_mean(shots[118], 0, 1920, 1145, 1200)
    print(f"top rows mean={top:.2f} bottom rows mean={bottom:.2f}")
    assert top > 2 and bottom > 2, f"top/bottom rows black ({top:.2f}, {bottom:.2f}): letterboxed 16:9, not 16:10"
