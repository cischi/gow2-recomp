"""Vendored NVAPI (thirdparty/nvapi): license, minimal excerpt, provenance, isolation."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NVAPI = ROOT / "thirdparty" / "nvapi"
ONLY_USER = Path("runtime/platform/platform_win32_display.cpp")


def read(p):
    return p.read_text(encoding="utf-8")


def test_mit_license():
    t = read(NVAPI / "License.txt")
    assert "MIT" in t and "Permission is hereby granted" in t


def test_minimal_excerpt():
    t = read(NVAPI / "nvapi_vrr.h")
    assert "Permission is hereby granted" in t
    for s in ("0x0150e828", "0xae457190", "0xdf8fda57", "NV_GET_VRR_INFO_V1"):
        assert s in t.lower() or s in t, s
    assert not re.search(r'#\s*include\s*[<"]nvapi', t, re.I)


def test_provenance():
    t = read(NVAPI / "VENDORED.md")
    assert "https://github.com/NVIDIA/nvapi" in t
    assert re.search(r"\b[0-9a-f]{40}\b", t)
    assert "nvapi_vrr.h" in t


def test_notice_names_nvapi():
    assert "NVAPI" in read(ROOT / "NOTICE")


def test_no_nvapi_include_outside_the_display_module():
    pat = re.compile(r'#\s*include\s*[<"][^>"]*nvapi', re.I)
    for base in ("runtime", "tests"):
        for p in (ROOT / base).rglob("*"):
            if p.suffix not in (".h", ".hpp", ".cpp", ".cc", ".inl") or p.relative_to(ROOT) == ONLY_USER:
                continue
            assert not pat.search(p.read_text(encoding="utf-8", errors="ignore")), p
