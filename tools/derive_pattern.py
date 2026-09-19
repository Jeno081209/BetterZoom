#!/usr/bin/env python3
"""Cross-version function locator for libminecraftpe.so.

Given a function address that is KNOWN GOOD in one game build, this builds a
version-tolerant byte pattern for it and searches another build for matches.

How the pattern is built: the function's first N instructions are encoded to
bytes; every instruction whose encoding depends on its own address (PC-relative
ones: ADRP/ADR/B/BL/B.cond/CBZ/CBNZ/TBZ/TBNZ/LDR-literal) is wildcarded, since
those bytes legitimately change when surrounding code shifts. Absolute
instructions (prologue, moves, vtable loads, FMOV/FMUL...) are pinned.

That is exactly the wildcarding rule the community uses for shipped
signatures, applied automatically instead of by hand.

Usage:
    python3 tools/derive_pattern.py --lib OLD.so --addr 0xae31ec0
    python3 tools/derive_pattern.py --lib OLD.so --addr 0xae31ec0 --find-in NEW.so
    python3 tools/derive_pattern.py --lib OLD.so --addr 0xae31ec0 --find-in NEW.so --show

Disassembly backend: capstone if installed, otherwise llvm-objdump/objdump
(see tools/lib/asm_backend.py -- this is what makes the tool work on
Android/bionic/Termux, where capstone is often unavailable).
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lib.asm_backend import (backend_name, disassemble, file_data,  # noqa: E402
                            is_pc_relative, parse_elf_loads, vaddr_to_offset)


def derive(data, segs, path, addr, count):
    off = vaddr_to_offset(segs, addr)
    if off is None:
        raise SystemExit(f"address 0x{addr:x} is not inside a PT_LOAD segment")
    insns = disassemble(path, addr, count)
    if not insns:
        raise SystemExit("nothing decoded at that address "
                         f"(backend: {backend_name()})")
    values, masks = [], []
    for insn in insns:
        raw = insn.raw or b"\x00\x00\x00\x00"
        wild = is_pc_relative(insn)
        for b in raw:
            values.append(0 if wild else b)
            masks.append(0 if wild else 0xFF)
    return values, masks, insns


def to_pattern(values, masks):
    return " ".join("??" if m == 0 else f"{v:02X}" for v, m in zip(values, masks))


def find_all(data, values, masks):
    """Fast scan: locate via the longest fully-pinned run, then verify."""
    hits = []
    run_len = best_start = 0
    i = 0
    n = len(masks)
    while i < n:
        if masks[i] == 0xFF:
            j = i
            while j < n and masks[j] == 0xFF:
                j += 1
            if j - i > run_len:
                run_len, best_start = j - i, i
            i = j
        else:
            i += 1
    if run_len < 4:
        raise SystemExit("pattern has no long pinned run; cannot scan fast")
    needle = bytes(values[best_start:best_start + run_len])
    pos = 0
    while True:
        pos = data.find(needle, pos)
        if pos < 0:
            break
        base = pos - best_start
        if base >= 0 and base + n <= len(data):
            ok = True
            for k in range(n):
                if masks[k] and data[base + k] != values[k]:
                    ok = False
                    break
            if ok:
                hits.append(base)
        pos += 1
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", help="library holding the KNOWN-GOOD function")
    ap.add_argument("--addr", help="function address, e.g. 0xae31ec0")
    ap.add_argument("--insns", type=int, default=12)
    ap.add_argument("--find-in", dest="find_in", help="library to search")
    ap.add_argument("--show", action="store_true", help="disassemble the match")
    ap.add_argument("--backend", action="store_true", help="print the disassembly backend and exit")
    args = ap.parse_args()

    if args.backend:
        print(backend_name())
        return 0
    if not args.lib or not args.addr:
        ap.error("--lib and --addr are required (or use --backend to just report the backend)")

    addr = int(args.addr, 16)
    old = file_data(args.lib)
    segs = parse_elf_loads(old)
    values, masks, insns = derive(old, segs, args.lib, addr, args.insns)

    print(f"source: {args.lib} @ 0x{addr:x} ({len(insns)} instructions)")
    for insn in insns:
        mark = "  <- wildcarded" if is_pc_relative(insn) else ""
        print(f"    0x{insn.address:x}: {insn.mnemonic:<9} {insn.op_str}{mark}")
    pattern = to_pattern(values, masks)
    print(f"\npattern ({len(insns)} insns, {pattern.count('??')} wildcard bytes):")
    print(f'    "{pattern}"')

    if not args.find_in:
        return 0

    new = file_data(args.find_in)
    hits = find_all(new, values, masks)
    print(f"\nsearching {args.find_in}: {len(hits)} hit(s)")
    for h in hits:
        print(f"    0x{h:x}")
    if len(hits) == 1:
        print("\nUNIQUE -> safe to use as a signature on this build")
    elif not hits:
        print("\nMISSING -> the function changed shape; derive a new pattern by hand")
    else:
        print("\nAMBIGUOUS -> pin more instructions (--insns N) or disambiguate by hand")

    if args.show and hits:
        off = hits[0]
        print(f"\ndisassembly at 0x{off:x}:")
        for insn in disassemble(args.find_in, off, args.insns):
            print(f"    0x{insn.address:x}: {insn.mnemonic:<9} {insn.op_str}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
