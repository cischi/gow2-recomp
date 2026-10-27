"""Measures the real fps of gow2.exe between two screenshots.

Usage: python tools/measure_fps.py --out <folder> --runs 3 --upscaler fsr --frame-rate 60 --vsync on
     --frame-generation off --start 118 --end 158 [--extra-input <script>] [--window borderless]
Repeats N runs with fixed arguments and fresh user data per run under --out (never in the repository); real fps =
difference of the `frame host` of the two screenshots divided by the seconds. Prints one line per run and median/min/max.
"""
import argparse
import re
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))  # constants of the milestone tests (INPUT_SCRIPT, SKIP_TRAINING, SKIP_MOVIES)
SHOT = re.compile(r"^\[gpu\] screenshot (.+) \d+x\d+ frame host (\d+)$", re.M)
FG = re.compile(r"^\[fg\] last 10 s: real (\d+), generated (\d+)(?:, half-interval median ([\d.]+)%)?$", re.M)


def _shots(log: str) -> dict[str, tuple[int, int]]:
    """Final file name -> (host frame, position of the line in the log)."""
    return {re.split(r"[\\/]", m.group(1))[-1]: (int(m.group(2)), m.start()) for m in SHOT.finditer(log)}


def fps_between(log: str, first: str, second: str, seconds: float) -> float | None:
    """Real fps between the `[gpu] screenshot <file> WxH frame host N` lines of the two files (final names)."""
    shots = _shots(log)
    if first not in shots or second not in shots:
        return None
    return (shots[second][0] - shots[first][0]) / seconds


def fg_window(log: str) -> list[tuple[int, int, float | None]]:
    """(real, generated, P%) of the `[fg] last 10 s` lines in order; P is None without frame statistics."""
    return [(int(r), int(g), float(p) if p else None) for r, g, p in FG.findall(log)]


def input_script(extra: str) -> str:
    """Script of the milestone tests + "Skip Training" (no wait on "Choose Path") + `extra`."""
    from test_milestone_upscaler import INPUT_SCRIPT, SKIP_TRAINING
    return ",".join(x for x in (INPUT_SCRIPT, SKIP_TRAINING, extra) if x)


def _run(exe: Path, run_dir: Path, a: argparse.Namespace, script: str, skip: list[str]) -> str:
    run_dir.mkdir(parents=True)
    args = [str(exe), "--data", str(ROOT / "game_data"), "--user-data", str(run_dir / "user_data"),
            "--window", a.window, "--resolution", "1920x1080", "--mute", "--run-seconds", str(a.end + 5),
            "--input-script", script, "--aa", "off", "--sharpening", "off", "--internal-resolution", "1",
            "--upscaler", a.upscaler, "--frame-rate", a.frame_rate, "--vsync", a.vsync,
            "--frame-generation", a.frame_generation,
            "--screenshot", f"{a.start}:{run_dir / f'{a.start}.bmp'}", "--screenshot", f"{a.end}:{run_dir / f'{a.end}.bmp'}"]
    for movie in skip:
        args += ["--skip-movie", movie]
    try:
        subprocess.run(args, cwd=run_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=a.end + 180)
    except subprocess.TimeoutExpired:  # run() already killed the process; the partial log is still there to read
        print(f"{run_dir.name}: timeout", file=sys.stderr)
    log = run_dir / "gow2.log"
    return log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--runs", type=int, default=3)
    p.add_argument("--upscaler", choices=["off", "fsr", "dlss"], required=True)
    p.add_argument("--frame-rate", default="60")
    p.add_argument("--vsync", choices=["on", "off"], default="on")
    p.add_argument("--frame-generation", choices=["off", "on", "fsr", "dlss"], default="off")
    p.add_argument("--window", choices=["windowed", "borderless"], default="windowed",
                   help="borderless: output at the monitor resolution, like the FG spike")
    p.add_argument("--start", type=int, default=118, help="second of the first screenshot")  # Jacinto
    p.add_argument("--end", type=int, default=158, help="second of the second screenshot")  # in-game with the HUD
    p.add_argument("--extra-input", default="", help="input appended to the milestone tests' script and to SKIP_TRAINING")
    p.add_argument("--exe", type=Path, default=ROOT / "build" / "clang-release" / "runtime" / "gow2.exe")
    a = p.parse_args()

    from test_milestone_upscaler import SKIP_MOVIES

    script = input_script(a.extra_input)
    config = f"{a.window}-{a.upscaler}-{a.frame_rate}-vsync{a.vsync}-fg{a.frame_generation}"
    values = []
    for i in range(1, a.runs + 1):
        log = _run(a.exe, a.out / f"{config}-{i}", a, script, SKIP_MOVIES)
        fps = fps_between(log, f"{a.start}.bmp", f"{a.end}.bmp", a.end - a.start)
        lines = log.splitlines()
        # only the creations (`... (WxH -> WxH)`): the capture line that failed at startup does not count
        upscaler = [l for l in lines if l.startswith("[gpu] upscaler:") and " -> " in l]
        bad = [l for l in lines if "device removed" in l or l.startswith("crash")]
        extra = ""
        if a.frame_generation != "off":
            shots = _shots(log)
            if f"{a.start}.bmp" in shots and f"{a.end}.bmp" in shots:
                window = fg_window(log[shots[f"{a.start}.bmp"][1]:shots[f"{a.end}.bmp"][1]])
                ps = [p for _, _, p in window if p is not None]
                extra = (f" fg real {sum(w[0] for w in window)} generated {sum(w[1] for w in window)}"
                         f" P median {statistics.median(ps) if ps else None}")
        print(f"{config} run {i}: fps {fps if fps is None else round(fps, 2)}{extra} "
              f"upscalers created {len(upscaler)} anomalies {len(bad)}", flush=True)
        if fps is not None:
            values.append(fps)
    if values:
        print(f"{config}: median {statistics.median(values):.2f} min {min(values):.2f} max {max(values):.2f} "
              f"({len(values)}/{a.runs} runs)")
    return 0 if len(values) == a.runs else 1


if __name__ == "__main__":
    sys.exit(main())
