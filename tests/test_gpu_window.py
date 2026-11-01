"""gow2.exe with the d3d12 backend (default): window, DXGI presenter, screenshot.

Skipped without game_data/default.xex, without gow2.exe or without a hardware
D3D12 adapter (decided by `gow2.exe --gpu-probe`: exit 0 = adapter).
The screenshot goes to a temporary folder (never in the repository).
"""

import os
import struct
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
GAME_DATA = ROOT / "game_data"
XEX = GAME_DATA / "default.xex"

pytestmark = pytest.mark.skipif(not XEX.is_file(), reason=f"{XEX} missing (extract the disc first)")


def _gow2_exe() -> Path:
    override = os.environ.get("GOW2_EXE")
    exe = Path(override) if override else ROOT / "build" / "clang-release" / "runtime" / "gow2.exe"
    if not exe.is_file():
        pytest.skip(f"gow2.exe not built ({exe}): run `python tools/gow2.py build --iso <iso>`")
    return exe


def _require_adapter(exe: Path) -> str:
    proc = subprocess.run([str(exe), "--gpu-probe"], capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=60)
    if proc.returncode != 0:
        pytest.skip(f"no hardware D3D12 adapter (--gpu-probe exit {proc.returncode})")
    return proc.stdout.strip()


def _summary(log: str, key: str) -> int:
    prefix = f"[summary] {key}="
    values = [line[len(prefix):] for line in log.splitlines() if line.startswith(prefix)]
    assert len(values) == 1, f"{prefix} missing or repeated:\n{log[-4000:]}"
    return int(values[0])


def test_gpu_probe_prints_adapter():
    exe = _gow2_exe()
    name = _require_adapter(exe)
    assert name, "--gpu-probe with exit 0 must print the adapter name"


@pytest.mark.gioco
def test_d3d12_window_presents_and_takes_screenshot(tmp_path):
    exe = _gow2_exe()
    _require_adapter(exe)
    shot = tmp_path / "shots" / "s.bmp"
    shot.parent.mkdir()
    proc = subprocess.run([str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "ud"),
                           "--run-seconds", "20", "--screenshot", f"10:{shot}"],
                          cwd=tmp_path, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=180)
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{log[-4000:]}"
    assert _summary(log, "host_frames_presented") >= 60, log[-4000:]
    assert any(line.startswith("[gpu] screenshot ") and "1280x720 frame host " in line
               for line in log.splitlines()), log[-4000:]
    data = shot.read_bytes()
    assert data[:2] == b"BM"
    width, height = struct.unpack_from("<ii", data, 18)
    assert (width, height) == (1280, 720)
    assert struct.unpack_from("<H", data, 28)[0] == 24
    assert len(data) == 54 + 1280 * 720 * 3


@pytest.mark.gioco
def test_d3d12_repeated_screenshots(tmp_path):
    """--screenshot is repeatable: two 1280x720 BMPs, each from the first frame after its instant."""
    exe = _gow2_exe()
    _require_adapter(exe)
    a, b = tmp_path / "a.bmp", tmp_path / "b.bmp"
    proc = subprocess.run([str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "ud"),
                           "--run-seconds", "20", "--screenshot", f"5:{a}", "--screenshot", f"10:{b}"],
                          cwd=tmp_path, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=180)
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{log[-4000:]}"
    shots = [line for line in log.splitlines() if line.startswith("[gpu] screenshot ")]
    assert len(shots) == 2, log[-4000:]
    frames = [int(line.rsplit(" ", 1)[1]) for line in shots]
    assert frames[0] < frames[1], shots
    for shot in (a, b):
        data = shot.read_bytes()
        assert data[:2] == b"BM"
        assert struct.unpack_from("<ii", data, 18) == (1280, 720)
        assert len(data) == 54 + 1280 * 720 * 3


@pytest.mark.gioco
def test_d3d12_backend_runs_the_game_30_seconds(tmp_path):
    """Xenia's D3D12CommandProcessor runs the game's ring: draws, one presentation per VdSwap."""
    exe = _gow2_exe()
    _require_adapter(exe)
    proc = subprocess.run([str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "ud"),
                           "--run-seconds", "30"],
                          cwd=tmp_path, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=240)
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{log[-4000:]}"
    lines = log.splitlines()
    assert not [line for line in lines if line.startswith("crash ") or " crash " in line], log[-4000:]
    assert not [line for line in lines if "device removed" in line], log[-4000:]
    assert _summary(log, "host_frames_presented") >= 60, log[-4000:]
    # one presentation per VdSwap (at most a few swaps still in the ring at exit)
    assert _summary(log, "host_frames_presented") >= _summary(log, "frames_presented") - 3, log[-4000:]
    assert _summary(log, "gpu_draws") > 0, log[-4000:]


