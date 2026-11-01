"""cmake/Recompile.cmake rejects XenonRecomp's skip_* / *_as_local options.

gow2_kernel, gow2_gpu and gow2_gpu_fake compile PPCContext without the generated
ppc_config.h (runtime/kernel/ppc_layout.h): an enabled option would change the
layout of the recompiled code only. The check runs in script mode
before launching XenonRecomp; here it is exercised with a test TOML and a
nonexistent XenonRecomp (if the check did not stop the script, it would fail
later with a different message).
"""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "cmake" / "Recompile.cmake"
CMAKE = shutil.which("cmake")

pytestmark = pytest.mark.skipif(CMAKE is None, reason="cmake not on PATH")


def _run_script(tmp_path: Path, toml_text: str):
    toml = tmp_path / "gow2.toml"
    toml.write_text(toml_text, encoding="utf-8")
    xex = tmp_path / "default.xex"
    xex.write_bytes(b"XEX2")
    return subprocess.run(
        [CMAKE,
         f"-DXENON_RECOMP={tmp_path / 'does_not_exist.exe'}",
         f"-DCONFIG={toml}",
         f"-DPPC_CONTEXT={tmp_path / 'ppc_context.h'}",
         f"-DGENERATED_DIR={tmp_path / 'generated'}",
         f"-DLOG={tmp_path / 'recomp.log'}",
         f"-DPYTHON={sys.executable}",
         f"-DCHECKER={tmp_path / 'checker.py'}",
         f"-DXEX={xex}",
         "-P", str(SCRIPT)],
        capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)


@pytest.mark.parametrize("option", ["skip_lr", "skip_msr", "ctr_as_local", "non_volatile_as_local"])
def test_layout_changing_option_is_rejected(tmp_path, option):
    proc = _run_script(tmp_path, f"[main]\nskip_lr = false\n{option} = true\n")
    assert proc.returncode != 0
    assert "options not supported by the HLE kernel" in proc.stderr
    assert option in proc.stderr
    assert not (tmp_path / "generated").exists()  # stopped before XenonRecomp


def test_all_false_passes_the_check(tmp_path):
    proc = _run_script(tmp_path, "[main]\nskip_lr = false\ncr_as_local = false\n")
    assert "options not supported" not in proc.stderr


def test_repository_config_passes_the_check():
    text = (ROOT / "games" / "gow2" / "gow2.toml").read_text(encoding="utf-8")
    import re
    bad = re.findall(r"^\s*(skip_\w+|\w+_as_local)\s*=\s*true", text, re.MULTILINE)
    assert bad == []
