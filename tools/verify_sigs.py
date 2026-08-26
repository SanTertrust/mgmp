# verify_sigs.py -- independent check of sigs_generated.h against the SHIPPED PE.
#
# The generator ran inside IDA. CLAUDE.md's rule is that a call/hook target is
# verified against Mewgenics.exe itself, not against IDA's notes, because the
# two can disagree. This reads the PE directly and asserts, for every pattern:
#   - it matches at the rva it claims
#   - it matches EXACTLY ONCE in the executable section
# and for data patterns, that decoding the displacement recovers the claimed rva.
import re, struct, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
HDR  = os.path.join(HERE, "..", "src", "core", "mgmp_sigs.generated.h")

# The game executable is not in this repo. Point at your own copy, in order of
# preference: argv[1], the MEWGENICS_EXE environment variable, or a
# Mewgenics.exe sitting one level above the mod directory (the layout this was
# developed in).
EXE = (sys.argv[1] if len(sys.argv) > 1 else
       os.environ.get("MEWGENICS_EXE") or
       os.path.join(HERE, "..", "..", "Mewgenics.exe"))

if not os.path.isfile(EXE):
    sys.exit("Mewgenics.exe not found at %r\n"
             "  pass it as an argument, or set MEWGENICS_EXE" % EXE)

IMAGEBASE = 0x140000000

data = open(EXE, "rb").read()
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
nsec  = struct.unpack_from("<H", data, e_lfanew + 6)[0]
optsz = struct.unpack_from("<H", data, e_lfanew + 20)[0]
sect  = e_lfanew + 24 + optsz

text = None
for i in range(nsec):
    o = sect + 40 * i
    name = data[o:o+8].rstrip(b"\0").decode()
    vsz, va, rawsz, raw = struct.unpack_from("<IIII", data, o + 8)
    chars = struct.unpack_from("<I", data, o + 36)[0]
    if chars & 0x20000000:            # IMAGE_SCN_MEM_EXECUTE
        text = (va, raw, min(vsz, rawsz), name)
        break
va, raw, size, tname = text
blob = data[raw:raw + size]
print(f"section {tname}: rva 0x{va:X}, {size} bytes")

def to_regex(pat):
    out = b""
    for tok in pat.split():
        out += b"." if tok == "?" else re.escape(bytes([int(tok, 16)]))
    return out

src = open(HDR, "r", encoding="utf-8").read()

code = re.findall(r'\{\s*0x([0-9A-Fa-f]{8})\s*,\s*"([^"]+)"\s*,\s*"([0-9A-Fa-f? ]+)"\s*\}', src)
dat  = re.findall(r'\{\s*0x([0-9A-Fa-f]{8})\s*,\s*"([^"]+)"\s*,\s*"([0-9A-Fa-f? ]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', src)

fails = 0
print(f"\n--- {len(code)} code patterns")
for rva_s, name, pat in code:
    rva = int(rva_s, 16)
    hits = [m.start() for m in re.finditer(to_regex(pat), blob, re.DOTALL)]
    want = rva - va
    okpos = (want in hits)
    uniq  = (len(hits) == 1)
    if not (okpos and uniq):
        fails += 1
        print(f"  FAIL {name:32} rva 0x{rva:08X}  hits={len(hits)} at_rva={okpos}")
print(f"  {len(code)-fails} / {len(code)} code patterns unique and correctly placed")

print(f"\n--- {len(dat)} data patterns")
dfails = 0
for rva_s, name, pat, disp_off, insn_end in dat:
    rva = int(rva_s, 16); disp_off = int(disp_off); insn_end = int(insn_end)
    hits = [m.start() for m in re.finditer(to_regex(pat), blob, re.DOTALL)]
    if len(hits) != 1:
        dfails += 1
        print(f"  FAIL {name:20} referencing code hits={len(hits)}")
        continue
    off = hits[0]
    disp = struct.unpack_from("<i", blob, off + disp_off)[0]
    recovered = va + off + insn_end + disp          # rva arithmetic
    mark = "ok" if recovered == rva else "MISMATCH"
    if recovered != rva: dfails += 1
    print(f"  {mark:8} {name:20} claimed 0x{rva:08X}  recovered 0x{recovered:08X}")

total = fails + dfails
print(f"\n{'ALL PATTERNS VERIFIED' if total == 0 else str(total) + ' FAILURE(S)'}")
sys.exit(1 if total else 0)
