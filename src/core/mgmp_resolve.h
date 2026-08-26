// mgmp_resolve.h -- the one place that turns a Target/Call into an address.
//
// Before this existed every consumer did `base + kTargets[T].rva` and guarded it
// with a 16-byte prologue compare. That is correct on exactly one build and
// wrong on every other, and the console release is expected to bring a PC patch
// that invalidates all 46 RVAs at once.
//
// Now: mgmp_sigscan resolves each one by a signature that is unique in .text,
// with the pinned RVA kept as a hint that makes the common case free. This
// header is the accessor. `base + rva` must not appear anywhere else.
//
// ---------------------------------------------------------------------------
// A RESOLVE FAILURE IS NOT ONE KIND OF EVENT
//
// Losing the peer-cursor draw call and losing the command boundary are not the
// same accident, and collapsing them into one refusal is how a session dies
// over a nicety. So each target carries a criticality, and only CRITICAL ones
// can stop the mod from loading:
//
//   SIG_CRITICAL   lockstep cannot be correct without it. Refuse to load.
//   SIG_FEATURE    one feature turns itself off and says which. Keep going.
//
// This mirrors what the modules already did by hand -- mgmp_cursor called its
// own failure "non-fatal by design", mgmp_catsync turned cat sync off and named
// it -- and moves the judgement next to the address instead of leaving it
// scattered across five call sites.
#pragma once

#include <cstdint>

#include "mgmp_addresses.h"

namespace mgmp {

// Resolves every target, call and datum. Logs a one-line summary, plus one
// loud line per address that MOVED and one per address that failed.
//
// Returns false only when a SIG_CRITICAL target could not be resolved -- the
// caller should then refuse to hook. A false return has already logged why.
bool resolve_init(uintptr_t module_base);

// Absolute address, or 0 when that target did not resolve. Callers that can
// degrade gracefully must check for 0; callers that cannot are SIG_CRITICAL and
// resolve_init already refused on their behalf.
uintptr_t addr_of(Target t);
uintptr_t addr_of_call(Call c);

// The globals recovered from a referencing instruction's displacement.
enum DataSym : int {
    D_MewDirectorPtr = 0,
    D_MouseCache,
    D_ApplicationBase,
    D_COUNT
};
uintptr_t addr_of_data(DataSym d);

// True when every address came from its pinned RVA -- i.e. this is the build
// the signatures were generated from. False means the game moved and we
// followed it; the log says by how much.
bool resolve_is_pinned_build();

// How many resolved, moved and failed -- for the startup banner.
void resolve_counts(int* resolved, int* moved, int* failed);

} // namespace mgmp
