"""Milestone of the HDR10 output.

70 s runs at 1920x1080, 60 fps, `--hdr force --hdr-peak-nits 1000 --hdr-paper-white-nits 200` (quick start of
test_milestone_upscaler). Criteria: exit 0; no `crash` or `device removed` line; the line
`[hdr] HDR10: peak 1000 nit, paper white 200 nit`; summary with `, HDR10`. Screenshots at 45 and 60 s: BMP and
`.pq.ppm` 1920x1080; on pixels at step 2 the decoded PQ (nit / paper white, BT.2020 -> 709) matches the SDR
(coherence >= 99% within 2 codes where there is no light above paper white) and at least one screenshot has highlights
(max(lin709) > 1.05) on >= 0.1% of the pixels. The run with `--frame-generation on` requires the line
`[fg] FG context ... HDR`, no `[fg] ffx error` and a 10 s window with real >= 200, generated >= 0.9 real.

Skipped without game_data/default.xex, without gow2.exe or without a D3D12 adapter; the FG run also without the frame
generation DLL next to gow2.exe or with OptiScaler installed there. The test prints coherence and highlights per screenshot.
"""

import re
import struct
import subprocess

import pytest

from test_milestone_gpu import XEX, GAME_DATA, _gow2_exe, _require_adapter
from test_milestone_upscaler import INPUT_SCRIPT, SKIP_MOVIES

RUN_SECONDS = 70
TIMEOUT_S = 160
SHOTS_AT_S = (45, 60)
WIDTH, HEIGHT = 1920, 1080
PAPER_WHITE = 200
MIN_COHERENT = 0.99
MIN_HIGHLIGHTS = 0.001
MIN_REAL = 200
MIN_RATIO = 0.9
FG_DLL = "amd_fidelityfx_framegeneration_dx12.dll"
HDR_ARGS = ["--hdr", "force", "--hdr-peak-nits", "1000", "--hdr-paper-white-nits", str(PAPER_WHITE)]

# Same as runtime/hdr_math.cpp.
M1, M2, C1, C2, C3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
BT709_TO_BT2020 = ((0.627403896, 0.329283039, 0.043313065),
                   (0.069097289, 0.919540395, 0.011362316),
                   (0.016391439, 0.088013308, 0.895595253))


def nits_from_pq(pq):
    q = min(max(pq, 0.0), 1.0) ** (1 / M2)
    return 10000.0 * (max(q - C1, 0.0) / (C2 - C3 * q)) ** (1 / M1)


def pq_from_nits(nits):
    y = min(max(nits / 10000.0, 0.0), 1.0) ** M1
    return ((C1 + C2 * y) / (1 + C3 * y)) ** M2


def _inverse3(m):
    (a, b, c), (d, e, f), (g, h, i) = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return ((( e * i - f * h) / det, -(b * i - c * h) / det, ( b * f - c * e) / det),
            (-(d * i - f * g) / det, ( a * i - c * g) / det, -(a * f - c * d) / det),
            (( d * h - e * g) / det, -(a * h - b * g) / det, ( a * e - b * d) / det))


BT2020_TO_BT709 = _inverse3(BT709_TO_BT2020)


def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def srgb8(lin):
    lin = min(max(lin, 0.0), 1.0)
    return round(255 * (lin * 12.92 if lin <= 0.0031308 else 1.055 * lin ** (1 / 2.4) - 0.055))


def read_bmp(path):
    """(width, height, rows from the top in BGR) of a bottom-up 24-bit BMP."""
    data = path.read_bytes()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    assert struct.unpack_from("<H", data, 28)[0] == 24 and height > 0
    stride = (width * 3 + 3) & ~3
    rows = [data[offset + y * stride:offset + y * stride + width * 3] for y in range(height)]
    return width, height, rows[::-1]


