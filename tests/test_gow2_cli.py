"""Tests of the tools/gow2.py orchestrator (arguments only: no build)."""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GOW2_PY = ROOT / "tools" / "gow2.py"


def _run(*args):
    return subprocess.run([sys.executable, str(GOW2_PY), *args], capture_output=True,
                          text=True, encoding="utf-8", errors="replace", timeout=60)


def test_help_lists_build():
    proc = _run("--help")
    assert proc.returncode == 0
    assert "build" in proc.stdout


def test_missing_iso_fails_with_path(tmp_path):
    iso = tmp_path / "does not exist [x]" / "DVD.iso"
    proc = _run("build", "--iso", str(iso))
    assert proc.returncode != 0
    assert str(iso) in proc.stderr


def test_build_rejects_unknown_game(tmp_path):
    proc = _run("build", "--game", "gow4", "--iso", str(tmp_path / "DVD.iso"))
    assert proc.returncode == 2


def test_build_gow3_missing_iso(tmp_path):
    proc = _run("build", "--game", "gow3", "--iso", str(tmp_path / "missing.iso"))
    assert proc.returncode == 1
    assert "error: ISO not found" in proc.stderr


def test_build_help_lists_game():
    proc = _run("build", "--help")
    assert proc.returncode == 0
    assert "--game" in proc.stdout


def test_build_requires_iso():
    proc = _run("build")
    assert proc.returncode != 0


def test_run_hint_mentions_runtime_options():
    hint = _gow2().RUN_HINT
    for opt in ("--run-seconds", "--trace", "--language", "--data", "--user-data", "--gpu",
                "--screenshot", "--gpu-cvar", "--gpu-probe"):
        assert opt in hint


