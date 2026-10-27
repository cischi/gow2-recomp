"""Finds the Xbox 360 compiler's register save/restore helpers.

Usage: find_helpers.py <image.bin> <info.json>
Prints TOML lines `key = 0xADDR` to stdout, to paste under [main] of the
XenonRecomp config. If a pattern is missing or appears more than once it exits with 1.
"""
import json
import struct
import sys
from pathlib import Path

# VX128_1 form (lvx128/stvx128): primary opcode 4, sub-opcode in bits 21-27
# with the extended register bits excluded by the mask.
VX128_1_MASK = 0xFC0007F3
STVX128 = 0x100001C3
LVX128 = 0x100000C3


def _is(op, mask=0xFFFFFFFF):
    return lambda w: (w & mask) == op


# key -> sequence of predicates on the consecutive words at the start of the routine
PATTERNS = {
    "savegprlr_14_address": [_is(0xF9C1FF68)],               # std r14,-0x98(r1)
    "restgprlr_14_address": [_is(0xE9C1FF68)],               # ld r14,-0x98(r1)
    # The FPR helpers address through r12 (the caller does addi r12,r1,-N before the bl),
    # not r1: the r1-based D9C1FF70/C9C1FF70 patterns do not appear in the real image.
    "savefpr_14_address": [_is(0xD9CCFF70)],                 # stfd f14,-0x90(r12)
    "restfpr_14_address": [_is(0xC9CCFF70)],                 # lfd f14,-0x90(r12)
    "savevmx_14_address": [_is(0x3960FEE0), _is(0x7DCB61CE)],  # li r11,-0x120; stvx v14,r11,r12
    "restvmx_14_address": [_is(0x3960FEE0), _is(0x7DCB60CE)],  # li r11,-0x120; lvx v14,r11,r12
    "savevmx_64_address": [_is(0x3960FC00), _is(STVX128, VX128_1_MASK)],  # li r11,-0x400; stvx128
    "restvmx_64_address": [_is(0x3960FC00), _is(LVX128, VX128_1_MASK)],   # li r11,-0x400; lvx128
}


def find_helpers(image: bytes, base: int) -> dict[str, int]:
    count = len(image) // 4
    words = struct.unpack(">%dI" % count, image[:count * 4])
    result = {}
    for key, preds in PATTERNS.items():
        first, rest = preds[0], preds[1:]
        hits = [i for i, w in enumerate(words)
                if first(w) and i + len(rest) < count
                and all(p(words[i + 1 + j]) for j, p in enumerate(rest))]
        if len(hits) != 1:
            what = "not found" if not hits else (
                "found %d times (%s)" % (len(hits), ", ".join("0x%X" % (base + 4 * i) for i in hits[:8])))
            raise ValueError(f"{key}: pattern {what}")
        result[key] = base + 4 * hits[0]
    return result


def main(argv):
    if len(argv) != 3:
        print("usage: find_helpers.py <image.bin> <info.json>", file=sys.stderr)
        return 2
    try:
        image = Path(argv[1]).read_bytes()
        info = json.loads(Path(argv[2]).read_text(encoding="utf-8"))
        found = find_helpers(image, int(info["base"]))
    except (OSError, ValueError, KeyError) as e:
        print(f"find_helpers: error: {e}", file=sys.stderr)
        return 1
    for key, addr in found.items():
        print(f"{key} = 0x{addr:X}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
