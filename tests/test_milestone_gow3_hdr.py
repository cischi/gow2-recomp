"""HDR10 on GoW3: not available, refused by the runtime instead of running a no-op.

The HDR reconstruction (gpu/hdr_passes.h) reproduces GoW2's uber post tonemapper; GoW3's final draw is another
(filmic curve + 3D LUT), so its profile has `post.hdr = false` and the launcher greys the option. A 20 s run with
`--hdr force` (HDR even on an SDR display): exit 0, no `crash` or `device removed` line, the line
`[hdr] HDR not available in this game (no reconstruction of its post processing): SDR`, no `[hdr] HDR10:` line and
no `, HDR10` swap chain line (without the profile check the swap chain turned HDR10 with a zero residual).

Skipped like test_milestone_gow3_logos.py.
"""

import pytest

from test_milestone_gow3_logos import XEX
from test_milestone_gow3_upscaler import run_gow3

pytestmark = pytest.mark.skipif(not XEX.is_file(), reason=f"{XEX} missing (extract the GoW3 disc first)")


@pytest.mark.gioco
def test_milestone_gow3_hdr_refused(tmp_path):
    log, lines, tail = run_gow3(tmp_path, "", ["--hdr", "force"], run_seconds=20, shots=())
    assert "[hdr] HDR not available in this game (no reconstruction of its post processing): SDR" in lines, tail
    assert not [l for l in lines if l.startswith("[hdr] HDR10:")], tail
    assert not [l for l in lines if ", HDR10" in l and not l.startswith("[hdr]")], tail  # swap chain line
