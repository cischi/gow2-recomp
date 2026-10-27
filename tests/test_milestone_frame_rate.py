"""Milestone of the frame rate sub-project: frames, cutscene duration and in-game audio drift.

One 180 s gow2.exe run per value (30, 60, 144, unlimited), with a quick start, first cutscene
(40..100 s), game in the hospital (145..175 s), `--vsync off --vrr off` so the count does not depend on the
monitor. The frames between two instants are the difference of the `frame host <n>` of the screenshot lines.
Thresholds: +-2% on the expected
number of frames; audio: drift between two lines of the cutscene relative to the 30 run <= 100 ms and <= 2%.
The audio reference is the 30 run of the same session (no file derived from the game in the repository).
Skipped without game_data/default.xex, without gow2.exe or without a D3D12 adapter. Prints the measured values.
"""

import array
import math
import operator
import os
import re
import statistics
import struct
import subprocess
from pathlib import Path

import pytest
from test_milestone_upscaler import SKIP_TRAINING

ROOT = Path(__file__).resolve().parent.parent
GAME_DATA = ROOT / "game_data"
XEX = GAME_DATA / "default.xex"
RUN_SECONDS = 180
TIMEOUT_S = 300
# Up to 34 s like the other milestone tests; "Skip Training" with the LT window (choice at 100 s at every frame rate).
INPUT_SCRIPT = ("8:START+200,14:A+200,18:A+200,22:A+200,26:A+200,30:A+200,34:A+200,"
                + SKIP_TRAINING + ",155:LS_UP+3000")
SKIP_MOVIES = ["EpicLogo", "MGSLogo", "Attract_Opening_Cinematic"]
SHOTS = [40, 100, 145, 175]
CUT_S = (40, 100)    # first cutscene
GAME_S = (145, 175)  # game
# 10 s windows of the 30 run for the audio drift, inside the first cutscene, each with a line of dialogue.
# Chosen on the envelope of the 30 run (cutscene from ~30 to ~97 s): among the windows starting between 40 and 54 s and between
# 78 and 88 s, those with the largest standard deviation of the envelope in dB (4.97 and 4.82 dB), i.e. with more
# lines and transients to correlate; the last one ends at 90 s, before the end of the cutscene.
CUT_START_S = 44.0
CUT_END_S = 80.0
WIN_S = 10.0
MIN_CORR = 0.6
MIN_MARGIN = 0.05   # correlation peak above the second peak (at >= 100 ms) by at least this much
MIN_STDEV_DB = 2.0  # envelope of the reference window: below this it is flat music or silence (menu)
MIN_COOKED_PACKAGES = 28  # campaign loaded (new_game: 34 measured, title and menu 11); here 53 with the game
MAX_DRIFT_S = 0.100
MAX_DRIFT_FRAC = 0.02
HOP = 480  # 10 ms a 48 kHz


def _gow2_exe() -> Path:
    override = os.environ.get("GOW2_EXE")
    exe = Path(override) if override else ROOT / "build" / "clang-release" / "runtime" / "gow2.exe"
    if not exe.is_file():
        pytest.skip(f"gow2.exe not built ({exe}): run `python tools/gow2.py build --iso <iso>`")
    return exe


def _require_adapter(exe: Path) -> None:
    proc = subprocess.run([str(exe), "--gpu-probe"], capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=60)
    if proc.returncode != 0:
        pytest.skip(f"no hardware D3D12 adapter (--gpu-probe exit {proc.returncode})")