def read_ppm10(path):
    """(width, height, big-endian RGB codes from the top) of a P6 PPM with maxval 1023."""
    data = path.read_bytes()
    magic, w, h, maxval, rest = data.split(maxsplit=4)
    assert magic == b"P6" and maxval == b"1023"
    w, h = int(w), int(h)
    return w, h, struct.unpack(f">{w * h * 3}H", rest[:w * h * 6])


def hdr_measure(bmp_path, ppm_path, paper_white, step=2):
    """(coherence, highlights, BMP size, PPM size) on pixels at step `step`.

    Coherence: among the pixels with max(lin709) <= 1 and BMP luminance > 1%, the share with |sRGB8(lin709) - bmp| <= 2 on
    every channel. Highlights: share of pixels with max(lin709) > 1.05.
    """
    bw, bh, rows = read_bmp(bmp_path)
    pw, ph, codes = read_ppm10(ppm_path)
    if (bw, bh) != (pw, ph):
        return 0.0, 0.0, (bw, bh), (pw, ph)
    nits = [nits_from_pq(c / 1023) / paper_white for c in range(1024)]
    lin8 = [srgb_to_linear(c / 255) for c in range(256)]
    (a, b, c), (d, e, f), (g, h, i) = BT2020_TO_BT709
    coherent = candidates = highlights = total = 0
    for y in range(0, bh, step):
        row = rows[y]
        base = y * pw * 3
        for x in range(0, bw, step):
            p = base + x * 3
            n0, n1, n2 = nits[codes[p]], nits[codes[p + 1]], nits[codes[p + 2]]
            r, gg, bb = a * n0 + b * n1 + c * n2, d * n0 + e * n1 + f * n2, g * n0 + h * n1 + i * n2
            total += 1
            m = max(r, gg, bb)
            if m > 1.05:
                highlights += 1
            if m > 1:
                continue
            sb, sg, sr = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
            if 0.2126 * lin8[sr] + 0.7152 * lin8[sg] + 0.0722 * lin8[sb] <= 0.01:
                continue
            candidates += 1
            if abs(srgb8(r) - sr) <= 2 and abs(srgb8(gg) - sg) <= 2 and abs(srgb8(bb) - sb) <= 2:
                coherent += 1
    return coherent / max(candidates, 1), highlights / total, (bw, bh), (pw, ph)


def test_hdr_measure_on_synthetic_images(tmp_path):
    """A PPM encoded from the BMP at paper white is coherent; a pixel at 4x paper white counts as a highlight."""
    w, h = 4, 2
    bgr = [(40, 120, 200), (255, 255, 255), (0, 0, 0), (90, 10, 250)] * 2
    bmp = tmp_path / "s.bmp"
    pixels = b"".join(bytes(p) for p in bgr)
    bmp.write_bytes(b"BM" + struct.pack("<IHHI", 54 + len(pixels), 0, 0, 54) +
                    struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(pixels), 0, 0, 0, 0) + pixels)
    codes = []
    for k, (b8, g8, r8) in enumerate(bgr):
        lin = [srgb_to_linear(v / 255) * (4 if k % 4 == 1 else 1) for v in (r8, g8, b8)]
        for row in BT709_TO_BT2020:
            codes.append(round(1023 * pq_from_nits(sum(m * v for m, v in zip(row, lin)) * PAPER_WHITE)))
    ppm = tmp_path / "s.pq.ppm"
    ppm.write_bytes(f"P6\n{w} {h}\n1023\n".encode() + struct.pack(f">{len(codes)}H", *codes))
    coherence, highlights, bmp_size, ppm_size = hdr_measure(bmp, ppm, PAPER_WHITE, step=1)
    assert bmp_size == ppm_size == (w, h)
    assert coherence == 1.0
    assert highlights == pytest.approx(2 / 8)


