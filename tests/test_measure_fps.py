"""Tests of the pure part of tools/measure_fps.py on synthetic logs (no game bytes)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from measure_fps import fg_window, fps_between  # noqa: E402

LOG = (
    "[gpu] upscaler: FSR 3.1.4 (1280x720 -> 1920x1080)\n"
    "[gpu] screenshot C:\\tmp\\run 1\\280.bmp 1920x1080 frame host 1000\n"
    "[fg] last 10 s: real 600, generated 598\n"
    "[fg] last 10 s: real 590, generated 589, half-interval median 48.5%\n"
    "[gpu] screenshot C:\\tmp\\run 1\\320.bmp 1920x1080 frame host 3400\n"
)


def test_fps_between_two_screenshots():
    assert fps_between(LOG, "280.bmp", "320.bmp", 40) == 60.0


def test_fps_between_missing_screenshot():
    assert fps_between(LOG, "280.bmp", "999.bmp", 40) is None


def test_fg_window_in_order():
    assert fg_window(LOG) == [(600, 598, None), (590, 589, 48.5)]


def test_input_script_skip_training_and_extra():
    from measure_fps import input_script
    s = input_script("300:LT+1000")
    assert s.startswith("8:START+200,")
    assert "100:LT+300" in s and "121:LT+300" in s
    assert s.endswith(",300:LT+1000")
    assert input_script("") == input_script("").rstrip(",")


def test_skip_training_window_covers_choose_path():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from test_milestone_upscaler import SKIP_TRAINING
    times = [int(e.split(":")[0]) for e in SKIP_TRAINING.split(",")]
    assert all(e.split(":")[1].startswith("LT+") for e in SKIP_TRAINING.split(","))
    assert times[0] == 100 and times[-1] == 121  # screen at 93-99 s; the last LT before the game (143 s)
    assert all(b - a <= 3 for a, b in zip(times, times[1:]))
