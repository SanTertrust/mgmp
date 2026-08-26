// mgmp_sigscan.h -- resolving hook targets by SIGNATURE instead of by address.
//
// Every address in this mod is pinned to one build
// (SHA256 c3a41e...439e, SizeOfImage 0x156B000). That was the right trade while
// the game was a moving target we controlled: a wrong address splices a jump
// into the middle of an unrelated function, and refusing to load is strictly
// better. The console release is expected to ship a PC patch alongside it, and
// on that day EVERY RVA in mgmp_addresses.h becomes wrong at once.
//
// This module makes the pinned RVA a CROSS-CHECK rather than the answer.
//
// ---------------------------------------------------------------------------
// WHY THE EXISTING 16-BYTE PROLOGUES CANNOT BE THE PATTERN
//
// They are not unique, and it is not close. Measured against the shipped PE:
//
//   MewSaveFile::Load_0  48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55
//                        -> 6 occurrences in the image
//   sub_1400B6A90        48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57
//                        -> 6 occurrences in the image
//
// MSVC emits the same register-save prologue for every large function in a
// translation unit. That is already recorded in CLAUDE.md as the reason
// `CallDesc` grew a second window: save_adventure and ContinueAdventure are
// byte-identical for their first TWENTY-ONE bytes, and a 16-byte guard on the
// first happily accepts the second -- the one that destroys the House scene and
// frees the live cat registry.
//
// A 16-byte prologue works as a guard ONLY because it is checked at a fixed
// RVA: it answers "is this still the pinned build", never "where is this
// function". The moment we want the second question, uniqueness stops being
// optional. So a signature here is required to match EXACTLY ONCE in .text --
// and that requirement subsumes the second-window hack entirely.
//
// ---------------------------------------------------------------------------
// WHAT GETS WILDCARDED, AND WHY IT IS NOT A HEURISTIC
//
// A pattern is authored as text: "48 89 5C 24 ? 55 ? ? ? ? 48 8D 0D ? ? ? ?".
// `?` is one wildcard byte. The bytes that must be wildcarded are exactly the
// ones a rebuild is guaranteed to move, and they are identifiable from the
// instruction encoding rather than guessed:
//
//   call/jmp rel32      the 4 displacement bytes -- every call in the image
//                       shifts when anything before it changes size
//   lea/mov RIP-rel     the 4 displacement bytes -- all string and global refs
//   imm32/imm64 that
//     lands in the image a hardcoded address (vftables, string tables)
//
// Everything else is KEPT, deliberately, including struct displacements like
// [rcx+0E8h]. Those are semantic: +0xE8 is the ability vector's capacity field,
// and if it moves we want the signature to break loudly rather than resolve to
// a function that now means something else. Keeping them is what makes a short
// pattern unique.
//
// `mod/tools/ida/gen_sigs.py` derives all of this from the disassembly and
// grows each pattern one instruction at a time until it matches exactly once.
// It is re-runnable: point it at the .i64 for the NEW build and it regenerates
// the whole table. That is the migration path, and it is also the recovery path.
//
// ---------------------------------------------------------------------------
// RESOLUTION ORDER -- the point of which is that the common case costs NOTHING
//
//   1. Verify the pattern at base + rva_hint.
//        Hit  -> done. No scan happens at all. This is the pinned build, and
//                startup cost is 46 masked memcmps of a few dozen bytes.
//   2. Scan .text for the pattern.
//        exactly 1 -> resolved. LOG THE DRIFT (old rva -> new rva) loudly:
//                     the game updated and we survived, and the log is the
//                     only place that will ever say so.
//        0 matches -> REFUSE this target. The function changed shape.
//        2+        -> REFUSE this target. Ambiguous is not a coin flip we are
//                     allowed to toss; MSVC ICF folds identical functions, so
//                     "two matches" is a real and expected outcome.
//
// Step 1 deliberately does NOT also verify uniqueness. On the pinned build the
// hint is already known-good from the .i64, and scanning 46 patterns to
// re-derive what we know would cost real milliseconds every launch to learn
// nothing. Uniqueness is the GENERATOR's job, checked once, offline.
//
// A refusal is per-target, not global. Losing T_TimeDelayTick after a patch
// should not cost the whole session; losing T_ApplyAction should, and that
// judgement belongs to the caller, which knows which targets are load-bearing.
//
// ---------------------------------------------------------------------------
// SIMD -- and one correction worth stating plainly
//
// AVX (1) is NOT useful here. Its 256-bit operations are float-only; the byte
// compare this needs, `_mm256_cmpeq_epi8`, is AVX2. So the runtime check that
// matters is AVX2, not AVX. SSE2 is architectural on x86-64 and needs no check
// at all -- there is no machine that runs this game and lacks it, which makes
// the scalar path dead code kept only for clarity.
//
// Detection is `cpuid` leaf 7 EBX bit 5, gated on leaf 1 ECX bit 27 (OSXSAVE)
// and bit 28 (AVX), and then on XCR0 bits 1 and 2 via `xgetbv`. The XCR0 step
// is not optional: a CPU can report AVX2 while the OS does not save YMM state
// across a context switch, and using it there corrupts registers silently.
//
// The scan itself is anchor-driven. Comparing the pattern at every offset is
// wasteful; instead broadcast ONE byte of the pattern and let the vector unit
// find its candidate positions, then verify the full masked pattern only there.
// Which byte matters enormously: `48` is the REX prefix and occurs in millions
// of positions, while an opcode like `0F 2F` occurs in thousands. So the module
// builds a 256-entry frequency histogram of .text once and each pattern anchors
// on its RAREST literal byte. That is the difference between ~1 verification
// per megabyte and ~50,000.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

