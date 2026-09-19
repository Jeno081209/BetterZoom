#!/usr/bin/env python3
"""Disassemble a function at a vaddr (or file offset) in libminecraftpe.so.

Usage:  python3 tools/disasm.py <lib.so> <addr> [count]
Backend: capstone if installed, else llvm-objdump/objdump (tools/lib/asm_backend.py),
so this works on Android/bionic/Termux too.  `--backend` prints which one is active.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lib.asm_backend import (backend_name, disassemble,  # noqa: E402
                            file_data, parse_elf_loads, vaddr_to_offset)

if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help", "--backend"):
    print(__doc__.strip())
    print(f"\nactive backend: {backend_name()}")
    sys.exit(0)

path = sys.argv[1]
target = int(sys.argv[2], 16)
count = int(sys.argv[3]) if len(sys.argv) > 3 else 24

data = file_data(path)
segs = parse_elf_loads(data)
foff = vaddr_to_offset(segs, target)
print(f"target vaddr 0x{target:x} -> file offset "
      f"{('0x%x' % foff) if foff is not None else 'NOT MAPPED'}")
if foff is None:
    foff = target
    print(f"  (treating 0x{target:x} as a file offset directly)")

for insn in disassemble(path, target, count):
    print(f"  0x{insn.address:x}:  {insn.mnemonic:<10} {insn.op_str}")
