// test_sigscan.cpp -- pins the signature scanner's pure logic.
//
// What this can and cannot cover is the usual split for this project, and it is
// worth stating because the last three bugs that shipped were all on the wrong
// side of it: these tests pin WHAT sig_parse / sig_find_all / sig_resolve
// RETURN. They say nothing about whether the generated patterns are correct for
// the real binary -- that is mod/tools/verify_sigs.py's job, against the shipped
// PE, and neither check substitutes for the other.
//
// The resolve tests matter most. sig_resolve is the piece with a POLICY in it,
// and the policy has three outcomes that must stay distinguishable: the hint
// held (free), the hint moved (follow, and say so), and ambiguity (refuse). An
// earlier draft collapsed the last two, which is how you get a mod that silently
// hooks whichever of two ICF-folded functions the scan happened to reach first.
#include "mgmp_sigscan.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace mgmp;

static int g_fail = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__);             \
            std::printf(__VA_ARGS__);                                      \
            std::printf("\n");                                             \
            ++g_fail;                                                      \
        }                                                                  \
    } while (0)

static SigPattern parsed(const char* text) {
    SigPattern p{};
    char err[96] = {};
    const bool ok = sig_parse(text, &p, err, sizeof(err));
    CHECK(ok, "sig_parse rejected a good pattern '%s': %s", text, err);
    return p;
}

static void test_parse() {
    std::printf("parse\n");

    SigPattern p = parsed("48 89 5C 24 ? 55 56 57 41 54");
    CHECK(p.len == 10, "len %d != 10", p.len);
    CHECK(p.literals == 9, "literals %d != 9", p.literals);
    CHECK(p.bytes[0] == 0x48, "byte0 %02X", p.bytes[0]);
    CHECK(p.mask[4] == 0, "byte4 should be a wildcard");
    CHECK(p.mask[5] == 1 && p.bytes[5] == 0x55, "byte5 wrong");

    // "??" is the other spelling of one wildcard byte, not two.
    SigPattern q = parsed("48 89 ?? 24 55 56 57 41 54 55");
    CHECK(q.len == 10, "'??' consumed the wrong width: len %d", q.len);

    char err[96] = {};
    SigPattern bad{};
    CHECK(!sig_parse("? 48 89 5C 24 55 56 57 41 54", &bad, err, sizeof(err)),
          "a leading wildcard must be refused -- the match address is ambiguous");
    CHECK(!sig_parse("48 89 5C 24 55 56 57 41 54 ?", &bad, err, sizeof(err)),
          "a trailing wildcard must be refused -- it adds no uniqueness");
    CHECK(!sig_parse("48 89 5C", &bad, err, sizeof(err)),
          "too few literals must be refused");
    CHECK(!sig_parse("", &bad, err, sizeof(err)), "empty must be refused");
    CHECK(!sig_parse("48 89 ZZ 24 55 56 57 41 54 55", &bad, err, sizeof(err)),
          "non-hex must be refused");
}

static void test_match() {
    std::printf("match\n");

    SigPattern p = parsed("11 22 ? 44 55 66 77 88 99 AA");
    const uint8_t good[] = { 0x11,0x22,0xFF,0x44,0x55,0x66,0x77,0x88,0x99,0xAA };
    const uint8_t also[] = { 0x11,0x22,0x00,0x44,0x55,0x66,0x77,0x88,0x99,0xAA };
    const uint8_t bad [] = { 0x11,0x22,0xFF,0x45,0x55,0x66,0x77,0x88,0x99,0xAA };
    CHECK(sig_matches_at(good, p), "wildcard byte should match anything");
    CHECK(sig_matches_at(also, p), "wildcard byte should match anything");
    CHECK(!sig_matches_at(bad, p), "a literal mismatch must fail");
}

// A haystack with the needle planted at known offsets.
static std::vector<uint8_t> haystack(size_t size, const std::vector<size_t>& at,
                                     const uint8_t* needle, size_t nlen) {
    std::vector<uint8_t> v(size, 0x90);          // nop filler
    for (size_t off : at) std::memcpy(&v[off], needle, nlen);
    return v;
}

