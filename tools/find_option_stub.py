#!/usr/bin/env python3
"""Find BaseOptionRegistry option-getter stubs for a given option id.

Stub shape (26.50):
    stp x29,x30,[sp,#-0x10]! / mov x29,sp / ldr x8,[x0] /
    mov w1,#ID / ldr x8,[x8,#0x10] / blr x8 / ldp / b <helper>

We locate the `mov w1,#ID` encoding and check the surrounding instructions.
Backend: capstone if installed, else llvm-objdump/objdump (tools/lib/asm_backend.py),
so this works on Android/bionic/Termux as well.

Usage:  python3 tools/find_option_stub.py <lib.so> <id> [id...]     (ids decimal or 0x..)
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lib.asm_backend import backend_name, disassemble, file_data  # noqa: E402


def movz_w1(imm):
    """MOVZ W1, #imm -> bytes"""
    return struct.pack("<I", 0x52800001 | ((imm & 0xFFFF) << 5))


def norm(text: str) -> str:
    """Normalise an instruction text so capstone and objdump compare equal.

    capstone prints small immediates in decimal (#45), objdump often in hex
    (#0x2d), and both may drop the space after the comma.
    """
    def repl(m):
        return "#" + str(int(m.group(0)[1:], 16))
    t = re.sub(r"#0x[0-9a-fA-F]+", repl, text.lower())
    t = re.sub(r"\s+", " ", t)
    t = t.replace(", ", ",")
    return t.strip()


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip())
        print(f"\nactive backend: {backend_name()}")
        return 2
    lib = sys.argv[1]
    ids = [int(x, 0) for x in sys.argv[2:]]
    d = file_data(lib)

    for oid in ids:
        needle = movz_w1(oid)
        hits = []
        pos = 0
        while True:
            pos = d.find(needle, pos)
            if pos < 0:
                break
            hits.append(pos)
            pos += 1
        print(f"option {oid} (0x{oid:x}): {len(hits)} 处 'mov w1,#{oid}'")
        shown = 0
        for h in hits:
            start = h - 12
            if start < 0:
                continue
            insns = disassemble(lib, start, 10)
            if len(insns) < 8:
                continue
            text = [norm(f"{i.mnemonic} {i.op_str}") for i in insns]
            want = {norm(f"mov w1, #{oid}"), norm(f"mov w1, #{hex(oid)}")}
            if (text[0].startswith("stp x29,x30,[sp,#-0x10]") and text[1] == "mov x29,sp"
                    and text[2] == "ldr x8,[x0]" and text[3] in want):
                print(f"    STUB @0x{start:x}: " + " ; ".join(text[:8]))
                shown += 1
                if shown >= 4:
                    break
        if shown == 0:
            print("    (无标准 stub: 该 id 可能已内联或结构改变)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
