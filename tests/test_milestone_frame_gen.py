"""Milestone of FSR 3 frame generation.

A 70 s run at 1920x1080, 60 fps, `--frame-generation on`. Criteria: exit 0; no `crash` or
`device removed` line; `[fg] swap chain proxy ` and `[fg] FG context ` lines; a `[fg] generation on at
frame ` line; at least one `[fg] last 10 s: real R, generated G` line with R >= 200 and G >= 0.9 R.

Skipped without game_data/default.xex, without gow2.exe, without a D3D12 adapter or without the frame generation DLL
next to gow2.exe, or when OptiScaler is installed there (it turns frame generation off). The test prints the measured pairs.
"""

import re
import subprocess

import pytest

from test_milestone_gpu import XEX, GAME_DATA, _gow2_exe, _require_adapter
from test_milestone_upscaler import INPUT_SCRIPT, SKIP_MOVIES

RUN_SECONDS = 70
TIMEOUT_S = 160
MIN_REAL = 200
MIN_RATIO = 0.9
DLL = "amd_fidelityfx_framegeneration_dx12.dll"


@pytest.mark.gioco
def test_milestone_frame_gen(tmp_path):
    if not XEX.is_file():
        pytest.skip(f"{XEX} missing: extract the disc into game_data/")
    exe = _gow2_exe()
    if not (exe.parent / DLL).is_file():
        pytest.skip(f"{DLL} not next to gow2.exe")
    _require_adapter(exe)
    args = [str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "user_data"),
            "--run-seconds", str(RUN_SECONDS), "--audio", "null", "--input-script", INPUT_SCRIPT,
            "--window", "windowed", "--resolution", "1920x1080", "--frame-rate", "60",
            "--frame-generation", "on"]
    for movie in SKIP_MOVIES:
        args += ["--skip-movie", movie]
    try:
        proc = subprocess.run(args, cwd=tmp_path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        pytest.fail(f"gow2.exe did not exit within {TIMEOUT_S} s")
    log_path = tmp_path / "gow2.log"
    assert log_path.is_file(), f"gow2.log missing (exit {proc.returncode})"
    log = log_path.read_text(encoding="utf-8", errors="replace")
    lines = log.splitlines()
    tail = log[-4000:]
    pairs = [(int(m.group(1)), int(m.group(2))) for l in lines
             if (m := re.search(r"\[fg\] last 10 s: real (\d+), generated (\d+)", l))]
    print(f"exit={proc.returncode} pairs (real, generated): {pairs}")

    assert proc.returncode == 0, f"exit {proc.returncode}:\n{tail}"
    assert not [l for l in lines if l.startswith("crash")], f"crash line in the log:\n{tail}"
    assert not [l for l in lines if "device removed" in l], f"device removed in the log:\n{tail}"
    if "[optiscaler] frame generation turned off: OptiScaler installed" in lines:
        pytest.skip("OptiScaler (dxgi.dll) next to gow2.exe turns frame generation off: uninstall it to run this test")
    for prefix in ("[fg] swap chain proxy ", "[fg] FG context ", "[fg] generation on at frame "):
        assert [l for l in lines if l.startswith(prefix)], f"line '{prefix}' missing:\n{tail}"
    assert [p for p in pairs if p[0] >= MIN_REAL and p[1] >= MIN_RATIO * p[0]], \
        f"no pair with real >= {MIN_REAL} and generated >= {MIN_RATIO} * real: {pairs}"
