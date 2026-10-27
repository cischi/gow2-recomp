"""pytest configuration: registers the `gioco` marker and isolates gow2.exe from the launcher's gow2.toml."""

import os
import tempfile
from pathlib import Path


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "gioco: runs gow2.exe with the game data for tens of seconds (long suite; "
        "excluded from the short suite with -m \"not gioco\")")
    # The gow2.toml next to gow2.exe (written by the launcher) would change the test runs:
    # without --config, gow2.exe reads this empty file (GOW2_CONFIG, inherited by subprocesses).
    empty = Path(tempfile.gettempdir()) / f"gow2_pytest_{os.getpid()}.toml"
    empty.write_text("", encoding="utf-8")
    os.environ["GOW2_CONFIG"] = str(empty)
