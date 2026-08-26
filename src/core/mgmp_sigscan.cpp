// mgmp_sigscan.cpp -- see mgmp_sigscan.h for the design and the reasoning.
#include "mgmp_sigscan.h"

#include <windows.h>
#include <intrin.h>
#include <immintrin.h>

#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// --- CPU detection ---------------------------------------------------------
//
// AVX2 needs three separate yeses and skipping any one of them is a real bug:
// the CPU must have the instruction, the OS must have enabled XSAVE, and the OS
// must actually be saving the YMM half on a context switch. A machine can say
// yes to the first and no to the third -- old hypervisors did exactly that --
// and then YMM state is silently corrupted by any thread switch mid-scan.

bool detect_avx2() {
    int r[4] = {0, 0, 0, 0};
    __cpuid(r, 0);
    const int max_leaf = r[0];
    if (max_leaf < 7) return false;

    __cpuid(r, 1);
    const bool osxsave = (r[2] & (1 << 27)) != 0;   // leaf 1 ECX[27]
    const bool avx     = (r[2] & (1 << 28)) != 0;   // leaf 1 ECX[28]
    if (!osxsave || !avx) return false;

    // XCR0[1] = XMM saved, XCR0[2] = YMM saved. Both required.
    const unsigned long long xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6) != 0x6) return false;

    __cpuidex(r, 7, 0);
    return (r[1] & (1 << 5)) != 0;                  // leaf 7 EBX[5] = AVX2
}

SigCpu       g_cpu       = SIG_CPU_SCALAR;
bool         g_cpu_known = false;

// --- the .text byte histogram ----------------------------------------------

uint32_t g_hist[256];
bool     g_hist_ready = false;

} // namespace

SigCpu sigscan_cpu() {
    if (!g_cpu_known) {
        // SSE2 is architectural on x86-64, so the only question is AVX2.
        g_cpu       = detect_avx2() ? SIG_CPU_AVX2 : SIG_CPU_SSE2;
        g_cpu_known = true;
    }
    return g_cpu;
}

const char* sigscan_cpu_name() {
    switch (sigscan_cpu()) {
    case SIG_CPU_AVX2:   return "AVX2 (32 bytes/step)";
    case SIG_CPU_SSE2:   return "SSE2 (16 bytes/step)";
    default:             return "scalar";
    }
}

// --- parsing ---------------------------------------------------------------

bool sig_parse(const char* text, SigPattern* out, char* err, size_t err_size) {
    auto fail = [&](const char* msg) {
        if (err && err_size) _snprintf_s(err, err_size, _TRUNCATE, "%s", msg);
        return false;
    };
    if (!text || !out) return fail("null pattern");

    memset(out, 0, sizeof(*out));

    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    const char* p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;

        if (out->len >= kSigMaxBytes) return fail("pattern longer than kSigMaxBytes");

        if (*p == '?' || *p == 'x' || *p == 'X') {
            out->bytes[out->len] = 0;
            out->mask [out->len] = 0;
            ++out->len;
            ++p;
            if (*p == '?' || *p == 'x' || *p == 'X') ++p;   // accept "??"
            continue;
        }

        const int hi = hexval(p[0]);
        const int lo = p[1] ? hexval(p[1]) : -1;
        if (hi < 0 || lo < 0) return fail("expected two hex digits or '?'");
        out->bytes[out->len] = (uint8_t)((hi << 4) | lo);
        out->mask [out->len] = 1;
        ++out->literals;
        ++out->len;
        p += 2;
    }

    if (out->len == 0)          return fail("empty pattern");
    if (!out->mask[0])          return fail("pattern starts with a wildcard -- "
                                            "the match address would be ambiguous");
    if (!out->mask[out->len-1]) return fail("pattern ends with a wildcard -- "
                                            "it adds no uniqueness");
    if (out->literals < 8)      return fail("fewer than 8 literal bytes -- "
                                            "far too weak to be unique");

    sig_choose_anchor(out);
    return true;
}

// --- the region ------------------------------------------------------------

bool sigscan_text_section(uintptr_t module_base, SigRegion* out) {
    if (!module_base || !out) return false;

    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)module_base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS64* nt =
        (const IMAGE_NT_HEADERS64*)(module_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        out->base = (const uint8_t*)(module_base + sec[i].VirtualAddress);
        out->size = sec[i].Misc.VirtualSize;
        out->rva  = sec[i].VirtualAddress;
        return out->size > 0;
    }
    return false;
}

