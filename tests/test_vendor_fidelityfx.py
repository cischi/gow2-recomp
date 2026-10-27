"""FidelityFX SDK v2.3.0: vendored MIT headers, signed DLLs downloaded by CMake (never committed)."""
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FFX = ROOT / "thirdparty" / "fidelityfx"
HEADERS = [
    "Kits/FidelityFX/api/include/ffx_api.h",
    "Kits/FidelityFX/api/include/ffx_api_types.h",
    "Kits/FidelityFX/api/include/ffx_api_loader.h",
    "Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h",
    "Kits/FidelityFX/upscalers/include/ffx_upscale.h",
    "Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h",
    "Kits/FidelityFX/framegeneration/include/ffx_framegeneration_api_types.h",
    "Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h",
]
SHA256 = {
    "amd_fidelityfx_loader_dx12.dll": "e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa",
    "amd_fidelityfx_upscaler_dx12.dll": "d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46",
    "amd_fidelityfx_framegeneration_dx12.dll": "02297beedd285e822d3a64f314cf00faf378dcec0edc47ff0c4dd71b3a8c2f18",
}
BASE_URL = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.3.0/Kits/FidelityFX/signedbin"


def test_headers_are_vendored_with_mit_notice():
    for h in HEADERS:
        text = (FFX / h).read_text(encoding="utf-8")
        assert "Advanced Micro Devices" in text and "Permission is hereby granted" in text, h


def test_vendored_md_names_tag_commit_and_files():
    md = (FFX / "VENDORED.md").read_text(encoding="utf-8")
    assert "v2.3.0" in md and "60f4ea81909200d8542eca14dccb2628b763a9a3" in md
    for h in HEADERS:
        assert h in md, h
    for dll in SHA256:
        assert dll in md, dll
    assert (FFX / "LICENSE.md").is_file()


def test_cmake_pins_url_and_hashes():
    cmake = (ROOT / "cmake" / "FidelityFX.cmake").read_text(encoding="utf-8")
    assert BASE_URL in cmake
    for dll, sha in SHA256.items():
        assert re.search(re.escape(dll) + r"[\s\S]{0,200}" + sha, cmake), dll


def test_framegeneration_dll_is_in_a_separate_list():
    cmake = (ROOT / "cmake" / "FidelityFX.cmake").read_text(encoding="utf-8")
    upscaler = re.search(r"set\(_gow2_ffx_files([\s\S]*?)\)", cmake).group(1)
    fg = re.search(r"set\(_gow2_ffx_fg_files([\s\S]*?)\)", cmake).group(1)
    assert "framegeneration" not in upscaler
    assert "amd_fidelityfx_framegeneration_dx12.dll" in fg
    assert "GOW2_FIDELITYFX_FG_DLLS" in cmake


def test_no_dll_is_tracked():
    tracked = subprocess.run(["git", "ls-files", "thirdparty/fidelityfx"], cwd=ROOT, capture_output=True,
                             text=True, check=True).stdout.split()
    assert tracked, "thirdparty/fidelityfx is not versioned"
    assert not [f for f in tracked if f.lower().endswith((".dll", ".lib"))]
