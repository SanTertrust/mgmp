# Enumerate every TLS-resident RNG stream in the binary.
#
# Run headless -- no MCP server involved, and it cannot wedge one:
#   $env:MGMP_OUT = "tls_streams.txt"
#   copy Mewgenics.exe.i64 work.i64        # batch mode writes to what it opens
#   & "C:\Program Files\IDA Professional 9.4\idat.exe" -A `
#       -S"tools\ida\enum_tls_streams.py" -L"ida.log" work.i64
# Note: IDA 9.x is unified -- the binary is idat.exe, not idat64.exe.
#
# Run C showed the battle draws going to TLS+0x198, not the TLS+0x178 that
# CLAUDE.md documents as "the global stream". Those are 32 bytes apart -- exactly
# one xoshiro256 state -- so the TLS block holds several streams, and only one of
# them was ever being watched.
#
# Method: for every call to the four RNG entry points, walk back a bounded number
# of instructions looking for the `mov rax, gs:58h` / `add rdx, [rax]` idiom and
# whatever immediate was folded into rdx. That immediate is the stream's offset
# in the TLS block.
import os
import ida_funcs, idc, idautils

OUT = os.environ.get("MGMP_OUT", "tls_streams.txt")
lines = []
def p(s=""):
    lines.append(str(s))

RNG_FUNCS = {
    0x14094B0B0: "randint",
    0x140158B80: "randfloat",
    0x14094B230: "rand2",
    0x14094B600: "rand_disc",
    0x14094ABB0: "splitmix64_seed",
}

def back(ea, count):
    out, cur = [], ea
    for _ in range(count):
        prev = idc.prev_head(cur)
        if prev == idc.BADADDR or prev >= cur:
            break
        out.append(prev)
        cur = prev
    return out          # nearest-first

# offset -> {func_name: count}, plus example sites
streams = {}
gs_sites = 0
total_calls = 0
no_tls = 0

for fea, fname in RNG_FUNCS.items():
    for x in idautils.XrefsTo(fea):
        if idc.print_insn_mnem(x.frm) != "call":
            continue
        total_calls += 1
        window = back(x.frm, 14)
        saw_gs = any("gs:58h" in idc.generate_disasm_line(a, 0) for a in window)
        if not saw_gs:
            no_tls += 1
            continue
        gs_sites += 1

        # Find the immediate loaded into rdx/edx in the same window.
        off = None
        for a in window:
            m = idc.print_insn_mnem(a)
            if m != "mov":
                continue
            dst = idc.print_operand(a, 0).lower()
            if dst not in ("rdx", "edx"):
                continue
            if idc.get_operand_type(a, 1) == idc.o_imm:
                off = idc.get_operand_value(a, 1)
                break
        if off is None:
            off = "computed"   # offset came from a register, e.g. `add rdx, rdi`

        d = streams.setdefault(off, {"count": 0, "fns": {}, "sites": []})
        d["count"] += 1
        d["fns"][fname] = d["fns"].get(fname, 0) + 1
        if len(d["sites"]) < 12:
            cf = ida_funcs.get_func(x.frm)
            d["sites"].append((x.frm, ida_funcs.get_func_name(cf.start_ea) if cf else "?"))

p("=== TLS-resident RNG streams, by offset in the TLS block ===")
p(f"(scanned {total_calls} calls to the RNG API; {gs_sites} load TLS, "
  f"{no_tls} pass a non-TLS state)")
p()

def sortkey(k):
    return (1, 0) if isinstance(k, str) else (0, k)

for off in sorted(streams, key=sortkey):
    d = streams[off]
    label = off if isinstance(off, str) else f"0x{off:X} ({off})"
    p(f"--- TLS + {label}   {d['count']} call sites")
    p(f"    fns: " + ", ".join(f"{k}x{v}" for k, v in sorted(d["fns"].items())))
    for ea, fn in d["sites"]:
        p(f"      {hex(ea)}  {fn}")
    p()

with open(OUT, "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))

idc.qexit(0)
