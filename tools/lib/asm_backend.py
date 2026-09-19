#!/usr/bin/env python3
"""ARM64 disassembly backend that works with OR without capstone.

Why this exists
---------------
The version-adaptation tools (derive_pattern.py, disasm.py, find_option_stub.py)
must decode ARM64 instructions. They used to `import capstone` unconditionally,
which breaks on **Android/bionic (Termux)** environments where installing
capstone is awkward -- exactly the environment where you might need to adapt a
mod to a new game version.

So: use capstone when it is importable (fast, in-process), otherwise shell out to
`llvm-objdump` (or GNU `objdump`). The instruction BYTES always come from the ELF
file itself, not from the disassembler's text, so pattern derivation is
unaffected by which backend is active -- only the human-readable text differs
slightly.

Public API
----------
    backend_name() -> str
    disassemble(path, addr, count) -> list[Insn]     # addr = virtual address
    parse_elf_loads(data) -> [(vaddr, offset, filesz, flags)]
    vaddr_to_offset(segs, addr) -> int | None
    read_bytes(path, addr, n) -> bytes
    is_pc_relative(insn) -> bool
"""
from __future__ import annotations

import os
import re
import shutil
import struct
import subprocess
from dataclasses import dataclass


@dataclass
class Insn:
    address: int
    mnemonic: str
    op_str: str
    raw: bytes


# ----------------------------------------------------------------- ELF ----
def parse_elf_loads(data: bytes):
    """-> [(p_vaddr, p_offset, p_filesz, p_flags)] for every PT_LOAD."""
    if data[:4] != b"\x7fELF":
        raise ValueError("not an ELF file")
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if struct.unpack_from("<I", data, off)[0] != 1:      # PT_LOAD
            continue
        p_flags = struct.unpack_from("<I", data, off + 0x04)[0]
        p_offset = struct.unpack_from("<Q", data, off + 0x08)[0]
        p_vaddr = struct.unpack_from("<Q", data, off + 0x10)[0]
        p_filesz = struct.unpack_from("<Q", data, off + 0x20)[0]
        segs.append((p_vaddr, p_offset, p_filesz, p_flags))
    return segs


def vaddr_to_offset(segs, addr):
    for vaddr, off, size, _flags in segs:
        if vaddr <= addr < vaddr + size:
            return off + (addr - vaddr)
    return None


_DATA_CACHE: dict = {}


def file_data(path: str) -> bytes:
    """Whole-file bytes, cached (a 300MB library is read once per process)."""
    d = _DATA_CACHE.get(path)
    if d is None:
        with open(path, "rb") as f:
            d = f.read()
        _DATA_CACHE[path] = d
    return d


def read_bytes(path: str, addr: int, n: int, data: bytes | None = None) -> bytes:
    """Read n bytes starting at VIRTUAL address addr (b'' if unmapped)."""
    if data is None:
        data = file_data(path)
    segs = parse_elf_loads(data)
    off = vaddr_to_offset(segs, addr)
    if off is None:
        return b""
    return data[off:off + n]


# ------------------------------------------------------------- backends ----
def _capstone_module():
    try:
        import capstone  # noqa: F401
        return capstone
    except Exception:
        return None


def _objdump_tool():
    """First external disassembler that exists, or None."""
    for name in ("llvm-objdump", "llvm-objdump-17", "llvm-objdump-18",
                 "llvm-objdump-16", "objdump", "aarch64-linux-gnu-objdump"):
        p = shutil.which(name)
        if p:
            return p
    return None


def backend_name() -> str:
    cs = _capstone_module()
    if cs is not None:
        return f"capstone {getattr(cs, '__version__', '?')}"
    tool = _objdump_tool()
    if tool:
        return f"external: {os.path.basename(tool)} (capstone not installed)"
    return "NONE (install capstone, or llvm-objdump/binutils)"


def _disasm_capstone(path, addr, count, cs):
    md = cs.Cs(cs.CS_ARCH_ARM64, cs.CS_MODE_LITTLE_ENDIAN)
    raw = read_bytes(path, addr, count * 4)
    out = []
    for insn in md.disasm(raw, addr):
        off = insn.address - addr
        out.append(Insn(insn.address, insn.mnemonic, insn.op_str,
                        raw[off:off + 4]))
    return out


_LINE = re.compile(r"^\s*([0-9a-fA-F]+):\s+(?:([0-9a-fA-F]{2}(?:\s+[0-9a-fA-F]{2})*)\s+)?(\S+)\s*(.*)$")


def _disasm_objdump(path, addr, count, tool):
    stop = addr + count * 4
    args = [tool, "-d", "--no-show-raw-insn",
            f"--start-address={addr:#x}", f"--stop-address={stop:#x}", path]
    proc = subprocess.run(args, capture_output=True, text=True)
    if proc.returncode != 0 or not proc.stdout.strip():
        # some builds dislike --start-address in this position
        args = [tool, "-d", f"--start-address={addr:#x}",
                f"--stop-address={stop:#x}", path]
        proc = subprocess.run(args, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"{tool} failed: {proc.stderr.strip()[:200]}")

    data = file_data(path)
    out = []
    for line in proc.stdout.splitlines():
        m = _LINE.match(line)
        if not m:
            continue
        a = int(m.group(1), 16)
        if not (addr <= a < stop):
            continue
        mnem = m.group(3)
        ops = m.group(4).strip()
        # strip objdump's symbol annotations, e.g. "bl 0x1234 <foo+0x10>"
        ops = re.sub(r"\s*<[^>]*>", "", ops)
        if mnem.startswith(".") or mnem in ("...", "word"):
            continue
        raw = read_bytes(path, a, 4, data)
        out.append(Insn(a, mnem, ops, raw))
    # objdump prints instructions in order; make sure we did not lose any
    return out


def disassemble(path: str, addr: int, count: int = 24):
    """Decode `count` instructions at virtual address `addr`."""
    cs = _capstone_module()
    if cs is not None:
        insns = _disasm_capstone(path, addr, count, cs)
        if insns:
            return insns
    tool = _objdump_tool()
    if not tool:
        raise RuntimeError(
            "no disassembly backend: install capstone "
            "(`pkg install python-capstone` on Termux, `pip install capstone` "
            "elsewhere) or install llvm/binutils for llvm-objdump/objdump")
    return _disasm_objdump(path, addr, count, tool)


# ----------------------------------------------------------- PC-relative ---
COND_BRANCHES = {
    "b.eq", "b.ne", "b.cs", "b.hs", "b.cc", "b.lo", "b.mi", "b.pl",
    "b.vs", "b.vc", "b.hi", "b.ls", "b.ge", "b.lt", "b.gt", "b.le", "b.al",
}


def is_pc_relative(insn) -> bool:
    """True if the instruction's encoding depends on its own address.

    Those are exactly the bytes that legitimately change between game builds,
    so they must be wildcarded in a cross-version pattern.
    """
    m = (insn.mnemonic or "").lower()
    if m in ("adrp", "adr", "b", "bl", "cbz", "cbnz", "tbz", "tbnz"):
        return True
    if m in COND_BRANCHES or m.startswith("b."):
        return True
    if m in ("ldr", "ldrsw", "prfm"):
        # the LITERAL form has no base register, i.e. no "["
        return "[" not in insn.op_str
    return False
