# gen_sigs.py -- derive a UNIQUE, wildcarded byte signature for every hook
# target and call target in mgmp_addresses.h.
#
# Run inside IDA (MCP run_script, or headless idat -A -S). It reads the RVAs
# straight out of mgmp_addresses.h so the two can never drift apart, and writes
#   mod/src/core/mgmp_sigs.generated.h   the C++ table
#   mod/tools/ida/sigs_report.txt     what happened, per target
#
# THE MIGRATION PATH: after a game update, open the NEW .i64 and run this again.
# Targets whose code is unchanged regenerate identically; targets that moved get
# a new rva_hint; targets that genuinely changed shape are reported and are
# exactly the list a human has to look at.
#
# WHAT GETS WILDCARDED
#   call/jmp rel32          -- shifts whenever anything before it changes size
#   RIP-relative disp32     -- every string/global/vftable reference
#   imm32/imm64 in-image    -- hardcoded addresses
# Struct displacements ([rcx+0E8h]) are KEPT on purpose: they are semantic, they
# carry most of the uniqueness, and if one moves we want a loud break.
#
# THE PATTERN IS GROWN ONE INSTRUCTION AT A TIME AND STOPS AT THE FIRST LENGTH
# THAT IS UNIQUE. Shorter is less fragile across a rebuild, so minimal-unique is
# the right target, not maximal-distinctive.

import re, os, sys
import ida_bytes, ida_ua, ida_funcs, ida_segment, idc, idaapi

IMAGEBASE = 0x140000000


def _find_root():
    """The mod directory -- the one holding src/, tools/ and CMakeLists.txt.

    Three ways, because how this script gets run varies. Under headless idat
    -S and under most run_script paths __file__ exists and is exact. Under an
    exec() that does not set it, fall back to the .i64's own directory, since
    the database lives beside the mod directory in the developed layout. Set
    MGMP_ROOT to override either.
    """
    env = os.environ.get("MGMP_ROOT")
    if env:
        return os.path.abspath(env)

    here = globals().get("__file__")
    if here:                                   # <root>/tools/ida/gen_sigs.py
        return os.path.abspath(os.path.join(os.path.dirname(here), "..", ".."))

    idb = idc.get_idb_path()                   # <parent>/Mewgenics.exe.i64
    if idb:
        return os.path.join(os.path.dirname(os.path.abspath(idb)), "mod")

    raise RuntimeError("cannot locate the mod directory -- set MGMP_ROOT")


ROOT      = _find_root()
HEADER    = os.path.join(ROOT, "src", "core", "mgmp_addresses.h")
OUT_H     = os.path.join(ROOT, "src", "core", "mgmp_sigs.generated.h")
OUT_TXT   = os.path.join(ROOT, "tools", "ida", "sigs_report.txt")

MAX_BYTES = 160          # refuse to grow past this; something is wrong if we do
MIN_BYTES = 12           # never emit anything shorter, even if it looks unique
SEED_MAX  = 8            # literal bytes used to seed the candidate set


# ---------------------------------------------------------------- the haystack

def text_section():
    for i in range(ida_segment.get_segm_qty()):
        s = ida_segment.getnseg(i)
        if s and (s.perm & ida_segment.SEGPERM_EXEC):
            n = ida_segment.get_segm_name(s)
            if n in (".text", "CODE"):
                data = ida_bytes.get_bytes(s.start_ea, s.end_ea - s.start_ea)
                return s.start_ea, data
    raise RuntimeError("no executable segment found")


# ------------------------------------------------------------------ the parser