static void test_find() {
    std::printf("find\n");

    const uint8_t needle[] = { 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA };
    SigPattern p = parsed("11 22 33 ? 55 66 77 88 99 AA");

    {   // exactly one
        auto v = haystack(4096, { 1000 }, needle, sizeof(needle));
        SigRegion r{ v.data(), v.size(), 0x1000 };
        sigscan_profile(r);
        sig_choose_anchor(&p);
        const uint8_t* first = nullptr;
        CHECK(sig_find_all(r, p, &first) == 1, "expected exactly one match");
        CHECK(first == v.data() + 1000, "match at the wrong offset");
    }
    {   // two -- the case that must stay distinguishable from one
        auto v = haystack(4096, { 100, 3000 }, needle, sizeof(needle));
        SigRegion r{ v.data(), v.size(), 0x1000 };
        const uint8_t* first = nullptr;
        CHECK(sig_find_all(r, p, &first) == 2, "ambiguity must be COUNTED, not "
              "short-circuited -- refusing needs to know there were two");
    }
    {   // none
        auto v = haystack(4096, {}, needle, sizeof(needle));
        SigRegion r{ v.data(), v.size(), 0x1000 };
        const uint8_t* first = nullptr;
        CHECK(sig_find_all(r, p, &first) == 0, "expected no match");
        CHECK(first == nullptr, "first must stay null when nothing matched");
    }
    {   // a match that starts before the anchor, and one that runs off the end
        auto v = haystack(64, { 0 }, needle, sizeof(needle));
        SigRegion r{ v.data(), v.size(), 0x1000 };
        const uint8_t* first = nullptr;
        CHECK(sig_find_all(r, p, &first) == 1, "a match at offset 0 must be found "
              "even when the anchor is not byte 0");
    }
}

static void test_anchor() {
    std::printf("anchor\n");

    // 0x90 is everywhere in this haystack; 0xAA appears only in the needle. The
    // anchor must be the rare one -- that choice is worth ~85x on the real
    // .text, and nothing else in the module notices if it silently regresses.
    const uint8_t needle[] = { 0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0xAA };
    auto v = haystack(8192, { 4000 }, needle, sizeof(needle));
    SigRegion r{ v.data(), v.size(), 0x1000 };
    sigscan_profile(r);

    SigPattern p = parsed("90 90 90 90 90 90 90 90 90 AA");
    sig_choose_anchor(&p);
    CHECK(p.anchor == 9, "anchor %d -- expected the rare byte at index 9", p.anchor);

    const uint8_t* first = nullptr;
    CHECK(sig_find_all(r, p, &first) == 1, "rare-byte anchoring changed the result");
}

