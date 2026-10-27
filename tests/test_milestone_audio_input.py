"""Milestone of sub-project 4: game audio and input.

Run A: gow2.exe --run-seconds 60 --audio null --audio-capture <tmp>/a.wav
--input-script 40:START+250 --screenshot 38:<tmp>/a_pre.bmp
--screenshot 50:<tmp>/a_post.bmp, user_data in tmp, timeout 150 s.
Run B (control): the same command without --input-script (b.wav, b_pre.bmp,
b_post.bmp).

Criteria on run A: exit 0; no line that starts with `crash` nor
contains `device removed`; audio_callbacks >= 9000; audio_frames_submitted >=
0.95 * audio_callbacks; xma_kicks > 0, xma_frames_decoded > 0,
xma_decode_errors == 0; input_get_state_calls > 0, input_injected_events >= 1.
WAV: 48,000 Hz, 6 float channels, duration >= 40 s; the capture time starts
from the first frame sent by the game (the WAV only contains the frames sent) and the
window is [10 s, end]: rms_dbfs > -50 and max_window_peak_dbfs > -20.

Input criterion (controller decision, rulings of sub-project 4):
the title background is animated, so post - pre within the same run is not a
signal (control measured 11.23 versus 23.02 with the script). The two runs are
compared at the same instants: mean_abs_luma_diff(pre_A, pre_B) <= 10 (before
the input the two runs look alike) and mean_abs_luma_diff(post_A, post_B) > 10
(after START run A shows the menu, B is still on the title). Run B:
exit 0.

Skipped without game_data/default.xex, without gow2.exe or without a hardware D3D12
adapter (`gow2.exe --gpu-probe`). WAV, screenshots and log stay in the tmp_path.
The test prints all the measured values.
"""

import array
import math
import operator
import os
import struct
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
GAME_DATA = ROOT / "game_data"
XEX = GAME_DATA / "default.xex"
RUN_SECONDS = 60
TIMEOUT_S = 150
PRE_AT_S = 38
POST_AT_S = 50
INPUT_SCRIPT = "40:START+250"
RATE = 48000
CHANNELS = 6
WINDOW_START_S = 10.0
MIN_CAPTURE_S = 40.0
LUMA_THRESHOLD = 10.0

WAVE_FORMAT_IEEE_FLOAT = 3
WAVE_FORMAT_EXTENSIBLE = 0xFFFE
# KSDATAFORMAT_SUBTYPE_IEEE_FLOAT 00000003-0000-0010-8000-00AA00389B71
FLOAT_SUBFORMAT = bytes([0x03, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71])


# --- Pure helpers (tested below with synthetic data) ---------------------------------------------

def read_wav_f32(path):
    """(rate, channels, samples) of a float32 WAV (tag 3 or EXTENSIBLE with a float subformat).

    samples is an interleaved array('f'). If the size of the data chunk exceeds the bytes present
    (file truncated by an abrupt exit) it reads up to the end, in whole frames.
    """
    data = Path(path).read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    pos = 12
    fmt = None
    while pos + 8 <= len(data):
        tag = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = pos + 8
        if tag == b"fmt ":
            fmt = data[body:body + size]
        elif tag == b"data":
            if fmt is None:
                raise ValueError("data chunk before fmt")
            tag_fmt, channels, rate = struct.unpack_from("<HHI", fmt, 0)
            block_align, bits = struct.unpack_from("<HH", fmt, 12)
            if tag_fmt == WAVE_FORMAT_EXTENSIBLE:
                if len(fmt) < 40 or fmt[24:40] != FLOAT_SUBFORMAT:
                    raise ValueError("EXTENSIBLE with a non-float subformat")
            elif tag_fmt != WAVE_FORMAT_IEEE_FLOAT:
                raise ValueError(f"format {tag_fmt:#x} is not float")
            if bits != 32 or block_align != 4 * channels:
                raise ValueError(f"{bits}-bit samples, block_align {block_align}")
            end = min(body + size, len(data))
            end -= (end - body) % block_align
            samples = array.array("f")
            samples.frombytes(data[body:end])
            return rate, channels, samples
        pos = body + size + (size & 1)
    raise ValueError("data chunk missing")


