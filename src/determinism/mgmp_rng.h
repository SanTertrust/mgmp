// mgmp_rng.h -- the xoshiro256 draw recorder.
//
// Mewgenics has a real RNG API that takes the stream state as an explicit
// parameter, which is what makes fencing possible at all:
//
//   randint  (int    n, u64* state) -> int      @ 0x14094B0B0   ecx, rdx  -> eax
//   randfloat(double m, u64* state) -> double   @ 0x140158B80   xmm0, rdx -> xmm0
//   rand2    (double m, u64* state) -> __m128   @ 0x14094B230   xmm0, rdx -> xmm0
//
// `rand2` advances the state TWICE per call. It is tagged RNG_TWO in the record
// so the decoder does not mistake the second round's absence for a divergence.
//
// The shared "global" stream lives in thread-local storage at
// [gs:0x58][0] + 0x178. A draw that passes that exact address is a draw that
// both peers must agree on; a draw that passes anything else is using a scratch
// stream and cannot desync anyone. That single pointer comparison is the whole
// filter, and it is why `rng_global_only` can default to on without losing
// anything that matters.
//
// ---------------------------------------------------------------------------
// WHAT THIS DOES NOT SEE  -- read before trusting a clean diff
//
// Some call sites call these functions; others have the xoshiro round inlined
// into them by the compiler. Hooking the API catches the former and is blind to
// the latter. So an empty diff from this recorder is *not* proof of
// determinism -- it is proof that the ~210 call-based draw sites agree.
//
// The inlined sites are findable (byte-scan for `ror r64,19` plus `shl r64,17`,
// or xrefs to the 2^-53 constant at 0x141137238) and CLAUDE.md records the
// method. Closing that gap means either patching inlined sites individually or
// hashing the TLS state block at frame boundaries to catch drift from any
// source. The second is much cheaper and is the natural next step if Run C
// comes back clean but a real desync still shows up later.
#pragma once

#include <cstdint>

namespace mgmp {

// The TLS global stream, or nullptr if TLS is not set up on this thread yet.
uint64_t* rng_global_stream();

// Must be called before the detours run: call sites are recorded as RVAs and
// that needs the module base.
void rng_set_base(uintptr_t base);

// Detours. Installed by hooks_install() alongside the phase-1 hooks; the
// originals live here so the detours can tail-call them.
void* rng_detour_randint();
void* rng_detour_randfloat();
void* rng_detour_rand2();
void* rng_detour_rollchance();

void** rng_original_randint();
void** rng_original_randfloat();
void** rng_original_rand2();
void** rng_original_rollchance();

// Total draws seen, and of those, how many were on the global stream. Logged at
// each turn so the trace shows the recorder is alive even when the diff is
// empty.
void rng_counters(uint64_t* total, uint64_t* global);

} // namespace mgmp