def _run(tmp_path, extra):
    if not XEX.is_file():
        pytest.skip(f"{XEX} missing: extract the disc into game_data/")
    exe = _gow2_exe()
    if "--frame-generation" in extra and not (exe.parent / FG_DLL).is_file():
        pytest.skip(f"{FG_DLL} not next to gow2.exe")
    _require_adapter(exe)
    args = [str(exe), "--data", str(GAME_DATA), "--user-data", str(tmp_path / "user_data"),
            "--run-seconds", str(RUN_SECONDS), "--audio", "null", "--input-script", INPUT_SCRIPT,
            "--window", "windowed", "--resolution", f"{WIDTH}x{HEIGHT}", "--frame-rate", "60", *HDR_ARGS, *extra]
    for movie in SKIP_MOVIES:
        args += ["--skip-movie", movie]
    try:
        proc = subprocess.run(args, cwd=tmp_path, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        pytest.fail(f"gow2.exe did not exit within {TIMEOUT_S} s")
    log_path = tmp_path / "gow2.log"
    assert log_path.is_file(), f"gow2.log missing (exit {proc.returncode})"
    log = log_path.read_text(encoding="utf-8", errors="replace")
    lines = log.splitlines()
    tail = log[-4000:]
    assert proc.returncode == 0, f"exit {proc.returncode}:\n{tail}"
    assert not [l for l in lines if l.startswith("crash")], f"crash line in the log:\n{tail}"
    assert not [l for l in lines if "device removed" in l], f"device removed in the log:\n{tail}"
    assert [l for l in lines if l.startswith(f"[hdr] HDR10: peak 1000 nit, paper white {PAPER_WHITE} nit")], \
        f"[hdr] HDR10 line missing:\n{tail}"
    assert [l for l in lines if ", HDR10" in l and not l.startswith("[hdr]")], f"summary without ', HDR10':\n{tail}"
    if "[optiscaler] frame generation turned off: OptiScaler installed" in lines:
        pytest.skip("OptiScaler (dxgi.dll) next to gow2.exe turns frame generation off: uninstall it to run this test")
    return lines, tail


@pytest.mark.gioco
def test_milestone_hdr(tmp_path):
    shots = {s: tmp_path / f"s{s}.bmp" for s in SHOTS_AT_S}
    extra = []
    for s, path in shots.items():
        extra += ["--screenshot", f"{s}:{path}"]
    _, tail = _run(tmp_path, extra)
    results = {}
    for s, bmp in shots.items():
        ppm = bmp.with_suffix(".pq.ppm")
        assert bmp.is_file() and ppm.is_file(), f"screenshot at {s} s missing:\n{tail}"
        coherence, highlights, bmp_size, ppm_size = hdr_measure(bmp, ppm, PAPER_WHITE)
        print(f"screenshot {s} s: BMP {bmp_size} PPM {ppm_size} coherence {100 * coherence:.3f}% "
              f"highlights {100 * highlights:.3f}%")
        assert bmp_size == ppm_size == (WIDTH, HEIGHT), f"sizes at {s} s: BMP {bmp_size}, PPM {ppm_size}"
        assert coherence >= MIN_COHERENT, f"coherence at {s} s {100 * coherence:.3f}% < 99%"
        results[s] = highlights
    assert max(results.values()) >= MIN_HIGHLIGHTS, f"no screenshot with highlights >= 0.1%: {results}"


@pytest.mark.gioco
def test_milestone_hdr_frame_gen(tmp_path):
    lines, tail = _run(tmp_path, ["--frame-generation", "on"])
    pairs = [(int(m.group(1)), int(m.group(2))) for l in lines
             if (m := re.search(r"\[fg\] last 10 s: real (\d+), generated (\d+)", l))]
    print(f"pairs (real, generated): {pairs}")
    assert [l for l in lines if l.startswith("[fg] FG context ") and l.endswith(", HDR")], \
        f"line '[fg] FG context ... HDR' missing:\n{tail}"
    assert not [l for l in lines if l.startswith("[fg] ffx error")], f"ffx error in the log:\n{tail}"
    assert [p for p in pairs if p[0] >= MIN_REAL and p[1] >= MIN_RATIO * p[0]], \
        f"no pair with real >= {MIN_REAL} and generated >= {MIN_RATIO} * real: {pairs}"