def rms_dbfs(samples):
    """20*log10(RMS over all samples and channels); -inf for silence or no samples."""
    if len(samples) == 0:
        return float("-inf")
    mean_sq = sum(map(operator.mul, samples, samples)) / len(samples)
    return 20.0 * math.log10(math.sqrt(mean_sq)) if mean_sq > 0.0 else float("-inf")


def max_window_peak_dbfs(samples, rate, channels, window_s=1.0):
    """Absolute peak of the best window of window_s seconds (non-overlapping windows), in dBFS.

    The peak of the best window is the absolute peak of all the complete windows; the last
    incomplete window does not count.
    """
    per_window = int(rate * window_s) * channels
    best = 0.0
    for start in range(0, len(samples) - per_window + 1, per_window):
        window = samples[start:start + per_window]
        peak = max(max(window), -min(window))
        best = max(best, peak)
    return 20.0 * math.log10(best) if best > 0.0 else float("-inf")


def _bmp_luma(bmp):
    offset = struct.unpack_from("<I", bmp, 10)[0]
    width, height = struct.unpack_from("<ii", bmp, 18)
    bpp = struct.unpack_from("<H", bmp, 28)[0]
    if bpp != 24 or (width * 3) % 4 != 0:
        raise ValueError(f"unsupported BMP {width}x{height} at {bpp} bits")
    pixels = memoryview(bmp)[offset:offset + width * abs(height) * 3]
    b = pixels[0::3]
    g = pixels[1::3]
    r = pixels[2::3]
    return (width, height), [0.299 * rv + 0.587 * gv + 0.114 * bv for rv, gv, bv in zip(r, g, b)]


def mean_abs_luma_diff(bmp_a, bmp_b):
    """Mean of |Y_a - Y_b| (Y = 0.299 R + 0.587 G + 0.114 B) over 24-bit BMPs of the same size."""
    size_a, ya = _bmp_luma(bmp_a)
    size_b, yb = _bmp_luma(bmp_b)
    if size_a != size_b:
        raise ValueError(f"different sizes: {size_a} and {size_b}")
    return sum(abs(a - b) for a, b in zip(ya, yb)) / len(ya)


# --- Helper tests ---------------------------------------------------------------------------

def _wav_bytes(samples, channels=CHANNELS, rate=RATE, extensible=True, subformat=FLOAT_SUBFORMAT, tag=3):
    payload = array.array("f", samples).tobytes()
    if extensible:
        fmt = struct.pack("<HHIIHHHHI", WAVE_FORMAT_EXTENSIBLE, channels, rate, rate * 4 * channels,
                          4 * channels, 32, 22, 32, 0x3F) + subformat
    else:
        fmt = struct.pack("<HHIIHH", tag, channels, rate, rate * 4 * channels, 4 * channels, 32)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(payload)) + payload
    return b"RIFF" + struct.pack("<I", len(body)) + body


def _sine_one_of_six(seconds=1.0, amplitude=0.5, freq=1000.0):
    n = int(RATE * seconds)
    out = [0.0] * (n * CHANNELS)
    for i in range(n):
        out[i * CHANNELS] = amplitude * math.sin(2.0 * math.pi * freq * i / RATE)
    return out


def test_read_wav_f32_extensible_and_plain_float(tmp_path):
    values = [0.25, -0.5, 0.0, 1.0, -1.0, 0.125] * 3
    ext = tmp_path / "ext.wav"
    ext.write_bytes(_wav_bytes(values))
    rate, channels, samples = read_wav_f32(ext)
    assert (rate, channels, list(samples)) == (RATE, CHANNELS, values)
    plain = tmp_path / "plain.wav"
    plain.write_bytes(_wav_bytes(values[:6], channels=2, rate=44100, extensible=False))
    assert read_wav_f32(plain)[:2] == (44100, 2)


