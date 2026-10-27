"""Tests of tools/gen_import_stubs.py on a synthetic header (no game bytes).

The declarations imitate XenonRecomp's ppc_recomp_shared.h: every kernel/XAM
import appears as `PPC_EXTERN_FUNC(__imp__Name);` next to the recompiled
functions (`sub_XXXXXXXX`) and the helpers (`__savegprlr_14`, `_xstart`...).
"""
import importlib.util
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / "tools" / "gen_import_stubs.py"

HEADER = """#pragma once

#include "ppc_config.h"
#include "ppc_context.h"

PPC_EXTERN_FUNC(sub_82220000);
PPC_EXTERN_FUNC(__imp__RtlInitializeCriticalSection);
PPC_EXTERN_FUNC(sub_8222004C);
PPC_EXTERN_FUNC(__imp__KeTlsAlloc);
PPC_EXTERN_FUNC(__imp__XamInputGetState);
PPC_EXTERN_FUNC(__savegprlr_14);
PPC_EXTERN_FUNC(_xstart);
"""


def _load():
    spec = importlib.util.spec_from_file_location("gen_import_stubs", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_parse_imports_returns_the_three_names():
    mod = _load()
    assert mod.parse_imports(HEADER) == [
        "RtlInitializeCriticalSection",
        "KeTlsAlloc",
        "XamInputGetState",
    ]


def test_parse_imports_ignores_impl_names_and_duplicates():
    mod = _load()
    header = HEADER + "PPC_EXTERN_FUNC(__imp__sub_82220000);\nPPC_EXTERN_FUNC(__imp__KeTlsAlloc);\n"
    assert mod.parse_imports(header) == [
        "RtlInitializeCriticalSection",
        "KeTlsAlloc",
        "XamInputGetState",
    ]


def test_generate_has_one_stub_per_import():
    mod = _load()
    out = mod.generate(mod.parse_imports(HEADER))
    assert out.count("ctx.r3.u64 = 0") == 3
    for name in ("RtlInitializeCriticalSection", "KeTlsAlloc", "XamInputGetState"):
        assert f"PPC_FUNC(__imp__{name})" in out
        assert f'LogImport("{name}", ctx);' in out
    assert '#include "ppc_recomp_shared.h"' in out


def test_cli_writes_output(tmp_path):
    d = tmp_path / "dir with space [x] ü"
    d.mkdir()
    header = d / "ppc_recomp_shared.h"
    header.write_text(HEADER, encoding="utf-8")
    out = d / "sub" / "import_stubs.cpp"
    proc = subprocess.run([sys.executable, str(SCRIPT), str(header), str(out)],
                          capture_output=True, text=True, encoding="utf-8")
    assert proc.returncode == 0, proc.stderr
    assert out.read_text(encoding="utf-8").count("ctx.r3.u64 = 0") == 3


def test_cli_header_without_imports_fails(tmp_path):
    header = tmp_path / "empty.h"
    header.write_text("PPC_EXTERN_FUNC(sub_82220000);\n", encoding="utf-8")
    proc = subprocess.run([sys.executable, str(SCRIPT), str(header), str(tmp_path / "o.cpp")],
                          capture_output=True, text=True, encoding="utf-8")
    assert proc.returncode == 1
    assert "no imports" in proc.stderr


def test_cli_missing_header_fails(tmp_path):
    proc = subprocess.run([sys.executable, str(SCRIPT), str(tmp_path / "missing.h"), str(tmp_path / "o.cpp")],
                          capture_output=True, text=True, encoding="utf-8")
    assert proc.returncode == 1
    assert "missing.h" in proc.stderr


SOURCES = [
    """#include "kernel/registry.h"
// KERNEL_EXPORT(InComment, X) counts anyway: it is just a list of names
uint32_t NtCloseImpl(uint32_t handle) { return 0; }
KERNEL_EXPORT(NtClose, NtCloseImpl)
""",
    "KERNEL_EXPORT( KeSetEvent ,\n    KeSetEventImpl)\n",
    "PPC_FUNC(__imp__KeTlsAlloc) {}\n",
    "KERNEL_EXPORT_RAW(KeBugCheck) { }\n",
]


def test_implemented_names_finds_kernel_exports():
    mod = _load()
    names = mod.implemented_names(SOURCES)
    assert {"NtClose", "KeSetEvent", "KeBugCheck"} <= names
    assert "KeTlsAlloc" not in names


def test_generate_skips_implemented_names():
    mod = _load()
    names = ["NtClose", "KeTlsAlloc", "KeSetEvent"]
    out = mod.generate(names, mod.implemented_names(SOURCES))
    assert "__imp__NtClose" not in out
    assert "__imp__KeSetEvent" not in out
    assert "PPC_FUNC(__imp__KeTlsAlloc)" in out
    assert out.count("ctx.r3.u64 = 0") == 1


def test_generate_stub_traces_and_notes_unimplemented():
    mod = _load()
    out = mod.generate(["KeTlsAlloc"])
    assert '#include "kernel/stats.h"' in out
    assert 'if (kernel::stats::Trace()) LogImport("KeTlsAlloc", ctx);' in out
    # hot path without locks: one static slot per stub, registered on the first call
    assert 'static kernel::stats::UnimplementedSlot s_slot{"KeTlsAlloc"};' in out
    assert "kernel::stats::NoteUnimplemented(s_slot, ImportRegs(ctx));" in out
    assert 'NoteUnimplemented("KeTlsAlloc"' not in out


def test_cli_implemented_from_dir(tmp_path):
    header = tmp_path / "ppc_recomp_shared.h"
    header.write_text(HEADER, encoding="utf-8")
    kdir = tmp_path / "kernel"
    (kdir / "io").mkdir(parents=True)
    (kdir / "sync_exports.cpp").write_text("KERNEL_EXPORT(KeTlsAlloc, F)\n", encoding="utf-8")
    (kdir / "io" / "file_exports.cpp").write_text("KERNEL_EXPORT(XamInputGetState, G)\n", encoding="utf-8")
    (kdir / "ignored.h").write_text("KERNEL_EXPORT(RtlInitializeCriticalSection, H)\n", encoding="utf-8")
    out = tmp_path / "import_stubs.cpp"
    proc = subprocess.run([sys.executable, str(SCRIPT), str(header), str(out), "--implemented-from", str(kdir)],
                          capture_output=True, text=True, encoding="utf-8")
    assert proc.returncode == 0, proc.stderr
    text = out.read_text(encoding="utf-8")
    assert "PPC_FUNC(__imp__RtlInitializeCriticalSection)" in text
    assert "__imp__KeTlsAlloc" not in text
    assert "__imp__XamInputGetState" not in text


def test_cli_implemented_from_missing_dir_fails(tmp_path):
    header = tmp_path / "ppc_recomp_shared.h"
    header.write_text(HEADER, encoding="utf-8")
    proc = subprocess.run([sys.executable, str(SCRIPT), str(header), str(tmp_path / "o.cpp"),
                           "--implemented-from", str(tmp_path / "missing_dir")],
                          capture_output=True, text=True, encoding="utf-8")
    assert proc.returncode == 1
    assert "missing_dir" in proc.stderr


def test_cli_implemented_from_multiple_dirs(tmp_path):
    # runtime/kernel and runtime/gpu_fake: --implemented-from repeated, the names add up
    header = tmp_path / "ppc_recomp_shared.h"
    header.write_text(HEADER, encoding="utf-8")
    kdir = tmp_path / "kernel"
    gdir = tmp_path / "gpu_fake"
    kdir.mkdir()
    gdir.mkdir()
    (kdir / "sync_exports.cpp").write_text("KERNEL_EXPORT(KeTlsAlloc, F)\n", encoding="utf-8")
    (gdir / "vd_exports.cpp").write_text("KERNEL_EXPORT(XamInputGetState, G)\n", encoding="utf-8")
    out = tmp_path / "import_stubs.cpp"
    proc = subprocess.run([sys.executable, str(SCRIPT), str(header), str(out),
                           "--implemented-from", str(kdir), "--implemented-from", str(gdir)],
                          capture_output=True, text=True, encoding="utf-8")
    assert proc.returncode == 0, proc.stderr
    text = out.read_text(encoding="utf-8")
    assert "__imp__KeTlsAlloc" not in text
    assert "__imp__XamInputGetState" not in text
    assert "PPC_FUNC(__imp__RtlInitializeCriticalSection)" in text
    assert "stubs: 1 (2 imports implemented)" in proc.stdout


def test_cli_implemented_from_second_dir_missing_fails(tmp_path):
    header = tmp_path / "ppc_recomp_shared.h"
    header.write_text(HEADER, encoding="utf-8")
    kdir = tmp_path / "kernel"
    kdir.mkdir()
    proc = subprocess.run([sys.executable, str(SCRIPT), str(header), str(tmp_path / "o.cpp"),
                           "--implemented-from", str(kdir), "--implemented-from", str(tmp_path / "missing_dir")],
                          capture_output=True, text=True, encoding="utf-8")
    assert proc.returncode == 1
    assert "missing_dir" in proc.stderr
