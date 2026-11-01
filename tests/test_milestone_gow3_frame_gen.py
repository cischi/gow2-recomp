"""Milestone of frame generation (FSR 3 and DLSS) on GoW3.

130 s runs with the gameplay script of test_milestone_gow3_upscaler.py, 1920x1080 window, `--frame-rate 60
--vsync off --frame-generation fsr|dlss`. Criteria: exit 0; no `crash` or `device removed` line; a
`[fg] generation on at frame ` line (frame generation turns on only with a capture of the post chain: never without
it); at least two `[fg] last 10 s: real R, generated G` windows with R >= 500 and G >= 0.95 R.

Skipped like test_milestone_gow3_logos.py, without the frame generation DLLs next to gow3.exe, when OptiScaler is
installed there (it turns frame generation off) or when the runtime refuses DLSS frame generation. The test prints
the measured pairs.
"""

import re

import pytest

from test_milestone_gow3_logos import XEX, _gow3_exe
from test_milestone_gow3_upscaler import run_gow3

MIN_REAL = 500
MIN_RATIO = 0.95
DLLS = {"fsr": ["amd_fidelityfx_framegeneration_dx12.dll"], "dlss": ["gow2_dlss.dll", "nvngx_dlssg.dll"]}

pytestmark = pytest.mark.skipif(not XEX.is_file(), reason=f"{XEX} missing (extract the GoW3 disc first)")


@pytest.mark.gioco
@pytest.mark.parametrize("generator", ["fsr", "dlss"])
def test_milestone_gow3_frame_gen(tmp_path, generator):
    for dll in DLLS[generator]:
        if not (_gow3_exe().parent / dll).is_file():
            pytest.skip(f"{dll} not next to gow3.exe")
    log, lines, tail = run_gow3(tmp_path, "", ["--frame-rate", "60", "--vsync", "off", "--frame-generation", generator],
                                shots=())
    if "[optiscaler] frame generation turned off: OptiScaler installed" in lines:
        pytest.skip("OptiScaler (dxgi.dll) next to gow3.exe turns frame generation off: uninstall it to run this test")
    refused = [l for l in lines if "frame generation not available" in l]
    if refused:
        pytest.skip("frame generation refused on this machine: " + refused[0])
    pairs = [(int(m.group(1)), int(m.group(2))) for l in lines
             if (m := re.search(r"\[fg\] last 10 s: real (\d+), generated (\d+)", l))]
    print(f"{generator}: pairs (real, generated) {pairs}")
    assert [l for l in lines if l.startswith("[fg] generation on at frame ")], \
        f"frame generation never turned on (no capture of the post chain?):\n{tail}"
    good = [p for p in pairs if p[0] >= MIN_REAL and p[1] >= MIN_RATIO * p[0]]
    assert len(good) >= 2, f"fewer than 2 windows with real >= {MIN_REAL} and generated >= {MIN_RATIO} real: {pairs}"