def test_read_wav_f32_rejects_pcm_and_reads_truncated_data(tmp_path):
    pcm = tmp_path / "pcm.wav"
    pcm.write_bytes(_wav_bytes([0.0] * 6, extensible=False, tag=1))
    with pytest.raises(ValueError):
        read_wav_f32(pcm)
    pcm_ext = tmp_path / "pcm_ext.wav"
    pcm_ext.write_bytes(_wav_bytes([0.0] * 6, subformat=bytes([1]) + FLOAT_SUBFORMAT[1:]))
    with pytest.raises(ValueError):
        read_wav_f32(pcm_ext)
    # The data chunk declares 3 frames, the file contains 2 and a half: 2 frames are read.
    truncated = tmp_path / "trunc.wav"
    truncated.write_bytes(_wav_bytes([0.5] * 18)[:-12])
    assert len(read_wav_f32(truncated)[2]) == 12


def test_rms_dbfs_sine_on_one_of_six_channels():
    expected = 20 * math.log10(0.5 / math.sqrt(2)) - 10 * math.log10(6)
    assert expected == pytest.approx(-16.81, abs=0.01)
    assert rms_dbfs(array.array("f", _sine_one_of_six())) == pytest.approx(-16.81, abs=0.05)


def test_rms_dbfs_silence_is_minus_inf():
    assert rms_dbfs(array.array("f", [0.0] * (RATE * CHANNELS))) == float("-inf")
    assert rms_dbfs(array.array("f")) == float("-inf")


def test_max_window_peak_dbfs_single_peak():
    samples = array.array("f", [0.0] * (3 * RATE * CHANNELS))
    samples[RATE * CHANNELS + 12345] = -0.1  # only in the second window
    assert max_window_peak_dbfs(samples, RATE, CHANNELS) == pytest.approx(-20.0, abs=1e-4)
    assert max_window_peak_dbfs(array.array("f", [0.0] * (RATE * CHANNELS)), RATE, CHANNELS) == float("-inf")


def _uniform_bmp(luma, width=8, height=2):
    header = b"BM" + struct.pack("<IHHI", 54 + width * height * 3, 0, 0, 54) + struct.pack(
        "<IiiHHIIiiII", 40, width, height, 1, 24, 0, width * height * 3, 0, 0, 0, 0)
    return header + bytes([luma, luma, luma]) * (width * height)


def test_mean_abs_luma_diff_uniform_images():
    assert mean_abs_luma_diff(_uniform_bmp(10), _uniform_bmp(30)) == pytest.approx(20.0, abs=1e-9)
    assert mean_abs_luma_diff(_uniform_bmp(30), _uniform_bmp(30)) == pytest.approx(0.0, abs=1e-9)


# --- Game runs ----------------------------------------------------------------------------

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


def _summary(log: str, key: str) -> int:
    prefix = f"[summary] {key}="
    values = [line[len(prefix):] for line in log.splitlines() if line.startswith(prefix)]
    assert len(values) == 1, f"{prefix} missing or repeated:\n{log[-4000:]}"
    return int(values[0])


def _run(exe: Path, run_dir: Path, name: str, script):
    run_dir.mkdir()
    args = [str(exe), "--data", str(GAME_DATA), "--user-data", str(run_dir / "user_data"),
            "--run-seconds", str(RUN_SECONDS), "--audio", "null",
            "--audio-capture", str(run_dir / f"{name}.wav"),
            "--screenshot", f"{PRE_AT_S}:{run_dir / f'{name}_pre.bmp'}",
            "--screenshot", f"{POST_AT_S}:{run_dir / f'{name}_post.bmp'}"]
    if script:
        args += ["--input-script", script]
    proc = subprocess.run(args, cwd=run_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                          timeout=TIMEOUT_S)
    log = (run_dir / "gow2.log").read_text(encoding="utf-8", errors="replace")
    return proc.returncode, log


