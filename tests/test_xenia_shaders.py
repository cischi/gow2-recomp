"""Xenia D3D12 shaders compiled by the build (cmake/XeniaShaders.cmake).

The bytecode headers are not vendored: tools/vendor_xenia.py copies the
.xesl/.hlsl sources and the build compiles them with fxc into
build/clang-release/xenia_shaders/xenia/<gpu|ui>/shaders/bytecode/d3d12_5_1/.
"""

import importlib.util
import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
GENERATED = REPO / "build" / "clang-release" / "xenia_shaders"


def _load():
    spec = importlib.util.spec_from_file_location("vendor_xenia", REPO / "tools" / "vendor_xenia.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_shader_stage_rejects_ids_without_stage():
    mod = _load()
    assert mod.shader_stage("resolve_full_32bpp_cs") == "cs"
    assert mod.shader_stage("guest_output_triangle_strip_rect_vs") == "vs"
    for bad in ("resolve_full_32bpp", "foo_xx", "cs", "foo_ps_"):
        with pytest.raises(ValueError):
            mod.shader_stage(bad)


def test_generated_headers_define_the_included_identifiers():
    # With xenia-canary the byte-for-byte comparison with Xenia's bytecode
    # @95a5c3e no longer holds (different shaders and fxc flags, /O3 /Qstrip_*):
    # every header included by the vendored code and by the presenter exists and
    # defines the array with its identifier (/Vn).
    headers = sorted(GENERATED.rglob("*.h"))
    if not headers:
        pytest.skip(f"build not run: {GENERATED}")
    vendor = REPO / "thirdparty" / "xenia"
    included = set()
    sources = list((vendor / "src").rglob("*.cc")) + list((vendor / "src").rglob("*.h")) + \
        list((REPO / "runtime" / "gpu").glob("*.cpp"))
    for src in sources:
        included |= set(re.findall(r'#include "(xenia/(?:gpu|ui)/shaders/bytecode/d3d12_5_1/[^"]+\.h)"',
                                   src.read_text(encoding="utf-8", errors="replace")))
    assert included
    generated = {h.relative_to(GENERATED).as_posix(): h for h in headers}
    assert included <= set(generated)
    for rel in included:
        shader_id = rel.rsplit("/", 1)[-1][:-2]
        assert re.search(rf"\b{shader_id}\[\]", generated[rel].read_text(encoding="utf-8")), rel