def operand_masks(insn, ln):
    """byte offsets within the instruction that must become wildcards."""
    out = set()
    img_lo, img_hi = IMAGEBASE, IMAGEBASE + 0x156B000

    for op in insn.ops:
        if op.type == ida_ua.o_void:
            break
        off = op.offb
        if off <= 0 or off >= ln:
            continue
        room = ln - off

        # branch targets: rel32 moves on any rebuild, rel8 is intra-function
        if op.type in (ida_ua.o_near, ida_ua.o_far):
            if room >= 4:
                out.update(range(off, off + 4))
            continue

        # RIP-relative data references decode to an absolute address in .addr
        if op.type in (ida_ua.o_mem, ida_ua.o_displ):
            if img_lo <= op.addr < img_hi and room >= 4:
                out.update(range(off, off + 4))
            continue

        # a hardcoded address as an immediate
        if op.type == ida_ua.o_imm:
            if img_lo <= op.value < img_hi:
                width = 8 if room >= 8 and op.value > 0xFFFFFFFF else 4
                if room >= width:
                    out.update(range(off, off + width))
            continue

    return out


def instruction_pattern(ea):
    """(bytes, mask, length) for one instruction, or None."""
    insn = ida_ua.insn_t()
    ln = ida_ua.decode_insn(insn, ea)
    if ln <= 0:
        return None
    raw = ida_bytes.get_bytes(ea, ln)
    if raw is None or len(raw) != ln:
        return None
    holes = operand_masks(insn, ln)
    mask = bytes(0 if i in holes else 1 for i in range(ln))
    return raw, mask, ln


# ----------------------------------------------------------------- the search

def seed_candidates(data, pat, msk):
    """offsets where the leading literal run matches -- a cheap prefilter."""
    run = 0
    while run < len(msk) and msk[run] and run < SEED_MAX:
        run += 1
    if run == 0:
        return None                      # pattern starts with a wildcard
    needle = pat[:run]
    out, i = [], data.find(needle)
    while i >= 0:
        out.append(i)
        i = data.find(needle, i + 1)
    return out


def matches_at(data, off, pat, msk):
    if off + len(pat) > len(data):
        return False
    for i in range(len(pat)):
        if msk[i] and data[off + i] != pat[i]:
            return False
    return True


def count_matches(data, cands, pat, msk):
    n, first = 0, -1
    for off in cands:
        if matches_at(data, off, pat, msk):
            if n == 0:
                first = off
            n += 1
            if n > 1:
                return n, first
    return n, first


def build_signature(ea, data, text_start):
    """grow instruction by instruction until exactly one match remains."""
    pat, msk, bounds = b"", b"", []
    cur = ea
    while len(pat) < MAX_BYTES:
        got = instruction_pattern(cur)
        if got is None:
            return None, "undecodable instruction at 0x%X" % cur
        raw, m, ln = got
        pat += raw
        msk += m
        cur += ln
        bounds.append(len(pat))

        if len(pat) < MIN_BYTES:
            continue
        if not msk[len(pat) - 1]:
            continue                       # never end on a wildcard

        cands = seed_candidates(data, pat, msk)
        if cands is None:
            return None, "pattern begins with a wildcard"
        n, first = count_matches(data, cands, pat, msk)
        if n == 1:
            if first + text_start != ea:
                return None, "unique match at 0x%X, expected 0x%X" % (
                    first + text_start, ea)
            return (pat, msk), None
        if n == 0:
            return None, "internal error: pattern does not match its own source"

    return None, "still ambiguous after %d bytes" % MAX_BYTES


def render(pat, msk):
    return " ".join("?" if not msk[i] else "%02X" % pat[i] for i in range(len(pat)))


# ------------------------------------------------- read the targets we must do

def _block(src, opener):
    """text between `opener` and the matching top-level `};`."""
    i = src.index(opener) + len(opener)
    depth, j = 1, i
    while depth:
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                break
        j += 1
    return src[i:j]


