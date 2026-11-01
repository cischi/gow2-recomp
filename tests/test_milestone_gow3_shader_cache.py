"""GoW3: Xenia's persistent shader/pipeline storage is written on a first run and loaded on the next.

Command (twice, same user data): gow3.exe --data game_data_gow3 --user-data <tmp>/user_data
--run-seconds 30 --audio null, cwd = tmp. 30 s reach past the logos: the title screen draws
its first ~40 pipelines at ~21 s (measured 2026-10-28).
Criterion: both runs exit 0 without a `crash` line; run 1 starts with an empty storage and
leaves <user_data>/shaders/shareable/4D5308AB.xsh and 4D5308AB.*.d3d12.xpso with entries
(more than their header); run 2 loads shaders and creates the stored pipelines at startup
("Pipeline cache loaded: N created", N > 0) and the load does not hold the first frame
back (the `[gpu] shader storage` line reports < 2000 ms; ~8 ms measured).
Skipped like test_milestone_gow3_logos.py.
"""

import re
import subprocess

import pytest

from test_milestone_gow3_logos import GAME_DATA, XEX, _gow3_exe
from test_milestone_gpu import _require_adapter

RUN_SECONDS = 30
TIMEOUT_S = 120
TITLE_ID = "4D5308AB"

pytestmark = pytest.mark.skipif(not XEX.is_file(), reason=f"{XEX} missing (extract the GoW3 disc first)")


def _run(exe, tmp_path, user_data) -> str:
    proc = subprocess.run([str(exe), "--data", str(GAME_DATA), "--user-data", str(user_data),
                           "--run-seconds", str(RUN_SECONDS), "--audio", "null"],
                          cwd=tmp_path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=TIMEOUT_S)
    log = (tmp_path / "gow3.log").read_text(encoding="utf-8", errors="replace")
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{log[-4000:]}"
    assert not [l for l in log.splitlines() if l.startswith("crash")], f"crash line in the log:\n{log[-4000:]}"
    return log


@pytest.mark.gioco
def test_milestone_gow3_shader_cache(tmp_path):
    exe = _gow3_exe()
    _require_adapter(exe)
    user_data = tmp_path / "user_data"
    log = _run(exe, tmp_path, user_data)
    assert "Loaded 0 shaders from storage" in log, f"first run did not start from an empty storage:\n{log[-4000:]}"
    shareable = user_data / "shaders" / "shareable"
    xsh = shareable / f"{TITLE_ID}.xsh"
    xpso = list(shareable.glob(f"{TITLE_ID}.*.d3d12.xpso"))
    assert xsh.is_file() and xsh.stat().st_size > 8, f"no shaders stored in {xsh}"
    assert len(xpso) == 1 and xpso[0].stat().st_size > 12, f"no pipelines stored in {shareable}: {xpso}"

    log = _run(exe, tmp_path, user_data)
    shaders = re.search(r"Loaded (\d+) shaders from storage", log)
    pipelines = re.search(r"Pipeline cache loaded: (\d+) created", log)
    load = re.search(rf"\[gpu\] shader storage {TITLE_ID}: loaded in (\d+) ms", log)
    print(f"shaders={shaders and shaders.group(1)} pipelines={pipelines and pipelines.group(1)} "
          f"load_ms={load and load.group(1)}")
    assert shaders and int(shaders.group(1)) > 0, f"second run loaded no shaders:\n{log[-4000:]}"
    assert pipelines and int(pipelines.group(1)) > 0, f"second run created no stored pipelines:\n{log[-4000:]}"
    assert load and int(load.group(1)) < 2000, f"storage load missing or too slow:\n{log[-4000:]}"
