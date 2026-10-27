"""All targets that compile Xenia code use the same C++ standard.

GPU (canary) and APU share the base headers of thirdparty/xenia/src: in
C++17 byte_order.h defines a std::endian polyfill different from the one of
C++20, and the same executable would have two definitions (ODR).
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FILES = [ROOT / "cmake" / "XeniaGpu.cmake", ROOT / "cmake" / "XeniaApu.cmake", ROOT / "tests" / "CMakeLists.txt"]
_PROP = re.compile(r"set_target_properties\((\w+)\s+PROPERTIES\s+CXX_STANDARD\s+(\d+)")


def test_xenia_targets_share_the_cxx_standard():
    standards = {}
    for f in FILES:
        for target, std in _PROP.findall(f.read_text(encoding="utf-8")):
            if target.startswith("xenia_"):
                standards[target] = int(std)
    assert {"xenia_gpu_core", "xenia_gpu_d3d12", "xenia_apu", "xenia_apu_test"} <= standards.keys()
    assert set(standards.values()) == {20}, standards