def run_game(run_dir: Path, value: str) -> dict:
    """One run at `--frame-rate value`; returns the host frames per screenshot second and the WAV."""
    run_dir.mkdir(parents=True, exist_ok=True)
    wav = run_dir / "audio.wav"
    args = [str(_gow2_exe()), "--data", str(GAME_DATA), "--user-data", str(run_dir / "user_data"),
            "--run-seconds", str(RUN_SECONDS), "--mute", "--audio-capture", str(wav),
            "--window", "windowed", "--resolution", "1920x1080", "--vsync", "off", "--vrr", "off",
            "--frame-rate", value, "--input-script", INPUT_SCRIPT]
    for m in SKIP_MOVIES:
        args += ["--skip-movie", m]
    for s in SHOTS:
        args += ["--screenshot", f"{s}:{run_dir / ('s%d.bmp' % s)}"]
    log_path = run_dir / "gow2.log"
    try:
        proc = subprocess.run(args, cwd=run_dir, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                              timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        partial = log_path.read_text(encoding="utf-8", errors="replace") if log_path.is_file() else "(gow2.log missing)"
        pytest.fail(f"gow2.exe ({value}) did not exit within {TIMEOUT_S} s:\n{partial[-4000:]}")
    assert log_path.is_file(), f"gow2.log missing (exit {proc.returncode}): {proc.stderr[-500:]}"
    log = log_path.read_text(encoding="utf-8", errors="replace")
    assert proc.returncode == 0, f"exit {proc.returncode} ({value}):\n{log[-4000:]}"
    frames = {}
    for m in re.finditer(r"\[gpu\] screenshot \S*?s(\d+)\.bmp \d+x\d+ frame host (\d+)", log):
        frames[int(m.group(1))] = int(m.group(2))
    assert sorted(frames) == SHOTS, f"screenshot {sorted(frames)} != {SHOTS} ({value}):\n{log[-4000:]}"
    assert wav.is_file(), f"WAV missing ({value})"
    cooked = re.search(r"^\[summary\] cooked_packages_opened=(\d+)$", log, re.M)
    print(f"[{value}] cooked_packages_opened={cooked and cooked.group(1)}")
    assert cooked and int(cooked.group(1)) >= MIN_COOKED_PACKAGES, (
        f"cooked packages {cooked and cooked.group(1)} < {MIN_COOKED_PACKAGES} ({value}): the quick start did not reach the campaign")
    return {"frames": frames, "wav": wav}


def envelope(wav: Path) -> list:
    """RMS envelope in dB at 10 ms over all channels (float32 WAV)."""
    b = wav.read_bytes()
    pos, ch, data = 12, 2, b""
    while pos + 8 <= len(b):
        cid, size = b[pos:pos + 4], struct.unpack_from("<I", b, pos + 4)[0]
        if cid == b"fmt ":
            ch = struct.unpack_from("<H", b, pos + 10)[0]
        elif cid == b"data":
            data = b[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    x = array.array("f")
    x.frombytes(data[: len(data) // 4 * 4])
    n = HOP * ch
    out = []
    for i in range(0, len(x) - n + 1, n):
        s = x[i:i + n]
        out.append(10 * math.log10(sum(map(operator.mul, s, s)) / n + 1e-10))
    return out


def _lag(ref, run, at, win, lo, hi):
    """(lag in 10 ms samples in [lo, hi], correlation, margin over the second peak at >= 100 ms)."""
    r = ref[at:at + win]
    m = sum(r) / win
    r = [v - m for v in r]
    rn = math.sqrt(sum(v * v for v in r))
    scores = {}
    for l in range(lo, hi + 1):
        s = at + l
        if s < 0 or s + win > len(run):
            continue
        seg = run[s:s + win]
        sm = sum(seg) / win
        seg = [v - sm for v in seg]
        c = sum(map(operator.mul, r, seg)) / (rn * math.sqrt(sum(v * v for v in seg)) + 1e-9)
        scores[l] = c
    best_l = max(scores, key=scores.get)
    others = [c for l, c in scores.items() if abs(l - best_l) >= 10]
    return best_l, scores[best_l], scores[best_l] - max(others, default=-1.0)


def audio_drift(ref_wav: Path, run_wav: Path):
    """(drift_s, lag1_s, corr1, lag2_s, corr2) of the run relative to the reference."""
    ref, run = envelope(ref_wav), envelope(run_wav)
    win = int(WIN_S * 100)
    a1, a2 = int(CUT_START_S * 100), int(CUT_END_S * 100)
    for at in (a1, a2):
        sd = statistics.pstdev(ref[at:at + win])
        assert sd >= MIN_STDEV_DB, f"flat reference envelope at {at / 100} s (stdev {sd:.2f} dB): cutscene not reached?"
    l1, c1, m1 = _lag(ref, run, a1, win, -800, 800)
    assert c1 >= MIN_CORR, f"audio correlation at the start {c1:.2f} < {MIN_CORR} (window {CUT_START_S} s)"
    assert m1 >= MIN_MARGIN, f"ambiguous initial lag: margin {m1:.3f} < {MIN_MARGIN} (periodic signal?)"
    l2, c2, m2 = _lag(ref, run, a2, win, l1 - 100, l1 + 100)
    assert m2 >= MIN_MARGIN, f"ambiguous final lag: margin {m2:.3f} < {MIN_MARGIN} (periodic signal?)"
    assert c2 >= MIN_CORR, f"audio correlation at the end {c2:.2f} < {MIN_CORR} (window {CUT_END_S} s)"
    print(f"margins {m1:.3f} {m2:.3f}")
    return (l2 - l1) / 100, l1 / 100, c1, l2 / 100, c2


@pytest.fixture(scope="module")
def runs(tmp_path_factory):
    if not XEX.is_file():
        pytest.skip(f"{XEX} missing: extract the disc into game_data/")
    _require_adapter(_gow2_exe())
    base = tmp_path_factory.mktemp("frame_rate")
    cache = {}

    def get(value):
        if value not in cache:
            cache[value] = run_game(base / value, value)
        return cache[value]
    return get


def _counts(r):
    f = r["frames"]
    return f[CUT_S[1]] - f[CUT_S[0]], f[GAME_S[1]] - f[GAME_S[0]]


def _check_audio(runs, value):
    drift, l1, c1, l2, c2 = audio_drift(runs("30")["wav"], runs(value)["wav"])
    span = CUT_END_S - CUT_START_S
    print(f"[{value}] audio lag1={l1:+.2f}s (corr {c1:.2f}) lag2={l2:+.2f}s (corr {c2:.2f}) "
          f"drift={drift * 1000:+.0f} ms ({abs(drift) / span * 100:.2f}%)")
    assert abs(drift) <= MAX_DRIFT_S, f"audio drift {drift:+.3f} s beyond {MAX_DRIFT_S} s"
    assert abs(drift) / span <= MAX_DRIFT_FRAC, f"audio drift {abs(drift) / span:.3%} beyond {MAX_DRIFT_FRAC:.0%}"


@pytest.mark.gioco
def test_frame_rate_30(runs):
    cut, game = _counts(runs("30"))
    print(f"[30] cutscene={cut} (1764..1836) game={game} (882..918)")
    assert 1764 <= cut <= 1836
    assert 882 <= game <= 918


@pytest.mark.gioco
def test_frame_rate_60(runs):
    cut, game = _counts(runs("60"))
    print(f"[60] cutscene={cut} (3528..3672) game={game} (1764..1836)")
    assert 3528 <= cut <= 3672
    assert 1764 <= game <= 1836
    _check_audio(runs, "60")


@pytest.mark.gioco
def test_frame_rate_144(runs):
    cut, game = _counts(runs("144"))
    print(f"[144] cutscene={cut} (only printed) game={game} (4234..4406)")
    assert 4234 <= game <= 4406
    _check_audio(runs, "144")


@pytest.mark.gioco
def test_frame_rate_unlimited(runs):
    cut, game = _counts(runs("unlimited"))
    print(f"[unlimited] cutscene={cut} (>3672) game={game} (>1836)")
    assert cut > 3672
    assert game > 1836
    _check_audio(runs, "unlimited")