def test_d3d12_adapter_out_of_range_exits_5_suggesting_fake(tmp_path):
    exe = _gow2_exe()
    _require_adapter(exe)
    proc = subprocess.run([str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "ud"),
                           "--gpu-cvar", "d3d12_adapter=99"],
                          cwd=tmp_path, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=120)
    assert proc.returncode == 5, proc.stderr[-4000:]
    assert "--gpu fake" in proc.stderr
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    assert "[summary] host_frames_presented=0" in log.splitlines()


@pytest.mark.parametrize("cvar", ["does_not_exist=1", "d3d12_adapter=abc", "d3d12_adapter", "=1"])
def test_bad_gpu_cvar_exits_2(tmp_path, cvar):
    proc = subprocess.run([str(_gow2_exe()), "--gpu-cvar", cvar], cwd=tmp_path, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=60)
    assert proc.returncode == 2, proc.stderr
    assert f"--gpu-cvar invalid: {cvar}" in proc.stderr
    assert not (tmp_path / "gow2.log").exists()  # exits before starting the runtime


@pytest.mark.parametrize("args", [["--aa", "smaa"], ["--sharpening", "2"], ["--sharpening", "on"],
                                  ["--internal-resolution", "0"], ["--internal-resolution", "4"],
                                  ["--internal-resolution", "x"], ["--enhanced-shaders", "sometimes"]])
def test_bad_post_processing_option_exits_2(tmp_path, args):
    proc = subprocess.run([str(_gow2_exe()), *args], cwd=tmp_path, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=60)
    assert proc.returncode == 2, proc.stderr
    assert f"{args[0]} invalid: {args[1]}" in proc.stderr
    assert not (tmp_path / "gow2.log").exists()


@pytest.mark.parametrize("flag, value", [("--frame-rate", "90"), ("--vrr", "yes"),
                                         ("--frame-generation", "maybe"), ("--aspect", "4:3"),
                                         ("--hdr", "maybe"), ("--hdr-peak-nits", "50"),
                                         ("--hdr-paper-white-nits", "abc")])
def test_bad_frame_rate_or_vrr_exits_2(tmp_path, flag, value):
    proc = subprocess.run([str(_gow2_exe()), flag, value], cwd=tmp_path, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=60)
    assert proc.returncode == 2, proc.stderr
    assert f"{flag} invalid: {value}" in proc.stderr
    assert not (tmp_path / "gow2.log").exists()


@pytest.mark.parametrize("flag, value", [("--shadow-quality", "ultra"), ("--dynamic-shadows", "no")])
def test_bad_engine_option_exits_2(tmp_path, flag, value):
    proc = subprocess.run([str(_gow2_exe()), flag, value], cwd=tmp_path, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=60)
    assert proc.returncode == 2, proc.stderr
    assert f"{flag} invalid: {value}" in proc.stderr
    assert not (tmp_path / "gow2.log").exists()


def test_bad_anisotropic_exits_2(tmp_path):
    proc = subprocess.run([str(_gow2_exe()), "--anisotropic", "3"], cwd=tmp_path, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=60)
    assert proc.returncode == 2, proc.stderr
    assert "--anisotropic invalid: 3" in proc.stderr
    assert not (tmp_path / "gow2.log").exists()


@pytest.mark.gioco
@pytest.mark.parametrize("extra, expected", [
    (["--anisotropic", "8"], "anisotropic 8x"),
    ([], "anisotropic 16x"),
    (["--anisotropic", "0"], "anisotropic game default"),
    (["--anisotropic", "8", "--gpu-cvar", "anisotropic_override=3"], "anisotropic 4x"),
    (["--gpu-cvar", "anisotropic_override=0"], "anisotropic off"),
    (["--gpu-cvar", "anisotropic_override=1"], "anisotropic 1x"),
])
def test_anisotropic_precedence(tmp_path, extra, expected):
    """--anisotropic < --gpu-cvar anisotropic_override (startup log line)."""
    exe = _gow2_exe()
    _require_adapter(exe)
    proc = subprocess.run([str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "ud"),
                           "--run-seconds", "5", *extra],
                          cwd=tmp_path, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=120)
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{log[-4000:]}"
    assert expected in log, log[-4000:]


