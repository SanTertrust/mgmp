"""Pull structure out of a phase-1 trace.

Two questions this answers:
  1. What is in a TurnAction? (field-by-field, across every observed action)
  2. Why does CHOICE fire so much, and does it ever return a real decision?
"""
import re
import sys
import struct
from collections import Counter, defaultdict

LINE = re.compile(r"^(\d{6}) (\d{4}) (\S+)\s+(.*)$")
HEX  = re.compile(r"\[([0-9A-F ]+)\]")
CLS  = re.compile(r"cls=(\S+)")


def blob(line):
    m = HEX.search(line)
    if not m:
        return None
    return bytes(int(b, 16) for b in m.group(1).split())


def u32(b, off):
    return struct.unpack_from("<I", b, off)[0] if off + 4 <= len(b) else None


def i32(b, off):
    return struct.unpack_from("<i", b, off)[0] if off + 4 <= len(b) else None


def u64(b, off):
    return struct.unpack_from("<Q", b, off)[0] if off + 8 <= len(b) else None


def main(path):
    rows = []
    with open(path, "r", errors="replace") as f:
        for raw in f:
            m = LINE.match(raw.rstrip("\n"))
            if m:
                rows.append((int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)))

    tags = Counter(r[2] for r in rows)
    print("tag histogram:", dict(tags))

    # --- who calls CHOICE, and does it ever decide anything? ---------------
    print("\n=== CHOICE ===")
    brains = Counter()
    results = Counter()
    for _, _, tag, rest in rows:
        if tag != "CHOICE":
            continue
        if rest.startswith("enter"):
            c = CLS.search(rest)
            brains[c.group(1) if c else "?"] += 1
        else:
            b = blob(rest)
            if b:
                results[u32(b, 0)] += 1
    print("callers   :", dict(brains))
    print("result[0] :", dict(results), " <- TurnAction type field of the return")

    # --- TurnAction field survey across real actions -----------------------
    print("\n=== TurnAction, from DOACTION/TRIGGER ===")
    per_off = defaultdict(list)
    actions = []
    for _, turn, tag, rest in rows:
        if tag not in ("DOACTION", "TRIGGER"):
            continue
        b = blob(rest)
        if not b:
            continue
        cls = CLS.search(rest)
        actions.append((turn, tag, cls.group(1) if cls else "?", rest, b))
        for off in range(0, min(len(b), 64), 4):
            per_off[off].append(u32(b, off))

    print(f"{len(actions)} actions\n")
    print(" off | distinct | sample values")
    print("-----+----------+" + "-" * 60)
    for off in sorted(per_off):
        vals = per_off[off]
        distinct = sorted(set(vals))
        show = ", ".join(f"0x{v:X}" for v in distinct[:6])
        if len(distinct) > 6:
            show += ", ..."
        print(f" +{off:02X} | {len(distinct):8} | {show}")

    print("\n=== decoded, assuming the layout the values imply ===")
    print("turn tag       class                        type ability            "
          "target   dir      actor")
    for turn, tag, cls, rest, b in actions:
        print(f"{turn:4} {tag:9} {cls:28} {u32(b,0):4} 0x{u64(b,8):012X} "
              f"({i32(b,16)},{i32(b,20)}) ({i32(b,24)},{i32(b,28)}) 0x{u64(b,32):012X}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "build/mgmp_trace.log")
