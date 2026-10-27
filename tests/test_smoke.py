"""Smoke test of gow2.exe (recompiled code + minimal runtime).

gow2.exe is looked for in build/clang-release/runtime/ (clang-release preset,
`gow2` target); the GOW2_EXE environment variable can point to another
executable. The tests are skipped if game_data/default.xex is missing: without game
data there is not even the recompiled code gow2.exe is built from.

The real game does not terminate (the imports are stubs that return 0): it is
left running for at most 15 s and then killed; only the log matters.
"""

import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
GAME_DATA = ROOT / "game_data"
XEX = GAME_DATA / "default.xex"
TIMEOUT_S = 15

pytestmark = pytest.mark.skipif(not XEX.is_file(), reason=f"{XEX} missing (extract the disc first)")


def _gow2_exe() -> Path:
    override = os.environ.get("GOW2_EXE")
    exe = Path(override) if override else ROOT / "build" / "clang-release" / "runtime" / "gow2.exe"
    if not exe.is_file():
        pytest.skip(f"gow2.exe not built ({exe}): run `python tools/gow2.py build --iso <iso>`")
    return exe


def test_missing_xex_exits_2(tmp_path):
    empty = tmp_path / "empty data [x]"
    empty.mkdir()
    proc = subprocess.run([str(_gow2_exe()), "--gpu", "fake", "--data", str(empty)], cwd=tmp_path,
                          capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=60)
    assert proc.returncode == 2, proc.stderr
    assert "default.xex not found" in proc.stderr


def test_unknown_language_exits_2(tmp_path):
    proc = subprocess.run([str(_gow2_exe()), "--language", "klingon"], cwd=tmp_path,
                          capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=60)
    assert proc.returncode == 2, proc.stderr
    assert "unknown language: klingon" in proc.stderr
    assert not (tmp_path / "gow2.log").exists()  # exits before starting the runtime


def test_language_option_requires_known_code_before_data_check(tmp_path):
    empty = tmp_path / "empty"
    empty.mkdir()
    proc = subprocess.run([str(_gow2_exe()), "--gpu", "fake", "--language", "IT", "--data", str(empty)], cwd=tmp_path,
                          capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=60)
    assert proc.returncode == 2, proc.stderr
    assert "default.xex not found" in proc.stderr  # "IT" is accepted (uppercase allowed)


@pytest.mark.parametrize("value", ["vulkan", "D3D12", "Fake"])
def test_unknown_gpu_backend_exits_2(tmp_path, value):
    proc = subprocess.run([str(_gow2_exe()), "--gpu", value], cwd=tmp_path,
                          capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=60)
    assert proc.returncode == 2, proc.stderr
    assert f"unknown GPU backend: {value}" in proc.stderr
    assert not (tmp_path / "gow2.log").exists()  # exits before starting the runtime


def test_real_game_reaches_entry_and_calls_kernel(tmp_path):
    exe = _gow2_exe()
    proc = subprocess.Popen([str(exe), "--gpu", "fake", "--data", str(GAME_DATA)], cwd=tmp_path,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        rc = proc.wait(timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        rc = None
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    lines = log.splitlines()
    entry = next((i for i, l in enumerate(lines) if l.startswith("entry 0x")), None)
    assert entry is not None, f"'entry 0x' missing from the log (rc={rc}):\n{log[-4000:]}"
    assert any(l.startswith("[kernel] ") for l in lines[entry + 1:]), \
        f"no [kernel] call after the entry (rc={rc}):\n{log[-4000:]}"
    # a crash must go through the handler (exit 3 with the `crash host=` line)
    if rc is not None and rc != 0:
        assert rc == 3 and any(l.startswith("crash host=0x") for l in lines), \
            f"unexpected exit {rc}:\n{log[-4000:]}"


def _data_copy(root: Path) -> Path:
    """Data folder with only default.xex (hard link if possible)."""
    data = root / "data"
    data.mkdir(parents=True)
    try:
        os.link(XEX, data / "default.xex")
    except OSError:
        import shutil
        shutil.copyfile(XEX, data / "default.xex")
    return data


def _assert_clean_error_exit(proc, cwd: Path):
    log = (cwd / "gow2.log").read_text(encoding="utf-8", errors="replace")
    lines = log.splitlines()
    assert proc.returncode == 1, f"exit {proc.returncode}:\n{log[-4000:]}"
    assert any("cannot create" in l for l in lines), log[-4000:]
    assert not any(l.startswith("crash") for l in lines), f"crash line in the log:\n{log[-4000:]}"


def test_user_data_not_creatable_exits_1_without_crash(tmp_path):
    # user_data next to the data is a file: create_directories fails after
    # the KeTimeStampBundle thread has started, which must be stopped before exiting.
    data = _data_copy(tmp_path)
    (tmp_path / "user_data").write_bytes(b"not a folder")
    proc = subprocess.run([str(_gow2_exe()), "--gpu", "fake", "--data", str(data)], cwd=tmp_path,
                          capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
    _assert_clean_error_exit(proc, tmp_path)


def test_user_data_option_not_creatable_exits_1_without_crash(tmp_path):
    blocker = tmp_path / "file"
    blocker.write_bytes(b"x")
    proc = subprocess.run([str(_gow2_exe()), "--gpu", "fake", "--data", str(GAME_DATA), "--user-data", str(blocker / "ud")],
                          cwd=tmp_path, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=60)
    _assert_clean_error_exit(proc, tmp_path)