def read_targets():
    """The two tables, each IN ENUM ORDER -- the generated arrays are indexed
       by Target and Call, so order is load-bearing, not cosmetic."""
    src = open(HEADER, "r", encoding="utf-8", errors="replace").read()

    def entries(block):
        return [(int(m.group(1), 16), m.group(2))
                for m in re.finditer(r'\{\s*0x([0-9A-Fa-f]{8})\s*,\s*"([^"]+)"', block)]

    targets = entries(_block(src, "static const TargetDesc kTargets[T_COUNT] = {"))
    calls   = entries(_block(src, "static const CallDesc kCalls[C_COUNT] = {"))
    return targets, calls


# ----------------------------------------------------- DATA: resolve by xref
#
# A global cannot be scanned for -- its bytes are runtime state. It is
# recovered from the code that references it, by decoding that reference's own
# RIP-relative displacement. Same rule CLAUDE.md already imposes on the SDL
# slots ("decode the RVA from the thunk's own jmp operand, never by counting"),
# generalised to every datum once the build is allowed to move.

import idautils

def build_data_signature(data_ea, data_rva, name):
    text_start, data = text_section()

    for ref in idautils.DataRefsTo(data_ea):
        f = ida_funcs.get_func(ref)
        if not f:
            continue

        insn = ida_ua.insn_t()
        ln = ida_ua.decode_insn(insn, ref)
        if ln <= 0:
            continue

        # which operand carries the displacement that names this datum
        off = None
        for op in insn.ops:
            if op.type == ida_ua.o_void:
                break
            if op.addr == data_ea and op.offb > 0 and ln - op.offb >= 4:
                off = op.offb
                break
        if off is None:
            continue

        disp_off     = (ref - f.start_ea) + off
        insn_end_off = (ref - f.start_ea) + ln

        # grow from the FUNCTION START so the match address is well defined,
        # and keep growing until the pattern both covers the reference and is
        # unique.
        pat, msk, cur = b"", b"", f.start_ea
        while len(pat) < MAX_BYTES:
            got = instruction_pattern(cur)
            if got is None:
                break
            raw, m, l = got
            pat += raw
            msk += m
            cur += l
            if len(pat) < max(MIN_BYTES, insn_end_off):
                continue
            if not msk[len(pat) - 1]:
                continue
            cands = seed_candidates(data, pat, msk)
            if cands is None:
                break
            n, first = count_matches(data, cands, pat, msk)
            if n == 1 and first + text_start == f.start_ea:
                return {
                    "rva": data_rva, "name": name,
                    "pattern": render(pat, msk),
                    "disp_off": disp_off, "insn_end_off": insn_end_off,
                    "via": "0x%X in %s" % (ref, idc.get_func_name(f.start_ea) or "?"),
                    "bytes": len(pat),
                }
            if n == 0:
                break
    return None


DATA_TARGETS = [
    (0x013D1970, "MewDirectorPtr"),
    (0x012F2E80, "MouseCache"),
    (0x013BB790, "ApplicationBase"),
    # The three SDL_DYNAPI slots are DELIBERATELY not here. Every thunk in that
    # table is `jmp cs:off_...` and every default stub shares one shape, so no
    # byte pattern can tell one slot from its neighbour -- which is the exact
    # trap CLAUDE.md records ("resolving by COUNTING table entries yielded a
    # stub belonging to a neighbouring function, and the pinned-build check
    # passed because every stub has the same prologue").
    #
    # They need the third mechanism instead: locate the real implementation by
    # a unique code pattern (SDL_GL_SwapWindow's own error strings identify it),
    # then find the one qword in .data holding that address. That is sound
    # because the mod injects at FrameBegin, long after SDL_InitDynamicAPI has
    # overwritten the table with real implementations.
]


# ------------------------------------------------------------------- the drive