// --- CPU dispatch ----------------------------------------------------------

enum SigCpu {
    SIG_CPU_SCALAR = 0,  // memchr; kept for clarity, unreachable on x86-64
    SIG_CPU_SSE2   = 1,  // 16 bytes/step, architectural on x86-64
    SIG_CPU_AVX2   = 2,  // 32 bytes/step
};

// Detected once, on first use. Safe to call from any thread.
SigCpu      sigscan_cpu();
const char* sigscan_cpu_name();

// --- patterns --------------------------------------------------------------

// Long enough for the worst case the generator produced; a pattern only grows
// until it is unique, and in practice that is well under 64 bytes.
static const int kSigMaxBytes = 256;

struct SigPattern {
    uint8_t bytes[kSigMaxBytes];
    uint8_t mask [kSigMaxBytes];   // 1 = must match, 0 = wildcard
    int     len;
    int     anchor;                // index of the rarest literal byte
    int     literals;              // how many mask[i] == 1
};

// Parses "48 89 5C 24 ? 55" (also accepts "??" and "xx" for a wildcard).
// A pattern must begin and end with a literal byte -- a leading wildcard makes
// the match address ambiguous, and a trailing one adds no uniqueness.
bool sig_parse(const char* text, SigPattern* out, char* err, size_t err_size);

// --- the region we search --------------------------------------------------

struct SigRegion {
    const uint8_t* base;
    size_t         size;
    uint32_t       rva;   // of base, so a hit can be reported as an RVA
};

// The executable section of a loaded module, from its own section headers.
// Searching .text alone rather than the whole image is not just speed: a
// pattern that "matches" inside .rdata is a false positive by construction.
bool sigscan_text_section(uintptr_t module_base, SigRegion* out);

// Builds the byte-frequency histogram used to pick anchors. Call once per
// region before sig_find; sig_parse works without it but picks anchor 0.
void sigscan_profile(const SigRegion& region);

// Re-picks the anchor for an already-parsed pattern using the current profile.
void sig_choose_anchor(SigPattern* p);

// --- searching -------------------------------------------------------------

bool sig_matches_at(const uint8_t* at, const SigPattern& p);

// Scans the whole region and reports how many matches exist. `first` receives
// the first match. Counting ALL matches rather than stopping at one is
// deliberate: ambiguity must be detectable, and it is the expensive-but-correct
// half of the reason this is only reached when the hint has already failed.
int sig_find_all(const SigRegion& region, const SigPattern& p, const uint8_t** first);

// --- resolution ------------------------------------------------------------

struct SigTargetDesc {
    uint32_t    rva_hint;   // the pinned-build RVA. A cross-check, not the answer.
    const char* name;
    const char* pattern;
};

struct SigResolution {
    uintptr_t addr;        // absolute; 0 when !ok
    uint32_t  rva;         // resolved rva
    int32_t   drift;       // resolved_rva - rva_hint; 0 when the hint held
    int       matches;     // what the scan saw (1 on the hint path)
    bool      from_hint;   // true = pinned build, no scan happened
    bool      ok;
    char      why[128];    // set when !ok, and when drift != 0
};

// The whole policy above, for one target.
SigResolution sig_resolve(uintptr_t module_base, const SigRegion& region,
                          const SigTargetDesc& target);

// --- DATA addresses, which cannot be scanned for at all --------------------
//
// Six of the mod's hardcoded addresses are not functions:
//
//   kRva_MewDirectorPtr      0x013D1970   the MewDirector* global
//   kRva_MouseCache          0x012F2E80   the engine's cached mouse, 2 doubles
//   kRva_ApplicationBase     0x013BB790   ApplicationBase**
//   kRva_SdlSwapSlot         0x012DE650   an SDL_DYNAPI jump-table slot
//   kRva_SdlGetWindowSizeSlot        0x012DF170
//   kRva_SdlGetWindowSizePxSlot      0x012DF178
//
// Scanning for them is meaningless -- their contents are runtime state, not a
// fixed byte string. They are recovered the way CLAUDE.md already insists the
// SDL slots must be: find the CODE that references the datum, and decode the
// reference's own displacement. "Never by counting" generalises to "never by
// address" once the build can move.
//
// So a data target carries a unique code pattern plus two offsets into it:
// where the 4-byte RIP-relative displacement sits, and where the referencing
// instruction ENDS -- because on x86-64 the displacement is relative to the end
// of the instruction, not to the operand and not to the pattern.
//
//   data_address = match + insn_end_off + (int32)disp
//
// The same drift reporting applies: if the recovered address differs from
// rva_hint, that is the game having moved and it gets logged loudly.

struct SigDataDesc {
    uint32_t    rva_hint;     // pinned-build RVA of the DATUM
    const char* name;
    const char* pattern;      // unique CODE pattern containing the reference
    uint16_t    disp_off;     // offset of the disp32 within the pattern
    uint16_t    insn_end_off; // offset of the end of the referencing instruction
};

SigResolution sig_resolve_data(uintptr_t module_base, const SigRegion& region,
                               const SigDataDesc& target);

} // namespace mgmp