def _gow2():
    import importlib.util
    spec = importlib.util.spec_from_file_location("gow2_tool", GOW2_PY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_merge_env_collapses_path_case():
    gow2 = _gow2()
    base = {"PATH": "old", "FOO": "1"}
    merged = gow2.merge_env(base, "Path=new;old\nINCLUDE=inc\n")
    assert merged["PATH"] == "new;old"
    assert [k for k in merged if k.upper() == "PATH"] == ["PATH"]
    assert merged["FOO"] == "1" and merged["INCLUDE"] == "inc"


def test_merge_env_equals_in_values_and_pseudo_vars():
    gow2 = _gow2()
    lines = ["=C:=C:" + chr(92) + "dir", "=ExitCode=00000000", "X=a=b=c", "INCLUDE=i", "LIB=l"]
    text = "\r\n".join(lines) + "\r\n"
    merged = gow2.merge_env({}, text)
    assert merged["X"] == "a=b=c"
    assert merged["LIB"] == "l"
    assert not any(k.startswith("=") or k == "" for k in merged)


def test_merge_env_requires_include():
    import pytest
    gow2 = _gow2()
    with pytest.raises(RuntimeError, match="INCLUDE"):
        gow2.merge_env({}, "PATH=x\n")


def test_extraction_complete_depends_on_marker(tmp_path):
    gow2 = _gow2()
    assert not gow2.extraction_complete(tmp_path)
    (tmp_path / "default.xex").write_bytes(b"XEX2")
    assert not gow2.extraction_complete(tmp_path)  # interrupted extraction: no marker
    (tmp_path / ".extract_ok").write_text("1\n")
    assert gow2.extraction_complete(tmp_path)
    (tmp_path / "default.xex").unlink()
    assert not gow2.extraction_complete(tmp_path)


def test_gow2_exe_usage_lists_gpu_options(tmp_path):
    """The usage printed by gow2.exe (unknown argument: exit 2) lists the GPU options."""
    import pytest
    exe = ROOT / "build" / "clang-release" / "runtime" / "gow2.exe"
    if not exe.is_file():
        pytest.skip(f"gow2.exe not built ({exe})")
    proc = subprocess.run([str(exe), "--help"], cwd=tmp_path, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=60)
    assert proc.returncode == 2
    for opt in ("--gpu fake|d3d12", "--screenshot", "--gpu-cvar", "--gpu-probe"):
        assert opt in proc.stderr


def _gow2_exe():
    import pytest
    exe = ROOT / "build" / "clang-release" / "runtime" / "gow2.exe"
    if not exe.is_file():
        pytest.skip(f"gow2.exe not built ({exe})")
    return exe


def test_gow2_exe_usage_lists_audio_options(tmp_path):
    """The usage of gow2.exe lists --audio, --mute and --audio-capture."""
    proc = subprocess.run([str(_gow2_exe()), "--help"], cwd=tmp_path, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=60)
    assert proc.returncode == 2
    for opt in ("--audio xaudio2|null", "--mute", "--audio-capture"):
        assert opt in proc.stderr


def test_gow2_exe_rejects_unknown_audio_sink(tmp_path):
    """--audio accepts only xaudio2 and null: anything else -> exit 2 before looking for the data."""
    for value in ("wasapi", "NULL", ""):
        proc = subprocess.run([str(_gow2_exe()), "--audio", value, "--data", str(tmp_path / "missing")],
                              cwd=tmp_path, capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=60)
        assert proc.returncode == 2, value
        assert f"--audio invalid: {value} (xaudio2|null)" in proc.stderr


def test_run_hint_mentions_audio_options():
    hint = _gow2().RUN_HINT
    for opt in ("--audio", "--mute", "--audio-capture"):
        assert opt in hint


def test_gow2_exe_usage_lists_input_script(tmp_path):
    proc = subprocess.run([str(_gow2_exe()), "--help"], cwd=tmp_path, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=60)
    assert proc.returncode == 2
    assert "--input-script" in proc.stderr


def test_gow2_exe_rejects_bad_input_script(tmp_path):
    """invalid --input-script (lowercase button...) -> exit 2 before looking for the data."""
    for value in ("40:start", "40", "", "40:A+0"):
        proc = subprocess.run([str(_gow2_exe()), "--input-script", value, "--data", str(tmp_path / "missing")],
                              cwd=tmp_path, capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=60)
        assert proc.returncode == 2, value
        assert f"--input-script invalid: {value}" in proc.stderr
        assert not (tmp_path / "gow2.log").exists()


def test_run_hint_mentions_input_script():
    assert "--input-script" in _gow2().RUN_HINT


def test_gow2_exe_unopenable_audio_capture_exits_2_before_window(tmp_path):
    """Unopenable --audio-capture: exit 2 immediately, before the window and GPU (no window)."""
    import time
    data = tmp_path / "data"
    data.mkdir()
    (data / "default.xex").write_bytes(b"")  # it only needs to exist: the capture is opened before everything else
    bad = tmp_path / "nonexistent_folder" / "a.wav"
    t0 = time.monotonic()
    proc = subprocess.run([str(_gow2_exe()), "--data", str(data), "--user-data", str(tmp_path / "ud"),
                           "--audio", "null", "--audio-capture", str(bad)],
                          cwd=tmp_path, env={**os.environ, "GOW2_SKIP_XEX_HASH": "1"}, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=30)
    elapsed = time.monotonic() - t0
    assert proc.returncode == 2, proc.stderr
    assert elapsed < 10, elapsed
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    assert "cannot open the --audio-capture file" in log
    assert "window" not in log and "D3D12" not in log


def test_gow2_exe_rejects_wrong_xex(tmp_path):
    data = tmp_path / "data"
    data.mkdir()
    (data / "default.xex").write_bytes(b"XEX2")
    env = {k: v for k, v in os.environ.items() if k != "GOW2_SKIP_XEX_HASH"}
    proc = subprocess.run([str(_gow2_exe()), "--data", str(data), "--user-data", str(tmp_path / "ud"),
                           "--audio", "null"], cwd=tmp_path, env=env, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=30)
    assert proc.returncode == 2, proc.stderr
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    assert "does not match the supported edition" in log


def _run_exe(*args):
    return subprocess.run([str(_gow2_exe()), *args], capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=60)


def test_exe_config_with_invalid_mode_exits_2(tmp_path):
    cfg = tmp_path / "gow2.toml"
    cfg.write_text('[video]\nmode = "x"\n', encoding="utf-8")
    proc = _run_exe("--config", str(cfg))
    assert proc.returncode == 2
    assert "mode" in proc.stderr


def test_exe_config_from_env(tmp_path, monkeypatch):
    cfg = tmp_path / "gow2.toml"
    cfg.write_text('[video]\nmode = "x"\n', encoding="utf-8")
    monkeypatch.setenv("GOW2_CONFIG", str(cfg))
    proc = _run_exe("--resolution", "10x10")  # without the env var it exits because of --resolution
    assert proc.returncode == 2
    assert "mode" in proc.stderr


def test_exe_invalid_resolution_exits_2():
    proc = _run_exe("--resolution", "10x10")
    assert proc.returncode == 2
    assert "--resolution" in proc.stderr


def test_exe_explicit_config_missing_exits_2(tmp_path):
    proc = _run_exe("--config", str(tmp_path / "missing.toml"))
    assert proc.returncode == 2
    assert "cannot read" in proc.stderr


def test_exe_invalid_upscaler_exits_2():
    proc = _run_exe("--upscaler", "xyz")
    assert proc.returncode == 2
    assert "--upscaler invalid" in proc.stderr


def test_exe_invalid_dlss_mode_exits_2():
    proc = _run_exe("--dlss-mode", "xyz")
    assert proc.returncode == 2
    assert "--dlss-mode invalid" in proc.stderr


def test_exe_invalid_frame_generation_exits_2():
    proc = _run_exe("--frame-generation", "xyz")
    assert proc.returncode == 2
    assert "--frame-generation invalid" in proc.stderr


def test_exe_frame_generation_dlss_accepted():
    proc = _run_exe("--frame-generation", "dlss", "--resolution", "10x10")
    assert proc.returncode == 2
    assert "--resolution" in proc.stderr
    assert "--frame-generation" not in proc.stderr


def test_exe_dlss_preset_j_accepted():
    # j is valid: the error comes later, from the resolution.
    proc = _run_exe("--dlss-preset", "j", "--resolution", "10x10")
    assert proc.returncode == 2
    assert "--resolution" in proc.stderr
    assert "--dlss-preset" not in proc.stderr


def test_exe_invalid_dlss_preset_exits_2():
    proc = _run_exe("--dlss-preset", "a")
    assert proc.returncode == 2
    assert "--dlss-preset invalid" in proc.stderr


def test_gow2_exe_launcher_with_help_shows_usage(tmp_path):
    """--launcher with --help does not open the launcher: it prints the usage (which lists --launcher) and exits."""
    proc = subprocess.run([str(_gow2_exe()), "--launcher", "--help"], cwd=tmp_path, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=10)
    assert proc.returncode == 2
    assert "[--launcher]" in proc.stderr


def test_gow2_exe_xma_decoder_option(tmp_path):
    """--xma-decoder new|old: the usage lists it, any other value -> exit 2 before looking for the data."""
    proc = subprocess.run([str(_gow2_exe()), "--help"], cwd=tmp_path, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=60)
    assert "--xma-decoder new|old" in proc.stderr
    for value in ("master", "NEW", ""):
        proc = subprocess.run([str(_gow2_exe()), "--xma-decoder", value, "--data", str(tmp_path / "missing")],
                              cwd=tmp_path, capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=60)
        assert proc.returncode == 2, value
        assert f"--xma-decoder invalid: {value} (new|old)" in proc.stderr


def test_exe_dlss_nr_option_removed_exits_2():
    """--dlss-nr no longer exists (neural rendering moved to OptiScaler): unknown argument."""
    proc = _run_exe("--dlss-nr", "on")
    assert proc.returncode == 2


def test_gow2_exe_is_windows_subsystem():
    """gow2.exe is a GUI exe (no console on double click); the console only opens from a setting."""
    data = _gow2_exe().read_bytes()
    pe = int.from_bytes(data[0x3C:0x40], "little")
    assert data[pe:pe + 4] == b"PE" + bytes(2)
    # Optional header: offset 24 from "PE"; Subsystem at +68 (same in PE32 and PE32+).
    assert int.from_bytes(data[pe + 24 + 68:pe + 24 + 70], "little") == 2  # IMAGE_SUBSYSTEM_WINDOWS_GUI