@pytest.mark.gioco
def test_milestone_audio_input(tmp_path):
    exe = _gow2_exe()
    _require_adapter(exe)
    a_dir, b_dir = tmp_path / "a", tmp_path / "b"
    code_a, log_a = _run(exe, a_dir, "a", INPUT_SCRIPT)
    code_b, log_b = _run(exe, b_dir, "b", None)
    tail_a, tail_b = log_a[-4000:], log_b[-4000:]

    assert code_a == 0, f"run A: exit {code_a}:\n{tail_a}"
    assert code_b == 0, f"run B: exit {code_b}:\n{tail_b}"
    lines_a = log_a.splitlines()
    assert not [l for l in lines_a if l.startswith("crash")], f"crash line in the log:\n{tail_a}"
    assert not [l for l in lines_a if "device removed" in l], f"device removed in the log:\n{tail_a}"
    lines_b = log_b.splitlines()
    assert not [l for l in lines_b if l.startswith("crash")], f"crash line in log B:\n{tail_b}"
    assert not [l for l in lines_b if "device removed" in l], f"device removed in log B:\n{tail_b}"

    keys = ["audio_callbacks", "audio_frames_submitted", "xma_kicks", "xma_frames_decoded",
            "xma_decode_errors", "input_get_state_calls", "input_injected_events", "host_frames_presented"]
    sa = {k: _summary(log_a, k) for k in keys}
    sb = {k: _summary(log_b, k) for k in keys}
    print("run A:", " ".join(f"{k}={v}" for k, v in sa.items()))
    print("run B:", " ".join(f"{k}={v}" for k, v in sb.items()))

    rate, channels, samples = read_wav_f32(a_dir / "a.wav")
    duration = len(samples) / (channels * rate)
    window = samples[int(WINDOW_START_S * rate) * channels:]
    rms = rms_dbfs(window)
    peak = max_window_peak_dbfs(window, rate, channels)
    print(f"wav A: rate={rate} channels={channels} duration_s={duration:.2f} "
          f"window=[{WINDOW_START_S:.0f} s, end] rms_dbfs={rms:.2f} max_window_peak_dbfs={peak:.2f}")

    shots = {}
    for d, n in ((a_dir, "a"), (b_dir, "b")):
        for when in ("pre", "post"):
            path = d / f"{n}_{when}.bmp"
            assert path.is_file(), f"screenshot {path.name} missing"
            shots[n, when] = path.read_bytes()
    pre_diff = mean_abs_luma_diff(shots["a", "pre"], shots["b", "pre"])
    post_diff = mean_abs_luma_diff(shots["a", "post"], shots["b", "post"])
    print(f"luma: pre_A_vs_pre_B={pre_diff:.2f} post_A_vs_post_B={post_diff:.2f} "
          f"(A post-pre={mean_abs_luma_diff(shots['a', 'post'], shots['a', 'pre']):.2f}, "
          f"B post-pre={mean_abs_luma_diff(shots['b', 'post'], shots['b', 'pre']):.2f})")

    assert sa["audio_callbacks"] >= 9000, tail_a
    assert sa["audio_frames_submitted"] >= 0.95 * sa["audio_callbacks"], tail_a
    assert sa["xma_kicks"] > 0, tail_a
    assert sa["xma_frames_decoded"] > 0, tail_a
    assert sa["xma_decode_errors"] == 0, tail_a
    assert (rate, channels) == (RATE, CHANNELS)
    assert duration >= MIN_CAPTURE_S
    assert rms > -50, f"capture too quiet: rms {rms:.2f} dBFS"
    assert peak > -20, f"capture too quiet: peak {peak:.2f} dBFS"
    assert sa["input_get_state_calls"] > 0, tail_a
    assert sa["input_injected_events"] >= 1, tail_a
    assert sb["input_injected_events"] == 0, tail_b
    assert pre_diff <= LUMA_THRESHOLD, f"the runs already differ before the input: {pre_diff:.2f}"
    assert post_diff > LUMA_THRESHOLD, f"START had no visible effect: {post_diff:.2f}"
