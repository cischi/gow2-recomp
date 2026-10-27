"""Finds code reached only through pointers: inline switches and functions.

Usage: find_missing_functions.py <image.bin> <info.json> <ppc_func_mapping.cpp>
                               [--switch-tables gow2_switch_tables.toml]

1) Inline switches: the sequence
       lis r12,T@ha / addi r12,r12,T@l / slwi r0,rX,2 / lwzx r0,r12,r0 /
       mtctr r0 / bctr
   immediately followed (T = bctr+4) by a table of pointers into code. Without a
   cmplwi/bgt before it, XenonAnalyse cannot derive the number of entries and does not
   list it: XenonRecomp translates the bctr as an indirect call and decodes the
   table as code. The table ends at the first word that does not point into
   code or where the first case begins. For each one it prints the [[switch]] entry and the
   size of the function that contains it (recursive descent following b, bc and the
   switches, up to blr/bctr or a jump to another .pdata function), if
   .pdata does not already cover it entirely.

2) Functions reached only through pointers: aligned 32-bit words across the whole
   image that point into a code section, to an address that is not
   a known function start (generated map), nor a switch target, nor
   inside a .pdata function (.pdata entries cover whole functions:
   an address inside them is a label, e.g. the end of a try block).
   Words inside code sections only count if a neighbor also points
   into code (table): on their own they are instructions (lwz r16-r23 has the
   encoding 0x82xxxxxx). The target is a plausible start if the previous word is a terminator
   (blr, bctr, b), a padding (0) or a pointer into code to a known start
   or to another target (tail of a table), and if the code from the target
   decodes up to a blr/bctr/b without invalid instructions and without
   entering the next known function. The size extends to the next
   function start, known or found.

Prints to stdout the lines `{ address = 0x..., size = 0x... }, # ...` for the
`functions` list of config/gow2.toml and, after a comment line, the
[[switch]] entries for gow2_switch_tables.toml; a summary on stderr.
"""
import argparse
import bisect
import json
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

BLR = 0x4E800020
BCTR = 0x4E800420
MTCTR_R0 = 0x7C0903A6
LWZX_R0_R12_R0 = 0x7C0C002E

# Primary opcodes that do not exist on the Xenon (0 also covers padding).
INVALID_PRIMARY = {0, 1, 9, 22, 56, 57, 60, 61}
# Valid extended opcodes of group 19 (bits 21-30).
OP19_VALID = {0, 16, 18, 33, 50, 129, 150, 193, 225, 257, 274, 289, 417, 449, 528}

SCN_CNT_CODE = 0x20
SCN_MEM_EXECUTE = 0x20000000

# Maximum distance the recursive descent follows past the function start.
MAX_FUNCTION_SPAN = 0x10000


@dataclass
class Candidate:
    address: int
    size: int
    sources: list = field(default_factory=list)
    table: tuple | None = None  # (start, number of words) of the table in code that precedes it


@dataclass
class InlineSwitch:
    base: int        # lis r12: address from which XenonRecomp activates the switch
    bctr: int
    r: int           # index register (rS of the slwi)
    labels: list
    function: int    # start of the function that contains it
    end: int         # end (exclusive) of the function, recursive descent


