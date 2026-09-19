#!/usr/bin/env python3
"""Verify BetterZoom's signatures (read from src/Signatures.hpp) against a build.

Also predicts the runtime hide-HUD choice by applying the same anchor rule the
mod uses, so a version port can be validated without a device.

Usage:
    python3 tools/verify_signatures.py /path/to/libminecraftpe.so
"""
import argparse
import mmap
import re
import sys
from pathlib import Path


def parse_pattern(sig):
    values, masks = bytearray(), bytearray()
    for tok in sig.split():
        if tok in ("?", "??"):
            values.append(0)
            masks.append(0)
            continue
        if len(tok) % 2:
            raise ValueError(f"bad token {tok!r}")
        for i in range(0, len(tok), 2):
            pair = tok[i:i + 2]
            if pair == "??":
                values.append(0)
                masks.append(0)
                continue
            value = mask = 0
            for j, ch in enumerate(pair):
                if ch == "?":
                    continue
                mask |= 0xF << ((1 - j) * 4)
                value |= int(ch, 16) << ((1 - j) * 4)
            values.append(value)
            masks.append(mask)
    return bytes(values), bytes(masks)


def find_hits(data, sig):
    values, masks = parse_pattern(sig)
    n = len(values)
    if n == 0:
        return []
    best_len = best_start = 0
    i = 0
    while i < n:
        if masks[i] == 0xFF:
            j = i
            while j < n and masks[j] == 0xFF:
                j += 1
            if j - i > best_len:
                best_len, best_start = j - i, i
            i = j
        else:
            i += 1
    needle = values[best_start:best_start + best_len]
    hits = []
    if best_len >= 4:
        pos = 0
        while True:
            pos = data.find(needle, pos)
            if pos < 0:
                break
            base = pos - best_start
            if base >= 0 and base + n <= len(data) and all(
                    not masks[k] or data[base + k] == values[k] for k in range(n)):
                hits.append(base)
            pos += 1
    else:
        for base in range(0, len(data) - n):
            if all(not masks[k] or data[base + k] == values[k] for k in range(n)):
                hits.append(base)
    return hits


def load_header(root):
    text = (root / "src" / "Signatures.hpp").read_text(encoding="utf-8")
    pats = {}
    for m in re.finditer(r'k(\w+)\s*=\s*((?:"[^"]*"\s*)+);', text):
        pats[m.group(1)] = "".join(re.findall(r'"([^"]*)"', m.group(2)))
    dist = re.search(r'kHideHudToHideHandDistance\s*=\s*(0x[0-9a-fA-F]+)', text)
    return pats, int(dist.group(1), 16) if dist else 0x70


def status(hits):
    if not hits:
        return "MISSING"
    if len(hits) == 1:
        return "UNIQUE"
    return f"AMBIG({len(hits)})"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("library")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    args = ap.parse_args()

    pats, dist = load_header(Path(args.root))
    print(f"patterns in Signatures.hpp: {len(pats)}  "
          f"(hideHud->hideHand distance = 0x{dist:x})\n")

    required = ["GetFovPattern", "ApplyTurnDeltaPattern", "GetHideItemInHandPattern"]
    optional = ["GetHideHudPatternOption45", "GetHideHudPatternOption48"]

    ok = True
    with open(args.library, "rb") as f:
        data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        for name in required:
            if name not in pats:
                print(f"  MISSING-FROM-SOURCE {name}")
                ok = False
                continue
            hits = find_hits(data, pats[name])
            st = status(hits)
            if st != "UNIQUE":
                ok = False
            print(f"  {st:9s} {name:26s} " + " ".join(f"0x{h:x}" for h in hits[:3]))

        # hide-HUD: reproduce the mod's selection rule
        hand = find_hits(data, pats["GetHideItemInHandPattern"])
        anchor = hand[0] if len(hand) == 1 else 0
        chosen, chosen_name = 0, None
        print()
        for name in optional:
            hits = find_hits(data, pats[name])
            st = status(hits)
            note = ""
            if len(hits) == 1:
                if anchor:
                    delta = anchor - hits[0]
                    if delta == dist:
                        note = f"  <- ACCEPTED (anchor - 0x{dist:x})"
                        if chosen == 0:
                            chosen, chosen_name = hits[0], name
                    else:
                        note = f"  (rejected: anchor delta 0x{delta:x})"
                else:
                    note = "  (no anchor; would be accepted)"
                    if chosen == 0:
                        chosen, chosen_name = hits[0], name
            print(f"  {st:9s} {name:26s} " + " ".join(f"0x{h:x}" for h in hits[:3]) + note)

        print()
        if chosen:
            print(f"  runtime hide-HUD choice: {chosen_name} @0x{chosen:x}")
        else:
            print("  runtime hide-HUD choice: none "
                  "(hook skipped; F1 HUD toggle still works)")

    print()
    if ok:
        print("Required signatures OK -> safe to ship on this build.")
        return 0
    print("NOT SAFE: a required signature is missing or ambiguous on this build.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