static void test_resolve() {
    std::printf("resolve\n");

    const uint8_t needle[] = { 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xAA };
    const char*   text     = "11 22 33 ? 55 66 77 88 99 AA";

    {   // the hint holds -- the pinned build, and the only path that must be free
        auto v = haystack(4096, { 512 }, needle, sizeof(needle));
        SigRegion r{ v.data(), v.size(), 0 };
        sigscan_profile(r);
        const uintptr_t base = (uintptr_t)v.data();
        SigTargetDesc t{ 512, "hit", text };
        SigResolution res = sig_resolve(base, r, t);
        CHECK(res.ok, "should resolve");
        CHECK(res.from_hint, "must have taken the hint path");
        CHECK(res.drift == 0, "drift %d != 0", res.drift);
        CHECK(res.addr == base + 512, "wrong address");
    }
    {   // the hint is wrong but the pattern is unique -- follow it, and report
        auto v = haystack(4096, { 2048 }, needle, sizeof(needle));
        SigRegion r{ v.data(), v.size(), 0 };
        const uintptr_t base = (uintptr_t)v.data();
        SigTargetDesc t{ 512, "moved", text };       // claims 512, really at 2048
        SigResolution res = sig_resolve(base, r, t);
        CHECK(res.ok, "a moved target must still resolve");
        CHECK(!res.from_hint, "must have scanned");
        CHECK(res.drift == 2048 - 512, "drift %d wrong", res.drift);
        CHECK(res.addr == base + 2048, "wrong address");
        CHECK(std::strstr(res.why, "MOVED") != nullptr,
              "a move must be reported: why='%s'", res.why);
    }
    {   // ambiguous -- REFUSE. Never pick one.
        auto v = haystack(4096, { 100, 2048 }, needle, sizeof(needle));
        SigRegion r{ v.data(), v.size(), 0 };
        const uintptr_t base = (uintptr_t)v.data();
        SigTargetDesc t{ 512, "folded", text };
        SigResolution res = sig_resolve(base, r, t);
        CHECK(!res.ok, "two matches must NOT resolve");
        CHECK(res.addr == 0, "an unresolved target must report address 0");
        CHECK(res.matches == 2, "matches %d != 2", res.matches);
        CHECK(std::strstr(res.why, "AMBIGUOUS") != nullptr,
              "why='%s'", res.why);
    }
    {   // absent
        auto v = haystack(4096, {}, needle, sizeof(needle));
        SigRegion r{ v.data(), v.size(), 0 };
        SigTargetDesc t{ 512, "gone", text };
        SigResolution res = sig_resolve((uintptr_t)v.data(), r, t);
        CHECK(!res.ok, "an absent target must not resolve");
        CHECK(res.addr == 0, "an unresolved target must report address 0");
    }
    {   // a hint pointing outside the region must not be dereferenced
        auto v = haystack(4096, { 2048 }, needle, sizeof(needle));
        SigRegion r{ v.data(), v.size(), 0 };
        SigTargetDesc t{ 0x7FFFFFFF, "wild-hint", text };
        SigResolution res = sig_resolve((uintptr_t)v.data(), r, t);
        CHECK(res.ok && !res.from_hint,
              "an out-of-range hint must fall through to the scan, not fault");
    }
}

static void test_resolve_data() {
    std::printf("resolve_data\n");

    // `lea rcx, [rip+disp32]` at pattern offset 3; the instruction ends at 7,
    // which is the RIP base. The tail exists so the pattern clears the
    // 8-literal minimum -- a 6-literal pattern is correctly refused as too weak,
    // and the real generated data patterns run 38-49 bytes.
    std::vector<uint8_t> v(4096, 0x90);
    const size_t site = 0x100;
    const uint8_t code[] = { 0x48, 0x8D, 0x0D, 0,0,0,0,
                             0x33, 0xC0, 0xC3, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    std::memcpy(&v[site], code, sizeof(code));

    const size_t insn_end = site + 7;              // rip base
    const int32_t disp = (int32_t)(0x800 - insn_end);
    std::memcpy(&v[site + 3], &disp, sizeof(disp));

    SigRegion r{ v.data(), v.size(), 0 };
    sigscan_profile(r);
    const uintptr_t base = (uintptr_t)v.data();

    SigDataDesc d{ 0x800, "datum", "48 8D 0D ? ? ? ? 33 C0 C3 41 57 48 83 EC 20", 3, 7 };
    SigResolution res = sig_resolve_data(base, r, d);
    CHECK(res.ok, "should recover the datum: %s", res.why);
    CHECK(res.rva == 0x800, "recovered rva 0x%X != 0x800", res.rva);
    CHECK(res.drift == 0, "drift %d != 0", res.drift);

    // And the same code claiming the wrong datum must report the drift rather
    // than quietly returning the address the table asked for.
    SigDataDesc wrong{ 0x900, "datum", "48 8D 0D ? ? ? ? 33 C0 C3 41 57 48 83 EC 20", 3, 7 };
    SigResolution res2 = sig_resolve_data(base, r, wrong);
    CHECK(res2.ok, "should still recover");
    CHECK(res2.rva == 0x800, "must trust the DISPLACEMENT, not the hint");
    CHECK(res2.drift == (int32_t)(0x800 - 0x900), "drift %d wrong", res2.drift);
}

int main() {
    std::printf("sigscan: %s\n", sigscan_cpu_name());
    test_parse();
    test_match();
    test_find();
    test_anchor();
    test_resolve();
    test_resolve_data();

    if (g_fail) { std::printf("\n%d FAILURE(S)\n", g_fail); return 1; }
    std::printf("\nall sigscan tests passed\n");
    return 0;
}