class Image:
    def __init__(self, data: bytes, base: int):
        self.base = base
        if data[:2] != b"MZ":
            raise ValueError("image without an MZ header")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("image without a PE header")
        nsec, = struct.unpack_from("<H", data, pe + 6)
        optsize, = struct.unpack_from("<H", data, pe + 20)
        off = pe + 24 + optsize
        self.code = []
        self.pdata = []  # (start, end) of the functions in .pdata
        for i in range(nsec):
            name, vsize, va, _, _, _, _, _, _, flags = struct.unpack_from("<8sIIIIIIHHI", data, off + 40 * i)
            if flags & (SCN_CNT_CODE | SCN_MEM_EXECUTE):
                self.code.append((base + va, base + va + vsize))
            if name.rstrip(b"\0") == b".pdata":
                for j in range(vsize // 8):
                    begin, info = struct.unpack_from(">II", data, va + 8 * j)
                    length = ((info >> 8) & 0x3FFFFF) * 4
                    if begin and length:
                        self.pdata.append((begin, begin + length))
        if not self.code:
            raise ValueError("no code section")
        self.code.sort()
        self.pdata.sort()
        self.pdata_starts = [b for b, _ in self.pdata]
        self.pdata_begin_set = set(self.pdata_starts)
        count = len(data) // 4
        self.words = struct.unpack(">%dI" % count, data[:count * 4])

    def word(self, addr):
        return self.words[(addr - self.base) // 4]

    def code_range(self, v):
        for lo, hi in self.code:
            if lo <= v < hi:
                return lo, hi
        return None

    def in_code(self, v):
        return v & 3 == 0 and self.code_range(v) is not None

    def in_exec(self, addr):
        return self.code_range(addr) is not None

    def pdata_containing(self, addr):
        j = bisect.bisect_right(self.pdata_starts, addr) - 1
        if j >= 0 and self.pdata[j][0] <= addr < self.pdata[j][1]:
            return self.pdata[j]
        return None


def is_valid(insn: int) -> bool:
    op = insn >> 26
    if op in INVALID_PRIMARY:
        return False
    if op == 19:
        return ((insn >> 1) & 0x3FF) in OP19_VALID
    if op == 58:
        return (insn & 3) != 3
    if op == 62:
        return (insn & 3) in (0, 1)
    return True


def is_terminator(insn: int) -> bool:
    if insn in (BLR, BCTR):
        return True
    return (insn >> 26) == 18 and (insn & 3) == 0  # relative b without link


def _branch_target(addr, insn):
    op = insn >> 26
    if op == 18:
        off = insn & 0x03FFFFFC
        if off & 0x02000000:
            off -= 0x04000000
    else:
        off = insn & 0xFFFC
        if off & 0x8000:
            off -= 0x10000
    return off if insn & 2 else addr + off


def switch_bctrs(img: Image, switches: dict) -> dict:
    """bctr -> labels for the already known switches (base = load instruction)."""
    result = {}
    for base, labels in switches.items():
        if img.code_range(base) is None:
            continue
        for k in range(16):
            a = base + 4 * k
            if img.code_range(a) is None:
                break
            if img.word(a) == BCTR:
                result[a] = labels
                break
    return result


def function_end(img: Image, start: int, bctr_labels: dict) -> int:
    """End (exclusive) of the function: recursive descent from start."""
    _, hi = img.code_range(start)
    limit = min(hi, start + MAX_FUNCTION_SPAN)
    seen = set()
    work = [start]
    end = start
    while work:
        addr = work.pop()
        while start <= addr < limit and addr not in seen:
            seen.add(addr)
            end = max(end, addr + 4)
            insn = img.word(addr)
            op = insn >> 26
            if insn == BLR:
                break
            if insn == BCTR:
                work.extend(bctr_labels.get(addr, ()))
                break
            if op == 18 and not insn & 1:  # b
                t = _branch_target(addr, insn)
                if t != start and t in img.pdata_begin_set:
                    break  # tail jump to another function
                work.append(t)
                break
            if op == 16 and not insn & 1:  # bc
                work.append(_branch_target(addr, insn))
            addr += 4
    return end


def find_inline_switches(img: Image, known: set, switch_bases: set, bctr_labels: dict) -> list:
    found = []
    starts = sorted(known)
    for lo, hi in img.code:
        for a in range(lo + 0x14, hi - 4, 4):
            if img.word(a) != BCTR or img.word(a - 4) != MTCTR_R0 or img.word(a - 8) != LWZX_R0_R12_R0:
                continue
            slwi, addi, lis = img.word(a - 12), img.word(a - 16), img.word(a - 20)
            # rlwinm r0,rS,2,0,29 / addi r12,r12,lo / lis r12,ha
            if slwi & 0xFC1FFFFF != 0x5400103A or addi >> 16 != 0x398C or lis >> 16 != 0x3D80:
                continue
            table = ((lis & 0xFFFF) << 16) + ((addi & 0xFFFF) ^ 0x8000) - 0x8000
            base = a - 20
            if table != a + 4 or base in switch_bases:
                continue
            labels = []
            t = table
            while t < hi and img.in_code(img.word(t)) and (not labels or t < min(labels)):
                labels.append(img.word(t))
                t += 4
            if not labels:
                continue
            p = img.pdata_containing(a)
            if p:
                function = p[0]
            else:
                j = bisect.bisect_right(starts, base) - 1
                function = starts[j] if j >= 0 else lo
            found.append(InlineSwitch(base, a, (slwi >> 21) & 31, labels, function, 0))
    all_labels = dict(bctr_labels)
    all_labels.update({s.bctr: s.labels for s in found})
    for s in found:
        s.end = function_end(img, s.function, all_labels)
    return found


def find_missing(image: bytes, base: int, known: set, switch_targets: set, extra_starts=()) -> list:
    """Functions reached only through pointers (point 2 of the module docstring)."""
    img = image if isinstance(image, Image) else Image(image, base)
    known = set(known)

    # all aligned values that point into code, with the words that contain them
    # (a word in code only counts if a neighbor also points into code:
    # table; on its own it is almost always an instruction, e.g. lwz r16-r23 has the
    # encoding 0x82xxxxxx)
    pointed = {}
    words = img.words
    for i, v in enumerate(words):
        if not img.in_code(v):
            continue
        src = base + 4 * i
        if img.in_exec(src) and not (img.in_code(words[i - 1]) or (i + 1 < len(words) and img.in_code(words[i + 1]))):
            continue
        pointed.setdefault(v, []).append(src)

    starts = sorted(known | set(extra_starts))

    def next_known(addr):
        j = bisect.bisect_right(starts, addr)
        return starts[j] if j < len(starts) else None

    def is_table_word(v):
        return img.in_code(v) and (v in known or v in pointed)

    found = {}
    for target in sorted(pointed):
        if target in known or target in switch_targets:
            continue
        lo, hi = img.code_range(target)
        if target == lo or img.pdata_containing(target):
            continue
        prev = img.word(target - 4)
        if not (is_terminator(prev) or prev == 0 or is_table_word(prev)):
            continue
        limit = min(x for x in (next_known(target), hi) if x is not None)
        addr = target
        ok = False
        while addr < limit:
            insn = img.word(addr)
            if not is_valid(insn):
                break
            if is_terminator(insn):
                ok = True
                break
            addr += 4
        if not ok:
            continue
        table = None
        if is_table_word(prev):
            first = target - 4
            while first - 4 >= lo and is_table_word(img.word(first - 4)):
                first -= 4
            table = (first, (target - first) // 4)
        found[target] = Candidate(target, 0, pointed[target], table)

    all_starts = sorted(set(starts) | set(found))
    result = []
    for target in sorted(found):
        j = bisect.bisect_right(all_starts, target)
        end = img.code_range(target)[1]
        if j < len(all_starts):
            end = min(end, all_starts[j])
        c = found[target]
        c.size = end - target
        result.append(c)
    return result


def covered_by_pdata(img: Image, function: int, end: int) -> bool:
    """True if a .pdata entry already contains [function, end): the function of the
    inline switch needs no `functions` entry (even if .pdata goes
    past the end found by the descent)."""
    p = img.pdata_containing(function)
    return bool(p) and p[0] <= function and p[1] >= end


def analyze(image: bytes, base: int, known: set, switches: dict):
    """Inline switches and missing functions. switches: base -> labels (known)."""
    img = image if isinstance(image, Image) else Image(image, base)
    bctr_labels = switch_bctrs(img, switches)
    inline = find_inline_switches(img, known, set(switches), bctr_labels)
    targets = {l for labels in switches.values() for l in labels}
    targets |= {l for s in inline for l in s.labels}
    functions = find_missing(img, base, known, targets, extra_starts={s.function for s in inline})
    return inline, functions


def read_known(mapping_text: str) -> set:
    return {int(m, 16) for m in re.findall(r"\{\s*0x([0-9A-Fa-f]+)\s*,\s*[A-Za-z_]\w*\s*\}", mapping_text)}


def read_switches(toml_text: str) -> dict:
    """base -> labels (with the default) of every [[switch]]; the file is generated, fixed format."""
    result = {}
    for block in re.split(r"^\s*\[\[switch\]\]\s*$", toml_text, flags=re.M)[1:]:
        m = re.search(r"^\s*base\s*=\s*0x([0-9A-Fa-f]+)", block, flags=re.M)
        if not m:
            continue
        labels = []
        lm = re.search(r"labels\s*=\s*\[([^\]]*)\]", block)
        if lm:
            labels = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", lm.group(1))]
        dm = re.search(r"^\s*default\s*=\s*0x([0-9A-Fa-f]+)", block, flags=re.M)
        if dm:
            labels.append(int(dm.group(1), 16))
        result[int(m.group(1), 16)] = labels
    return result


def describe(c: Candidate) -> str:
    if c.table:
        first, n = c.table
        return "after a table of %d pointers into code at 0x%X" % (n, first)
    src = ", ".join("0x%X" % s for s in c.sources[:3])
    more = " and %d more" % (len(c.sources) - 3) if len(c.sources) > 3 else ""
    return "pointed to by %s%s" % (src, more)


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("image")
    p.add_argument("info")
    p.add_argument("mapping")
    p.add_argument("--switch-tables")
    a = p.parse_args(argv[1:])
    try:
        image = Path(a.image).read_bytes()
        info = json.loads(Path(a.info).read_text(encoding="utf-8"))
        known = read_known(Path(a.mapping).read_text(encoding="utf-8"))
        switches = read_switches(Path(a.switch_tables).read_text(encoding="utf-8")) if a.switch_tables else {}
        img = Image(image, int(info["base"]))
        inline, functions = analyze(img, img.base, known, switches)
    except (OSError, ValueError, KeyError) as e:
        print(f"find_missing_functions: error: {e}", file=sys.stderr)
        return 1
    entries = [(s.function, s.end - s.function,
                "inline switch loaded at 0x%X: the function ends (exclusive) at 0x%X" % (s.base, s.end))
               for s in inline if not covered_by_pdata(img, s.function, s.end)]
    entries += [(c.address, c.size, describe(c)) for c in functions]
    for address, size, comment in sorted(set(entries)):
        print("    { address = 0x%X, size = 0x%X }, # %s" % (address, size, comment))
    if inline:
        print("# ---- gow2_switch_tables.toml ----")
    for s in inline:
        print("\n# 0x%X: absolute inline table at 0x%X, with no cmplwi/bgt before it: %d entries,"
              "\n# the table ends where the first case begins or at the first word outside the code."
              % (s.bctr, s.bctr + 4, len(s.labels)))
        print("[[switch]]\nbase = 0x%X\nr = %d\nlabels = [" % (s.base, s.r))
        for i in range(0, len(s.labels), 4):
            print("    " + " ".join("0x%X," % l for l in s.labels[i:i + 4]))
        print("]")
    print(f"find_missing_functions: inline switches: {len(inline)}, functions "
          f"reached only through pointers: {len(functions)} ({len(known)} known starts, {len(switches)} known switches)",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
