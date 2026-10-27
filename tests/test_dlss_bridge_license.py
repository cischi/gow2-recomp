"""License boundary of the DLSS bridge.

runtime/gpu/dlss_bridge/ is MIT and includes no GPL headers of the project; the NVIDIA SDK headers
(nvsdk_ngx*) are only included there.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BRIDGE = REPO / "runtime" / "gpu" / "dlss_bridge"
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
SOURCES = ("*.h", "*.hpp", "*.c", "*.cc", "*.cpp", "*.inc")


def _sources(root: Path):
    for pattern in SOURCES:
        yield from root.rglob(pattern)


def test_bridge_is_mit():
    text = (BRIDGE / "LICENSE.txt").read_text(encoding="utf-8")
    assert text.startswith("MIT License")
    assert "not included in this repository" in text


def test_bridge_includes_no_project_headers():
    """No include of the bridge resolves to a GPL file of the project (runtime/, tests/, except the third-party
    copies in third_party/) outside the bridge."""
    project = {}
    for top in ("runtime", "tests"):
        for f in _sources(REPO / top):
            if BRIDGE not in f.parents and "third_party" not in f.parts:
                project.setdefault(f.name, []).append(f.relative_to(REPO).as_posix())
    for f in _sources(BRIDGE):
        for inc in INCLUDE.findall(f.read_text(encoding="utf-8", errors="replace")):
            hits = [h for h in project.get(Path(inc).name, []) if h.endswith(inc)]
            assert not hits, f"{f.relative_to(REPO)} includes {inc} of the project ({hits[0]})"


def test_ngx_headers_only_in_bridge():
    for f in _sources(REPO / "runtime"):
        if BRIDGE in f.parents:
            continue
        text = f.read_text(encoding="utf-8", errors="replace")
        assert not any(i.startswith("nvsdk_ngx") for i in INCLUDE.findall(text)), f.relative_to(REPO)