void sigscan_profile(const SigRegion& region) {
    memset(g_hist, 0, sizeof(g_hist));
    for (size_t i = 0; i < region.size; ++i) ++g_hist[region.base[i]];
    g_hist_ready = true;
}

void sig_choose_anchor(SigPattern* p) {
    if (!p || p->len <= 0) return;

    // Without a profile the best available guess is byte 0. With one, the
    // rarest literal byte -- which is the whole performance story: an anchor of
    // 0x48 (REX.W, on roughly every other instruction) produces orders of
    // magnitude more candidate positions to verify than a real opcode byte.
    int best = -1;
    uint32_t best_freq = 0xFFFFFFFFu;
    for (int i = 0; i < p->len; ++i) {
        if (!p->mask[i]) continue;
        const uint32_t f = g_hist_ready ? g_hist[p->bytes[i]] : 0;
        if (best < 0 || f < best_freq) { best = i; best_freq = f; }
        if (!g_hist_ready) break;
    }
    p->anchor = best < 0 ? 0 : best;
}

// --- matching --------------------------------------------------------------

bool sig_matches_at(const uint8_t* at, const SigPattern& p) {
    for (int i = 0; i < p.len; ++i)
        if (p.mask[i] && at[i] != p.bytes[i]) return false;
    return true;
}

namespace {

// Shared candidate check: `hit` is where the anchor byte was found.
inline void consider(const SigRegion& r, const SigPattern& p, size_t hit,
                     int* count, const uint8_t** first) {
    if (hit < (size_t)p.anchor) return;
    const size_t start = hit - (size_t)p.anchor;
    if (start + (size_t)p.len > r.size) return;
    if (!sig_matches_at(r.base + start, p)) return;
    if (*count == 0 && first) *first = r.base + start;
    ++*count;
}

int find_avx2(const SigRegion& r, const SigPattern& p, const uint8_t** first) {
    int count = 0;
    const __m256i needle = _mm256_set1_epi8((char)p.bytes[p.anchor]);
    size_t i = 0;
    for (; i + 32 <= r.size; i += 32) {
        const __m256i hay = _mm256_loadu_si256((const __m256i*)(r.base + i));
        uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hay, needle));
        while (m) {
            unsigned long bit;
            _BitScanForward(&bit, m);
            consider(r, p, i + bit, &count, first);
            m &= m - 1;
        }
    }
    for (; i < r.size; ++i)
        if (r.base[i] == p.bytes[p.anchor]) consider(r, p, i, &count, first);
    return count;
}

int find_sse2(const SigRegion& r, const SigPattern& p, const uint8_t** first) {
    int count = 0;
    const __m128i needle = _mm_set1_epi8((char)p.bytes[p.anchor]);
    size_t i = 0;
    for (; i + 16 <= r.size; i += 16) {
        const __m128i hay = _mm_loadu_si128((const __m128i*)(r.base + i));
        uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(hay, needle));
        while (m) {
            unsigned long bit;
            _BitScanForward(&bit, m);
            consider(r, p, i + bit, &count, first);
            m &= m - 1;
        }
    }
    for (; i < r.size; ++i)
        if (r.base[i] == p.bytes[p.anchor]) consider(r, p, i, &count, first);
    return count;
}

int find_scalar(const SigRegion& r, const SigPattern& p, const uint8_t** first) {
    int count = 0;
    for (size_t i = 0; i < r.size; ++i)
        if (r.base[i] == p.bytes[p.anchor]) consider(r, p, i, &count, first);
    return count;
}

} // namespace

int sig_find_all(const SigRegion& region, const SigPattern& p, const uint8_t** first) {
    if (first) *first = nullptr;
    if (!region.base || region.size < (size_t)p.len) return 0;

    switch (sigscan_cpu()) {
    case SIG_CPU_AVX2: return find_avx2(region, p, first);
    case SIG_CPU_SSE2: return find_sse2(region, p, first);
    default:           return find_scalar(region, p, first);
    }
}

// --- resolution ------------------------------------------------------------