@pytest.mark.gioco
def test_anisotropic_from_file(tmp_path):
    """[video] anisotropic = 4 in --config reaches the startup log."""
    exe = _gow2_exe()
    _require_adapter(exe)
    cfg = tmp_path / "gow2.toml"
    cfg.write_text("[video]\nanisotropic = 4\n", encoding="utf-8")
    proc = subprocess.run([str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "ud"),
                           "--config", str(cfg), "--run-seconds", "5"],
                          cwd=tmp_path, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=120)
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{log[-4000:]}"
    assert "anisotropic 4x" in log, log[-4000:]


def _run_frame_rate(tmp_path, extra) -> list[str]:
    exe = _gow2_exe()
    _require_adapter(exe)
    proc = subprocess.run([str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "ud"),
                           "--run-seconds", "8", *extra],
                          cwd=tmp_path, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=120)
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{log[-4000:]}"
    return log.splitlines()


@pytest.mark.gioco
def test_patches_on_with_the_real_xex(tmp_path):
    """The four [patches] patches find the expected bytes in the real XEX."""
    exe = _gow2_exe()
    _require_adapter(exe)
    cfg = tmp_path / "gow2.toml"
    keys = ["show_fps", "disable_motion_blur", "disable_lens_flares", "disable_bloom"]
    cfg.write_text("[patches]\n" + "".join(f"{k} = true\n" for k in keys), encoding="utf-8")
    proc = subprocess.run([str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "ud"),
                           "--config", str(cfg), "--run-seconds", "5"],
                          cwd=tmp_path, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=120)
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{log[-4000:]}"
    for k in keys:
        assert f"[patch] {k}: turned on" in log, log[-4000:]


@pytest.mark.gioco
def test_frame_rate_log_default_30(tmp_path):
    """Default: original 30, no monitor query."""
    lines = _run_frame_rate(tmp_path, [])
    assert "[frame] frame rate 30 (original)" in lines, "\n".join(lines[-80:])
    assert not any(line.startswith("[frame] monitor") for line in lines), "\n".join(lines[-80:])


@pytest.mark.gioco
def test_frame_rate_log_60(tmp_path):
    lines = _run_frame_rate(tmp_path, ["--frame-rate", "60"])
    assert "[frame] frame rate 60: present interval 1, Unlock FPS on, vblank 60 Hz" in lines, \
        "\n".join(lines[-80:])
    assert "[patch] frame_rate.interval: turned on" in lines, "\n".join(lines[-80:])
    assert "[patch] frame_rate.unlock: turned on" in lines, "\n".join(lines[-80:])


@pytest.mark.gioco
def test_frame_rate_log_144(tmp_path):
    lines = _run_frame_rate(tmp_path, ["--frame-rate", "144"])
    assert "[frame] frame rate 144: present interval 1, Unlock FPS on, vblank 144 Hz" in lines, \
        "\n".join(lines[-80:])


@pytest.mark.gioco
def test_frame_rate_log_unlimited_vrr_on(tmp_path):
    lines = _run_frame_rate(tmp_path, ["--frame-rate", "unlimited", "--vrr", "on", "--vsync", "off"])
    assert any(line.startswith("[frame] monitor ") and "VRR active (vrr = on), limit " in line for line in lines), \
        "\n".join(lines[-80:])


@pytest.mark.parametrize("arg", ["45", "-1:a.bmp", "86401:a.bmp", ":a.bmp"])
def test_bad_screenshot_exits_2(tmp_path, arg):
    proc = subprocess.run([str(_gow2_exe()), "--screenshot", arg], cwd=tmp_path, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=60)
    assert proc.returncode == 2, proc.stderr
    assert f"--screenshot invalid: {arg}" in proc.stderr
    assert not (tmp_path / "gow2.log").exists()


def test_shader_capture_with_fake_gpu_exits_2(tmp_path):
    proc = subprocess.run([str(_gow2_exe()), "--gpu", "fake", "--shader-capture"], cwd=tmp_path, capture_output=True,
                          text=True, encoding="utf-8", errors="replace", timeout=60)
    assert proc.returncode == 2, proc.stderr
    assert "--shader-capture invalid with --gpu fake (no presenter)" in proc.stderr
    assert not (tmp_path / "gow2.log").exists()


def _windows_of(pid: int) -> list[int]:
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    found: list[int] = []
    proc_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(proc_type(visit), 0)
    return found


