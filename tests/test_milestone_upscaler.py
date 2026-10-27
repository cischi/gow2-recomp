"""Milestone of the FSR and DLSS upscalers.

60 s runs with a quick start (initial movies skipped, input up to the first cutscene), a 1920x1080
window and 1x internal resolution (render 1280x720): one with `--upscaler off` (a single one for the module), one with `--upscaler fsr`,
one with `dlss` and one with `dlss` and `[patches] disable_motion_blur = true`. Each run has its own `--config` (empty
except for the patch): the `gow2.toml` next to gow2.exe does not enter the test.
Screenshot at 55 s (moving cutscene).

Criteria of the fsr/dlss runs: exit 0; no `crash` or `device removed` line; a
`[gpu] upscaler: FSR 3.` (or `DLSS `) line with an output larger than the render (`1280x720 -> WxH`, W > 1280); screenshot
wider than 1280 with a luminance standard deviation > 10; host_frames_presented >= 90% of the off run.

Skipped without game_data/default.xex, without gow2.exe or without a hardware D3D12 adapter. The FSR DLLs
must be next to gow2.exe (the build copies them); the dlss run is skipped without gow2_dlss.dll (build without
GOW2_DLSS_SDK_DIR) or without an NVIDIA GPU (the runtime refuses DLSS in a `[gpu] DLSS: ` line). Log and screenshot stay in the tmp_path; the test
prints the measured values.
"""

import re
import struct
import subprocess

import pytest

from test_milestone_gpu import XEX, GAME_DATA, _gow2_exe, _require_adapter, _summary, luminance_stddev

RUN_SECONDS = 60
SCREENSHOT_AT_S = 55
TIMEOUT_S = 150
INPUT_SCRIPT = "8:START+200,14:A+200,18:A+200,22:A+200,26:A+200,30:A+200,34:A+200"
SKIP_MOVIES = ["EpicLogo", "MGSLogo", "Attract_Opening_Cinematic"]
# "Choose Path" (93-99 s) does not close by itself: LT = "Skip Training", repeated every 3 s up to 121 s (choice at 100 s
# at every frame rate; Jacinto at 115-120 s, game with the HUD from 145 s). Measured in a spike.
SKIP_TRAINING = ",".join(f"{t}:LT+300" for t in range(100, 122, 3))
MIN_FRAME_RATIO = 0.9
MIN_STDDEV = 10
NAME = {"fsr": r"FSR 3\.\S*", "dlss": r"DLSS \S+"}


def _run(exe, run_dir, upscaler, no_motion_blur=False):
    run_dir.mkdir()
    shot = run_dir / "s.bmp"
    config = run_dir / "gow2.toml"
    config.write_text("[patches]\ndisable_motion_blur = true\n" if no_motion_blur else "", encoding="utf-8")
    args = [str(exe), "--config", str(config), "--data", str(GAME_DATA), "--user-data", str(run_dir / "user_data"),
            "--run-seconds", str(RUN_SECONDS), "--audio", "null", "--input-script", INPUT_SCRIPT,
            "--window", "windowed", "--resolution", "1920x1080", "--upscaler", upscaler,
            "--screenshot", f"{SCREENSHOT_AT_S}:{shot}"]
    for movie in SKIP_MOVIES:
        args += ["--skip-movie", movie]
    log_path = run_dir / "gow2.log"
    try:
        proc = subprocess.run(args, cwd=run_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        partial = log_path.read_text(encoding="utf-8", errors="replace") if log_path.is_file() else "(gow2.log missing)"
        pytest.fail(f"gow2.exe ({upscaler}) did not exit within {TIMEOUT_S} s:\n{partial[-4000:]}")
    assert log_path.is_file(), f"gow2.log missing (exit {proc.returncode})"
    return proc.returncode, log_path.read_text(encoding="utf-8", errors="replace"), shot


@pytest.fixture(scope="module")
def off_frames(tmp_path_factory):
    """Frames presented by the run without an upscaler: a single one for fsr and dlss."""
    if not XEX.is_file():
        pytest.skip(f"{XEX} missing: extract the disc into game_data/")
    exe = _gow2_exe()
    _require_adapter(exe)
    _, off_log, _ = _run(exe, tmp_path_factory.mktemp("upscaler") / "off", "off")
    return _summary(off_log, "host_frames_presented")


@pytest.mark.gioco
# dlss_no_mb: with [patches] disable_motion_blur the game used another uber post variant and the upscaler
# never started; now the motion blur is turned off in the constants and the variant stays the captured one.
@pytest.mark.parametrize("variant", ["fsr", "dlss", "dlss_no_mb"])
def test_milestone_upscaler(tmp_path, variant, request):
    upscaler = variant.removesuffix("_no_mb")
    exe = _gow2_exe()
    if upscaler == "dlss" and not (exe.parent / "gow2_dlss.dll").is_file():
        pytest.skip("gow2_dlss.dll not next to gow2.exe (build without GOW2_DLSS_SDK_DIR)")
    # After the bridge check: with -k dlss on a build without DLSS the off run does not start.
    off_frames = request.getfixturevalue("off_frames")
    code, log, shot = _run(exe, tmp_path / variant, upscaler, no_motion_blur=variant.endswith("_no_mb"))
    tail = log[-4000:]
    lines = log.splitlines()
    frames = _summary(log, "host_frames_presented")
    refused = [l for l in lines if "[gpu] DLSS: " in l]
    if upscaler == "dlss" and refused and not [l for l in lines if "upscaler: DLSS" in l]:
        pytest.skip("DLSS refused on this machine: " + refused[0])
    pattern = r"\[gpu\] upscaler: " + NAME[upscaler] + r" \((\d+)x(\d+) -> (\d+)x(\d+)\)"
    created = [m for l in lines if (m := re.search(pattern, l))]
    print(f"exit={code} frames {upscaler}={frames} off={off_frames} {[m.group(0) for m in created]}")

    assert code == 0, f"exit {code}:\n{tail}"
    assert not [l for l in lines if l.startswith("crash")], f"crash line in the log:\n{tail}"
    assert not [l for l in lines if "device removed" in l], f"device removed in the log:\n{tail}"
    assert created, f"{upscaler} not created (DLLs next to gow2.exe?):\n{tail}"
    rw, rh, ow, oh = map(int, created[-1].groups())
    assert ow > rw and oh > rh, f"no upscaling: {created[-1].group(0)}"
    assert shot.is_file(), f"screenshot missing:\n{tail}"
    bmp = shot.read_bytes()
    width, height = struct.unpack_from("<ii", bmp, 18)
    stddev = luminance_stddev(bmp)
    print(f"screenshot {width}x{abs(height)} stddev {stddev:.2f}")
    assert width > rw, f"screenshot {width}x{abs(height)} not wider than the render {rw}"
    assert stddev > MIN_STDDEV, f"screenshot almost uniform: luminance stddev {stddev:.2f}"
    assert frames >= MIN_FRAME_RATIO * off_frames, f"frames presented {frames} < 90% of {off_frames} (off)"
