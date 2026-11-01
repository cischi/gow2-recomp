"""gears.exe (game dispatcher): opens its window and exits after N frames; bad argument -> exit 2.

Skipped without gears.exe. The D3D11 window falls back to WARP, so no hardware adapter is required.
"""

import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent


def _gears_exe() -> Path:
    override = os.environ.get("GEARS_EXE")
    exe = Path(override) if override else ROOT / "build" / "clang-release" / "runtime" / "gears.exe"
    if not exe.is_file():
        pytest.skip(f"gears.exe not built ({exe})")
    return exe


def _run(args, cwd):
    return subprocess.run([str(_gears_exe()), *args], cwd=cwd, capture_output=True, timeout=30)


def test_max_frames_exits_zero_from_other_cwd(tmp_path):
    assert _run(["--max-frames", "5"], tmp_path).returncode == 0


@pytest.mark.parametrize("args", [["--bogus"], ["--max-frames", "x"], ["--max-frames", "0"], ["--max-frames"]])
def test_bad_arguments_exit_two(args, tmp_path):
    assert _run(args, tmp_path).returncode == 2
