"""Tests of tools/check_recomp_log.py on synthetic logs (no game bytes)."""
import importlib.util
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / "tools" / "check_recomp_log.py"

PROGRESS_LOG = (
    "Recompiling functions... 0.0012%\n"
    "Recompiling functions... 25.3%\n"
    "Recompiling functions... 100%\n"
)

# One line for every error message XenonRecomp prints to stdout,
# with the same formats as recompiler.cpp / recompiler_config.cpp.
ERROR_LINES = [
    "ERROR: Switch case at 82222040 is trying to jump outside function: 82223000",
    "ERROR: __savefpr_14 address is unspecified",
    "ERROR: Unable to load the patch file",
    "ERROR: Unable to apply the patch file, patch failed",
    "Unable to decode instruction 13371337 at 82100000",
    "Found a switch jump table at 82100010 with no switch table entry present",
    "Unrecognized instruction at 0x82100020: vfoo128",
    "Unexpected D3D color pack instruction at 82100030",
    "Unexpected float16_4 pack instruction at 82100040",
    "Unexpected float16_2 pack instruction at 82100048",
    "rlwinm. at 82100050 has RC bit enabled but no comparison was generated",
    "hook_x: can't return and jump at the same time",
    "hook_y: can't mix direct and conditional return/jump",
]


def _load():
    spec = importlib.util.spec_from_file_location("check_recomp_log", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)],
                          capture_output=True, text=True, encoding="utf-8")


def test_progress_only_log_has_no_errors():
    assert _load().find_errors(PROGRESS_LOG) == []


def test_one_entry_per_error_line():
    text = PROGRESS_LOG.replace("25.3%\n", "25.3%\n" + "\n".join(ERROR_LINES) + "\n")
    assert _load().find_errors(text) == ERROR_LINES


def test_every_prefix_is_detected():
    mod = _load()
    for prefix in mod.ERROR_PREFIXES:
        assert mod.find_errors(prefix + "xyz\n") == [prefix + "xyz"]


def test_crlf_and_trailing_spaces_are_stripped():
    assert _load().find_errors("Unrecognized instruction at 0x1: x  \r\n") == [
        "Unrecognized instruction at 0x1: x"]


def test_unresolved_calls_in_generated_code(tmp_path):
    (tmp_path / "ppc_recomp.0.cpp").write_text(
        "PPC_FUNC_IMPL(__imp__sub_82100000) {\n"
        "\t// bl 0x82abcdef\n"
        "\t// ERROR 82ABCDEF\n"
        "\t\t// ERROR: 0x82100040\n"  # switch case: already reported in the log
        "}\n", encoding="utf-8")
    (tmp_path / "ppc_recomp.1.cpp").write_text("\tsub_82100000(ctx, base);\n", encoding="utf-8")
    found = _load().find_unresolved_calls(tmp_path)
    assert found == ["ppc_recomp.0.cpp:3: unresolved call to 0x82ABCDEF"]


def test_debug_traps_in_generated_code(tmp_path):
    # XenonRecomp writes __builtin_debugtrap() in place of the instructions it
    # cannot translate (blrl, vcmpbfp, vpkd3d128/vupkd3d128 types) with
    # no message in the log: the check looks for them in the generated C++.
    (tmp_path / "ppc_recomp.0.cpp").write_text(
        "PPC_FUNC_IMPL(__imp__sub_82100000) {\n"
        "\t// vpkd3d128 v63,v1,3,1,3\n"
        "\tctx.fpscr.enableFlushMode();\n"
        "\t__builtin_debugtrap();\n"
        "\t// blrl \n"
        "__builtin_debugtrap();\n"
        "}\n", encoding="utf-8")
    found = _load().find_debug_traps(tmp_path)
    assert found == [
        "ppc_recomp.0.cpp:4: untranslated instruction (__builtin_debugtrap) in sub_82100000: vpkd3d128 v63,v1,3,1,3",
        "ppc_recomp.0.cpp:6: untranslated instruction (__builtin_debugtrap) in sub_82100000: blrl",
    ]


def test_cli_generated_dir_with_debug_trap_exits_1(tmp_path):
    log = tmp_path / "recomp.log"
    log.write_text(PROGRESS_LOG, encoding="utf-8")
    gen = tmp_path / "gen"
    gen.mkdir()
    (gen / "ppc_recomp.0.cpp").write_text(
        "PPC_FUNC_IMPL(__imp__sub_82100000) {\n\t// vcmpbfp128 v1,v2,v3\n\t__builtin_debugtrap();\n}\n",
        encoding="utf-8")
    r = _run(log, "--generated", gen)
    assert r.returncode == 1
    assert "vcmpbfp128 v1,v2,v3" in r.stderr


def test_cli_clean_log_exits_0(tmp_path):
    log = tmp_path / "dir [x] ü" / "recomp.log"
    log.parent.mkdir()
    log.write_text(PROGRESS_LOG, encoding="utf-8")
    r = _run(log)
    assert r.returncode == 0, r.stderr


def test_cli_errors_exit_1_and_are_printed(tmp_path):
    log = tmp_path / "recomp.log"
    log.write_text(PROGRESS_LOG + ERROR_LINES[0] + "\n" + ERROR_LINES[6] + "\n", encoding="utf-8")
    r = _run(log)
    assert r.returncode == 1
    assert ERROR_LINES[0] in r.stderr
    assert ERROR_LINES[6] in r.stderr
    assert "2 errors" in r.stderr


def test_cli_incomplete_log_exits_1(tmp_path):
    # XenonRecomp interrupted before the end: no "100%" line
    log = tmp_path / "recomp.log"
    log.write_text("Recompiling functions... 0.0012%\n", encoding="utf-8")
    r = _run(log)
    assert r.returncode == 1
    assert "incomplete" in r.stderr


def test_cli_generated_dir_with_unresolved_call_exits_1(tmp_path):
    log = tmp_path / "recomp.log"
    log.write_text(PROGRESS_LOG, encoding="utf-8")
    gen = tmp_path / "generated"
    gen.mkdir()
    (gen / "ppc_recomp.0.cpp").write_text("\t// ERROR 82ABCDEF\n", encoding="utf-8")
    r = _run(log, "--generated", gen)
    assert r.returncode == 1
    assert "0x82ABCDEF" in r.stderr


def test_cli_missing_log_exits_1(tmp_path):
    r = _run(tmp_path / "nope.log")
    assert r.returncode == 1
    assert "nope.log" in r.stderr