SigResolution sig_resolve(uintptr_t module_base, const SigRegion& region,
                          const SigTargetDesc& target) {
    SigResolution res;
    memset(&res, 0, sizeof(res));

    SigPattern pat;
    char perr[96] = {0};
    if (!sig_parse(target.pattern, &pat, perr, sizeof(perr))) {
        _snprintf_s(res.why, sizeof(res.why), _TRUNCATE,
                    "malformed pattern: %s", perr);
        return res;
    }

    // 1. The pinned RVA. On the build this was authored against, this is the
    //    only branch that ever runs and no scanning happens at all.
    const uintptr_t hint = module_base + target.rva_hint;
    const uintptr_t lo   = (uintptr_t)region.base;
    const uintptr_t hi   = lo + region.size;
    if (hint >= lo && hint + (size_t)pat.len <= hi &&
        sig_matches_at((const uint8_t*)hint, pat)) {
        res.addr      = hint;
        res.rva       = target.rva_hint;
        res.drift     = 0;
        res.matches   = 1;
        res.from_hint = true;
        res.ok        = true;
        return res;
    }

    // 2. The hint failed, so this is not the pinned build (or that function
    //    moved). Search, and insist on exactly one answer.
    const uint8_t* first = nullptr;
    const int n = sig_find_all(region, pat, &first);
    res.matches = n;

    if (n == 0) {
        _snprintf_s(res.why, sizeof(res.why), _TRUNCATE,
                    "not found -- the function changed shape or was removed");
        return res;
    }
    if (n > 1) {
        // Never guess. MSVC ICF folds identical functions, so more than one
        // match is an expected outcome and not evidence of a broken pattern.
        _snprintf_s(res.why, sizeof(res.why), _TRUNCATE,
                    "AMBIGUOUS -- %d matches; refusing rather than picking one", n);
        return res;
    }

    res.addr      = (uintptr_t)first;
    res.rva       = (uint32_t)(res.addr - module_base);
    res.drift     = (int32_t)(res.rva - target.rva_hint);
    res.from_hint = false;
    res.ok        = true;
    _snprintf_s(res.why, sizeof(res.why), _TRUNCATE,
                "MOVED: rva 0x%08X -> 0x%08X (%+d)",
                target.rva_hint, res.rva, res.drift);
    return res;
}

SigResolution sig_resolve_data(uintptr_t module_base, const SigRegion& region,
                               const SigDataDesc& target) {
    SigResolution res;
    memset(&res, 0, sizeof(res));

    // Locate the referencing CODE first -- exactly the same policy, including
    // the free path when the hint still holds.
    SigTargetDesc code = { 0, target.name, target.pattern };
    SigPattern pat;
    char perr[96] = {0};
    if (!sig_parse(target.pattern, &pat, perr, sizeof(perr))) {
        _snprintf_s(res.why, sizeof(res.why), _TRUNCATE,
                    "malformed pattern: %s", perr);
        return res;
    }
    if (target.disp_off + 4u > (unsigned)pat.len ||
        target.insn_end_off > (unsigned)pat.len) {
        _snprintf_s(res.why, sizeof(res.why), _TRUNCATE,
                    "disp/insn offsets fall outside the pattern");
        return res;
    }

    const uint8_t* first = nullptr;
    const int n = sig_find_all(region, pat, &first);
    res.matches = n;
    if (n == 0) {
        _snprintf_s(res.why, sizeof(res.why), _TRUNCATE,
                    "referencing code not found");
        return res;
    }
    if (n > 1) {
        _snprintf_s(res.why, sizeof(res.why), _TRUNCATE,
                    "AMBIGUOUS -- %d matches for the referencing code", n);
        return res;
    }

    int32_t disp = 0;
    memcpy(&disp, first + target.disp_off, sizeof(disp));
    const uintptr_t datum = (uintptr_t)(first + target.insn_end_off) + (intptr_t)disp;

    if (datum < module_base || datum >= module_base + 0x10000000ull) {
        _snprintf_s(res.why, sizeof(res.why), _TRUNCATE,
                    "recovered address 0x%llX is outside the module",
                    (unsigned long long)datum);
        return res;
    }

    res.addr      = datum;
    res.rva       = (uint32_t)(datum - module_base);
    res.drift     = (int32_t)(res.rva - target.rva_hint);
    res.from_hint = (res.drift == 0);
    res.ok        = true;
    if (res.drift)
        _snprintf_s(res.why, sizeof(res.why), _TRUNCATE,
                    "MOVED: data rva 0x%08X -> 0x%08X (%+d)",
                    target.rva_hint, res.rva, res.drift);
    (void)code;
    return res;
}

} // namespace mgmp
