// mgmp_resolve.cpp -- see mgmp_resolve.h.
#include "mgmp_resolve.h"

#include "mgmp_log.h"
#include "mgmp_sigscan.h"
#include "mgmp_sigs.generated.h"

#include <cstring>

namespace mgmp {
namespace {

// The generated tables are indexed by the enums, so a length mismatch means
// somebody added a target and did not re-run gen_sigs.py. Catch it at compile
// time rather than by silently resolving the wrong entry.
static_assert(sizeof(kTargetSigs) / sizeof(kTargetSigs[0]) == (size_t)T_COUNT,
              "mgmp_sigs.generated.h is stale -- re-run mod/tools/ida/gen_sigs.py");
static_assert(sizeof(kCallSigs) / sizeof(kCallSigs[0]) == (size_t)C_COUNT,
              "mgmp_sigs.generated.h is stale -- re-run mod/tools/ida/gen_sigs.py");
static_assert(sizeof(kSigData) / sizeof(kSigData[0]) == (size_t)D_COUNT,
              "mgmp_sigs.generated.h is stale -- re-run mod/tools/ida/gen_sigs.py");

// Criticality. The question is NOT "how much do we like this feature" but
// "if this address goes missing, does the mod fail VISIBLY or SILENTLY".
//
// A missing draw call turns a cursor off -- visible, and the module already
// says so. A missing GetChoice means remote decisions are never injected and
// both peers quietly play their own game: that is a silent wrong game, which
// CLAUDE.md ranks as strictly worse than a stall. Those four are the seam
// lockstep is built on, so they refuse.
bool is_critical(Target t) {
    switch (t) {
    case T_NextTurn:     // the turn barrier and the hash exchange
    case T_GetChoice:    // the injection seam for remote decisions
    case T_ApplyAction:  // the command boundary
    case T_FrameBegin:   // the socket pump
        return true;
    default:
        return false;
    }
}

uintptr_t g_target[T_COUNT];
uintptr_t g_call  [C_COUNT];
uintptr_t g_data  [D_COUNT];

int  g_resolved = 0, g_moved = 0, g_failed = 0;
bool g_pinned   = true;
bool g_ready    = false;

// One target, one log line -- but only when there is something to say. On the
// pinned build this is silent for all 49, which is the point.
void note(const char* kind, const char* name, const SigResolution& r) {
    if (!r.ok) {
        log_raw("  [!] %s %-34s UNRESOLVED: %s", kind, name, r.why);
        return;
    }
    if (r.drift != 0)
        log_raw("  [~] %s %-34s %s", kind, name, r.why);
}

} // namespace

bool resolve_init(uintptr_t module_base) {
    memset(g_target, 0, sizeof(g_target));
    memset(g_call,   0, sizeof(g_call));
    memset(g_data,   0, sizeof(g_data));
    g_resolved = g_moved = g_failed = 0;
    g_pinned   = true;
    g_ready    = false;

    SigRegion text;
    if (!sigscan_text_section(module_base, &text)) {
        log_raw("[!] could not find an executable section -- cannot resolve anything");
        return false;
    }
    sigscan_profile(text);

    log_raw("resolve: .text rva %08X, %.1f MB, scanner = %s",
            text.rva, text.size / (1024.0 * 1024.0), sigscan_cpu_name());

    bool critical_lost = false;

    for (int i = 0; i < T_COUNT; ++i) {
        // Anchors are chosen against the profile, which sig_parse could not see
        // when the table was a literal; sig_resolve re-parses, so this is free.
        const SigResolution r = sig_resolve(module_base, text, kTargetSigs[i]);
        note("target", kTargetSigs[i].name, r);
        if (r.ok) {
            g_target[i] = r.addr;
            ++g_resolved;
            if (r.drift) { ++g_moved; g_pinned = false; }
        } else {
            ++g_failed;
            if (is_critical((Target)i)) {
                log_raw("[!] %s is CRITICAL -- lockstep cannot be correct without it",
                        kTargetSigs[i].name);
                critical_lost = true;
            }
        }
    }

    for (int i = 0; i < C_COUNT; ++i) {
        const SigResolution r = sig_resolve(module_base, text, kCallSigs[i]);
        note("call  ", kCallSigs[i].name, r);
        if (r.ok) {
            g_call[i] = r.addr;
            ++g_resolved;
            if (r.drift) { ++g_moved; g_pinned = false; }
        } else {
            ++g_failed;
        }
    }

    for (int i = 0; i < D_COUNT; ++i) {
        const SigResolution r = sig_resolve_data(module_base, text, kSigData[i]);
        note("data  ", kSigData[i].name, r);
        if (r.ok) {
            g_data[i] = r.addr;
            ++g_resolved;
            if (r.drift) { ++g_moved; g_pinned = false; }
        } else {
            ++g_failed;
        }
    }

    g_ready = true;

    if (g_pinned && !g_failed) {
        log_raw("resolve: all %d addresses matched their pinned RVAs "
                "-- this is the build the signatures were generated from.",
                g_resolved);
    } else {
        log_raw("resolve: %d resolved, %d MOVED, %d failed.",
                g_resolved, g_moved, g_failed);
        if (g_moved)
            log_raw("[~] the game has been updated and the signatures followed it. "
                    "Re-run mod/tools/ida/gen_sigs.py against the new .i64 to "
                    "re-pin the hints and shrink startup back to zero scanning.");
    }

    return !critical_lost;
}

uintptr_t addr_of(Target t) {
    if (!g_ready || t < 0 || t >= T_COUNT) return 0;
    return g_target[t];
}

uintptr_t addr_of_call(Call c) {
    if (!g_ready || c < 0 || c >= C_COUNT) return 0;
    return g_call[c];
}

uintptr_t addr_of_data(DataSym d) {
    if (!g_ready || d < 0 || d >= D_COUNT) return 0;
    return g_data[d];
}

bool resolve_is_pinned_build() { return g_pinned; }

void resolve_counts(int* resolved, int* moved, int* failed) {
    if (resolved) *resolved = g_resolved;
    if (moved)    *moved    = g_moved;
    if (failed)   *failed   = g_failed;
}

} // namespace mgmp
