#!/usr/bin/env python3
"""Quick side-by-side triage for a paired host/client mgmp trace log desync.

Usage:
    python tools/diff_desync.py <host_trace.log> <client_trace.log>
"""

import re
import sys
from pathlib import Path

KEY_TAGS = (
    "HALT",
    "MISMATCH",
    "STATE DIFF",
    "CRASH",
    "EXCEPTION",
    "NODEHASH",
    "CATSYNC",
    "INVSYNC",
    "FOLLOW",
    "CHOICE",
)

TURN_HASH_RE = re.compile(
    r"LOCKSTEP\s+turn\s+(\d+)\s+hash\s+rng=([0-9a-f]+)\s+state=([0-9a-f]+)\s+queue=(\d+)\s+chars=(\d+/\d+)\s+battle=([0-9a-f]+)"
)
HALT_RE = re.compile(r"LOCKSTEP\s+!!\s+HALT\s+at\s+turn\s+(\d+):\s+(.*)")


def load_lines(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def find_halt(lines: list[str]) -> tuple[int, str] | None:
    for idx, line in enumerate(lines):
        m = HALT_RE.search(line)
        if m:
            return idx, m.group(2)
    return None


def find_turn_hashes(lines: list[str]) -> list[tuple[int, str, int, str, str, str, str]]:
    out = []
    for idx, line in enumerate(lines):
        m = TURN_HASH_RE.search(line)
        if m:
            out.append((idx, m.group(6), int(m.group(1)), m.group(2), m.group(3), m.group(4), m.group(5)))
    return out


def dump_window(label: str, lines: list[str], center_idx: int, before: int = 35, after: int = 12) -> None:
    lo = max(0, center_idx - before)
    hi = min(len(lines), center_idx + after)
    print(f"\n=== {label} (lines {lo + 1}..{hi}) ===")
    for i in range(lo, hi):
        print(f"{i + 1:5d}: {lines[i]}")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: python tools/diff_desync.py <host_trace.log> <client_trace.log>", file=sys.stderr)
        return 2

    host_path = Path(sys.argv[1])
    client_path = Path(sys.argv[2])
    h_lines = load_lines(host_path)
    c_lines = load_lines(client_path)

    print(f"HOST:   {host_path} ({len(h_lines)} lines)")
    print(f"CLIENT: {client_path} ({len(c_lines)} lines)")

    # 1. Compare turn hashes in order to pinpoint the first diverging (battle, turn).
    h_hashes = find_turn_hashes(h_lines)
    c_hashes = find_turn_hashes(c_lines)
    first_div = None
    for hh, ch in zip(h_hashes, c_hashes):
        if hh[1:] != ch[1:]:
            first_div = (hh, ch)
            break

    if first_div:
        hh, ch = first_div
        h_idx, h_bat, h_turn, h_rng, h_state, h_q, h_chars = hh
        c_idx, c_bat, c_turn, c_rng, c_state, c_q, c_chars = ch
        diffs = []
        if h_bat != c_bat:
            diffs.append(f"battle {h_bat} vs {c_bat}")
        if h_turn != c_turn:
            diffs.append(f"turn {h_turn} vs {c_turn}")
        if h_rng != c_rng:
            diffs.append(f"rng {h_rng} vs {c_rng}")
        if h_state != c_state:
            diffs.append(f"state {h_state} vs {c_state}")
        if h_q != c_q:
            diffs.append(f"queue {h_q} vs {c_q}")
        if h_chars != c_chars:
            diffs.append(f"chars {h_chars} vs {c_chars}")
        print(f"\nFIRST DIVERGING TURN HASH: battle={h_bat} turn={h_turn} -> {', '.join(diffs)}")
        dump_window("HOST around divergence", h_lines, h_idx)
        dump_window("CLIENT around divergence", c_lines, c_idx)
        return 0

    # 2. Otherwise check for explicit HALT or crash / NODEHASH mismatch.
    h_halt = find_halt(h_lines)
    c_halt = find_halt(c_lines)
    if h_halt or c_halt:
        print(f"\nHALT: host={h_halt[1] if h_halt else 'none'} | client={c_halt[1] if c_halt else 'none'}")
        if h_halt:
            dump_window("HOST around HALT", h_lines, h_halt[0])
        if c_halt:
            dump_window("CLIENT around HALT", c_lines, c_halt[0])
        return 0

    # 3. Fallback: show mismatches/crashes and tail of both logs.
    for label, lines in (("HOST", h_lines), ("CLIENT", c_lines)):
        hits = [(i, l) for i, l in enumerate(lines) if any(k in l for k in ("MISMATCH", "CRASH", "EXCEPTION", "!!"))]
        print(f"\n=== {label} warnings/errors ({len(hits)} total, last 15) ===")
        for i, l in hits[-15:]:
            print(f"{i + 1:5d}: {l}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