def main():
    text_start, data = text_section()
    print("text %08X..%08X  (%d bytes)" % (text_start, text_start + len(data), len(data)))

    targets, calls = read_targets()
    print("kTargets: %d entries, kCalls: %d entries" % (len(targets), len(calls)))

    report = []
    ok = bad = 0
    cache = {}

    def do(rva, name):
        """returns the pattern text, or None -- memoised, since the two tables
           overlap and a pattern is a pure function of the address."""
        nonlocal ok, bad
        if rva in cache:
            return cache[rva]
        ea = IMAGEBASE + rva
        seg = ida_segment.getseg(ea)
        segname = ida_segment.get_segm_name(seg) if seg else "?"
        if not seg or not (seg.perm & ida_segment.SEGPERM_EXEC):
            report.append("SKIP  %-34s rva 0x%08X  not code (%s)" % (name, rva, segname))
            cache[rva] = None
            return None

        f = ida_funcs.get_func(ea)
        note = "" if (f and f.start_ea == ea) else "  [not a function start]"

        sig, err = build_signature(ea, data, text_start)
        if sig is None:
            bad += 1
            report.append("FAIL  %-34s rva 0x%08X  %s%s" % (name, rva, err, note))
            cache[rva] = None
            return None

        pat, msk = sig
        ok += 1
        report.append("OK    %-34s rva 0x%08X  %3d bytes, %3d literal, %2d wildcard%s"
                      % (name, rva, len(pat), sum(msk), len(pat) - sum(msk), note))
        cache[rva] = render(pat, msk)
        return cache[rva]

    trows = [(rva, name, do(rva, name)) for rva, name in targets]
    crows = [(rva, name, do(rva, name)) for rva, name in calls]

    # --- the data half
    drows = []
    for rva, name in DATA_TARGETS:
        got = build_data_signature(IMAGEBASE + rva, rva, name)
        if got is None:
            bad += 1
            report.append("FAIL  %-12s rva 0x%08X  no usable code reference" % (name, rva))
            continue
        ok += 1
        drows.append(got)
        report.append("OK    %-12s rva 0x%08X  DATA via %s  (%d bytes, disp@%d)"
                      % (name, rva, got["via"], got["bytes"], got["disp_off"]))

    with open(OUT_TXT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(report) + "\n\n%d ok, %d failed\n" % (ok, bad))

    with open(OUT_H, "w", encoding="utf-8") as fh:
        fh.write("// GENERATED by mod/tools/ida/gen_sigs.py -- do not hand-edit.\n")
        fh.write("// Each pattern is minimal-unique in .text of the build it was\n")
        fh.write("// generated from. rva is a CROSS-CHECK, not the answer.\n")
        fh.write("#pragma once\n#include \"mgmp_sigscan.h\"\n\n")
        fh.write("namespace mgmp {\n\n")

        def emit(varname, rows, comment):
            fh.write("// %s\n" % comment)
            fh.write("static const SigTargetDesc %s[] = {\n" % varname)
            for rva, name, text in rows:
                if text is None:
                    fh.write('    { 0x%08X, "%s", nullptr },   // NO SIGNATURE\n'
                             % (rva, name))
                else:
                    fh.write('    { 0x%08X, "%s",\n      "%s" },\n' % (rva, name, text))
            fh.write("};\n\n")

        emit("kTargetSigs", trows, "INDEXED BY enum Target -- order is load-bearing.")
        emit("kCallSigs",   crows, "INDEXED BY enum Call -- order is load-bearing.")
        fh.write("// Globals, recovered from a referencing instruction's own\n"
                 "// RIP-relative displacement rather than by address.\n")
        fh.write("static const SigDataDesc kSigData[] = {\n")
        for d in drows:
            fh.write('    { 0x%08X, "%s",\n      "%s",\n      %d, %d },  // via %s\n'
                     % (d["rva"], d["name"], d["pattern"],
                        d["disp_off"], d["insn_end_off"], d["via"]))
        fh.write("};\nstatic const int kSigDataCount = %d;\n\n} // namespace mgmp\n"
                 % len(drows))

    print("\n".join(report))
    print("\n%d ok, %d failed -> %s" % (ok, bad, OUT_H))


main()
idc.qexit(0) if "idat" in sys.executable.lower() else None