@pytest.mark.gioco
def test_closing_the_window_exits_0_with_summary(tmp_path):
    """WM_CLOSE to the game window (like the close button): exit 0 and summary."""
    import ctypes
    import time
    exe = _gow2_exe()
    _require_adapter(exe)
    proc = subprocess.Popen([str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "ud"),
                             "--run-seconds", "120"],
                            cwd=tmp_path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + 60
        windows: list[int] = []
        while not windows and time.monotonic() < deadline and proc.poll() is None:
            time.sleep(0.2)
            windows = _windows_of(proc.pid)
        assert windows, "no visible window of the process"
        time.sleep(5)  # the game presents a few frames
        WM_CLOSE = 0x0010
        from ctypes import wintypes
        post = ctypes.WinDLL("user32").PostMessageW
        post.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        post.restype = wintypes.BOOL
        assert post(windows[0], WM_CLOSE, 0, 0)
        rc = proc.wait(timeout=30)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    assert rc == 0, f"exit {rc}:\n{log[-4000:]}"
    assert "gow2: window closed" in log
    assert _summary(log, "host_frames_presented") > 0, log[-4000:]


@pytest.mark.gioco
@pytest.mark.parametrize("extra, scaled", [
    (["--internal-resolution", "2"], 2),
    (["--internal-resolution", "3"], 3),
    (["--internal-resolution", "2", "--gpu-cvar", "draw_resolution_scale_x=1", "--gpu-cvar", "draw_resolution_scale_y=1"], 0),
    (["--config", "cfg"], 0),  # file: internal_resolution = 2 and [gpu].cvars at 1: the hand-set cvars win
])
def test_internal_resolution_precedence(tmp_path, extra, scaled):
    """internal_resolution from the file < --internal-resolution < [gpu].cvars < --gpu-cvar (startup log line)."""
    exe = _gow2_exe()
    _require_adapter(exe)
    if "cfg" in extra:
        cfg = tmp_path / "cfg.toml"
        cfg.write_text('[video]\ninternal_resolution = 2\n[gpu]\ncvars = { draw_resolution_scale_x = "1", draw_resolution_scale_y = "1" }\n')
        extra = ["--config", str(cfg)]
    proc = subprocess.run([str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "ud"),
                           "--run-seconds", "5", *extra],
                          cwd=tmp_path, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=120)
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{log[-4000:]}"
    assert [n for n in (2, 3) if f"internal resolution {n}x" in log] == ([scaled] if scaled else []), log[-4000:]


def _run_engine(tmp_path, extra, seconds="8"):
    exe = _gow2_exe()
    _require_adapter(exe)
    proc = subprocess.run([str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "ud"),
                           "--run-seconds", seconds, *extra],
                          cwd=tmp_path, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=180)
    log = (tmp_path / "gow2.log").read_text(encoding="utf-8", errors="replace")
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{log[-4000:]}"
    return log.splitlines()


@pytest.mark.gioco
def test_engine_log_default_is_original(tmp_path):
    lines = _run_engine(tmp_path, [])
    assert "[engine] original configuration" in lines, "\n".join(lines[-80:])
    assert not any("[patch] coalesced" in line for line in lines)
    assert not (tmp_path / "ud" / "engine").exists()


@pytest.mark.gioco
def test_engine_log_shadow_quality_high(tmp_path):
    lines = _run_engine(tmp_path, ["--shadow-quality", "high"])
    coalesced = sorted(p.name for p in (GAME_DATA / "GearGame" / "CookedXenon").iterdir()
                       if p.name.lower().startswith("coalesced_") and p.name.lower().endswith(".bin"))
    assert coalesced
    assert "[patch] coalesced.no_hash_check: turned on" in lines, "\n".join(lines[-80:])
    for name in coalesced:
        assert f"[engine] {name}: MaxShadowResolution 800 -> 2048" in lines, "\n".join(lines[-80:])
    assert any(line.startswith(f"[engine] {len(coalesced)} Coalesced modified in ") for line in lines), \
        "\n".join(lines[-80:])
    assert sorted(p.name for p in (tmp_path / "ud" / "engine").iterdir() if p.suffix == ".bin") == coalesced


@pytest.mark.gioco
def test_engine_log_low_options_no_dirty_disc(tmp_path):
    lines = _run_engine(tmp_path, ["--shadow-quality", "low", "--dynamic-shadows", "off"], seconds="30")
    assert not any("XamShowDirtyDiscErrorUI" in line for line in lines), "\n".join(lines[-80:])
