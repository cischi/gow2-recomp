"""Checks the XenonRecomp log and fails if it contains errors.

Usage: check_recomp_log.py <recomp.log> [--generated <folder>]

XenonRecomp reports problems only by printing them to stdout and still exits
with 0, so the only way not to miss them is to read the log.
With --generated it also checks the generated C++: calls (bl) to an
address that is not the start of a known function are only written as a
"// ERROR <addr>" comment in the source, with no message in the log.
"""
import re
import sys
from pathlib import Path

# Exact prefixes of XenonRecomp's error messages (commit ddd128b):
ERROR_PREFIXES = [
    # recompiler.cpp:48,86,621 e recompiler_config.cpp:41-48:
    #   "ERROR: Unable to apply the patch file, ...", "ERROR: Unable to load the patch file",
    #   "ERROR: Switch case at {:X} is trying to jump outside function: {:X}",
    #   "ERROR: __<helper> address is unspecified"
    "ERROR:",
    # recompiler.cpp:2422 - undecodable word (non-zero) inside a function
    "Unable to decode instruction ",
    # recompiler.cpp:2428 - bctr preceded by the switch pattern but with no table in the config
    "Found a switch jump table at ",
    # recompiler.cpp:2432 - decoded instruction with no translation
    "Unrecognized instruction at ",
    # recompiler.cpp:2005,2019 - vpkd3d128 with unhandled operands
    "Unexpected D3D color pack instruction at ",
    "Unexpected float16_4 pack instruction at ",
    # patches/xenonrecomp/0002: vpkd3d128 FLOAT16_2 with pack 0 (not handled by Xenia)
    "Unexpected float16_2 pack instruction at ",
]

# Messages that start with a variable name: recognized by their fixed trailing part.
ERROR_SUFFIXES = [
    # recompiler.cpp:2275 - "{} at {:X} has RC bit enabled but no comparison was generated"
    " has RC bit enabled but no comparison was generated",
    # recompiler_config.cpp:122,129 - inconsistent midasm hooks
    ": can't return and jump at the same time",
    ": can't mix direct and conditional return/jump",
]

# recompiler.cpp:2614 - last progress line, printed for the last function.
COMPLETION_LINE = "Recompiling functions... 100%"

# recompiler.cpp:402 - bl to an address that is not a known function.
UNRESOLVED_CALL = re.compile(r"^\t// ERROR ([0-9A-F]+)$")


def find_errors(text: str) -> list[str]:
    """Returns the log lines that are XenonRecomp error messages."""
    errors = []
    for line in text.splitlines():
        line = line.rstrip()
        if line.startswith(tuple(ERROR_PREFIXES)) or line.endswith(tuple(ERROR_SUFFIXES)):
            errors.append(line)
    return errors


def is_complete(text: str) -> bool:
    return any(line.rstrip() == COMPLETION_LINE for line in text.splitlines())


def find_unresolved_calls(generated_dir) -> list[str]:
    """Searches the generated C++ for calls to unresolved addresses."""
    found = []
    for cpp in sorted(Path(generated_dir).glob("*.cpp")):
        with cpp.open(encoding="utf-8", errors="replace") as f:
            for lineno, line in enumerate(f, 1):
                m = UNRESOLVED_CALL.match(line.rstrip("\r\n"))
                if m:
                    found.append(f"{cpp.name}:{lineno}: unresolved call to 0x{m.group(1)}")
    return found


# recompiler.cpp (BLRL, VCMPBFP, default of VPKD3D128/VUPKD3D128):
# __builtin_debugtrap() in place of an untranslated instruction, with no messages
# in the log. At runtime it becomes a 0x80000003 crash, so it is an error.
DEBUG_TRAP = re.compile(r"^\s*__builtin_debugtrap\(\);$")
FUNC_IMPL = re.compile(r"^PPC_FUNC_IMPL\(__imp__(\w+)\)")
INSN_COMMENT = re.compile(r"^\t// (.+)$")


def find_debug_traps(generated_dir) -> list[str]:
    """Searches the generated C++ for instructions left as __builtin_debugtrap()."""
    found = []
    for cpp in sorted(Path(generated_dir).glob("*.cpp")):
        func, insn = "?", "?"
        with cpp.open(encoding="utf-8", errors="replace") as f:
            for lineno, line in enumerate(f, 1):
                line = line.rstrip("\r\n")
                if m := FUNC_IMPL.match(line):
                    func = m.group(1)
                elif m := INSN_COMMENT.match(line):
                    insn = m.group(1).strip()
                elif DEBUG_TRAP.match(line):
                    found.append(f"{cpp.name}:{lineno}: untranslated instruction (__builtin_debugtrap) in {func}: {insn}")
    return found


def main(argv: list[str]) -> int:
    # non-ASCII paths: on Windows the default console (cp1252) cannot represent all of them
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    args = list(argv[1:])
    generated = None
    if "--generated" in args:
        i = args.index("--generated")
        if i + 1 >= len(args):
            print("usage: check_recomp_log.py <log> [--generated <folder>]", file=sys.stderr)
            return 2
        generated = Path(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        print("usage: check_recomp_log.py <log> [--generated <folder>]", file=sys.stderr)
        return 2

    log = Path(args[0])
    try:
        text = log.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        print(f"check_recomp_log: cannot read {log}: {e}", file=sys.stderr)
        return 1

    problems = find_errors(text)
    if not is_complete(text):
        problems.append(f"incomplete log: the line '{COMPLETION_LINE}' is missing "
                        "(XenonRecomp interrupted?)")
    if generated is not None:
        if not generated.is_dir():
            problems.append(f"generated code folder missing: {generated}")
        else:
            problems += find_unresolved_calls(generated)
            problems += find_debug_traps(generated)

    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        print(f"check_recomp_log: {len(problems)} errors in {log}", file=sys.stderr)
        return 1
    print(f"check_recomp_log: no errors in {log}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
