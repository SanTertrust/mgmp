#include "mgmp_replay.h"

#include "mgmp_ability.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_record.h"
#include "mgmp_rtti.h"
#include "mgmp_turnaction.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// One decision, flattened out of the capture. Names are copied in rather than
// referenced by interned id: the ids are only meaningful inside the capture
// they came from, and the replayer compares against live RTTI strings.
struct Step {
    uint32_t turn;
    uint32_t type;        // 2 ability or 3 end turn; nothing else is replayable
    int32_t  tx, ty;
    int32_t  dx, dy;
    uint8_t  b30, b31;
    uint8_t  slot_kind;
    uint8_t  slot_index;
    char     gon[64];     // ability's authored GON name, "" if unknown
    char     brain[96];   // class of the actor's brain when this was recorded
    bool     replayable;  // brain matches the filter -- i.e. it was the human's
};

const size_t kMaxSteps = 4096;

Step*    g_steps    = nullptr;
uint32_t g_count    = 0;
uint32_t g_head     = 0;
bool     g_active   = false;
bool     g_outstanding = false;

char g_brain_filter[64] = "PlayerBrain";

volatile LONG g_injected = 0, g_matched = 0, g_diverged = 0;

CRITICAL_SECTION g_cs;
bool             g_cs_ready = false;

struct Guard {
    Guard()  { if (g_cs_ready) EnterCriticalSection(&g_cs); }
    ~Guard() { if (g_cs_ready) LeaveCriticalSection(&g_cs); }
};

// -- capture parsing -------------------------------------------------------

// Reads the whole file. Captures are a few hundred KB and this runs once at
// init, off the hot path, so streaming would be complexity for nothing.
uint8_t* read_all(const wchar_t* path, size_t* out_size) {
    *out_size = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64 << 20)) {
        CloseHandle(h);
        return nullptr;
    }
    size_t   n   = (size_t)sz.QuadPart;
    uint8_t* buf = (uint8_t*)VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
    if (!buf) { CloseHandle(h); return nullptr; }

    size_t got = 0;
    while (got < n) {
        DWORD chunk = (DWORD)((n - got) > 0x100000 ? 0x100000 : (n - got));
        DWORD read  = 0;
        if (!ReadFile(h, buf + got, chunk, &read, nullptr) || read == 0) break;
        got += read;
    }
    CloseHandle(h);
    if (got != n) { VirtualFree(buf, 0, MEM_RELEASE); return nullptr; }
    *out_size = n;
    return buf;
}

// Interned-id -> string, for the two tables the replayer needs to resolve
// (EV_CLASS for the brain, EV_NAME for the ability's GON name).
const size_t kIdMax = 1024;
struct StrTable {
    char text[kIdMax][96];
    void set(uint32_t id, const char* s) {
        if (id == 0 || id >= kIdMax) return;
        strncpy_s(text[id], sizeof(text[id]), s, _TRUNCATE);
    }
    const char* get(uint32_t id) const {
        return (id && id < kIdMax) ? text[id] : "";
    }
};

bool brain_matches_filter(const char* brain_class) {
    if (!brain_class || !brain_class[0]) return false;
    return strstr(brain_class, g_brain_filter) != nullptr;
}

} // namespace

bool replay_init(const wchar_t* path, const char* brain_filter) {
    if (brain_filter && brain_filter[0])
        strncpy_s(g_brain_filter, sizeof(g_brain_filter), brain_filter, _TRUNCATE);

    size_t   size = 0;
    uint8_t* buf  = read_all(path, &size);
    if (!buf) {
        log_line("REPLAY", "cannot read capture -- replay disabled");
        return false;
    }

    // The capture must be v5 or newer. Older ones recorded ability pointers and
    // nothing else, and those pointers do not reproduce -- there is literally
    // nothing in a v4 capture that can name an ability on a fresh run.
    uint32_t version = 0;
    {
        // EV_META is the first record: EvHead then {magic, version, image}.
        if (size < sizeof(EvHead) + 12) {
            VirtualFree(buf, 0, MEM_RELEASE);
            log_line("REPLAY", "capture too short -- replay disabled");
            return false;
        }
        uint32_t magic = 0;
        memcpy(&magic, buf + sizeof(EvHead), 4);
        memcpy(&version, buf + sizeof(EvHead) + 4, 4);
        if (magic != kRecordMagic) {
            VirtualFree(buf, 0, MEM_RELEASE);
            log_line("REPLAY", "bad magic %08X -- replay disabled", magic);
            return false;
        }
        if (version < 5) {
            VirtualFree(buf, 0, MEM_RELEASE);
            log_line("REPLAY", "capture is v%u; replay needs v5+ (no slot identity "
                               "before that, and the pointers it does have do not "
                               "reproduce) -- replay disabled", version);
            return false;
        }
    }

    StrTable* classes = (StrTable*)VirtualAlloc(nullptr, sizeof(StrTable),
                                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    StrTable* names   = (StrTable*)VirtualAlloc(nullptr, sizeof(StrTable),
                                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_steps = (Step*)VirtualAlloc(nullptr, sizeof(Step) * kMaxSteps,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!classes || !names || !g_steps) {
        if (classes) VirtualFree(classes, 0, MEM_RELEASE);
        if (names)   VirtualFree(names, 0, MEM_RELEASE);
        if (g_steps) { VirtualFree(g_steps, 0, MEM_RELEASE); g_steps = nullptr; }
        VirtualFree(buf, 0, MEM_RELEASE);
        log_line("REPLAY", "out of memory -- replay disabled");
        return false;
    }

    uint32_t skipped_types = 0, skipped_ai = 0;
    size_t   off = 0;
    while (off + sizeof(EvHead) <= size) {
        EvHead h{};
        memcpy(&h, buf + off, sizeof(h));
        size_t payload = off + sizeof(h);
        if (payload + h.len > size) break;          // truncated tail; stop cleanly
        const uint8_t* p = buf + payload;

        if (h.kind == EV_CLASS && h.len > 4) {
            uint32_t id = 0; memcpy(&id, p, 4);
            classes->set(id, (const char*)p + 4);
        } else if (h.kind == EV_NAME && h.len > 4) {
            uint32_t id = 0; memcpy(&id, p, 4);
            names->set(id, (const char*)p + 4);
        } else if (h.kind == EV_ACTION && h.len >= sizeof(EvAction)) {
            EvAction a{};
            memcpy(&a, p, sizeof(a));

            // Only a brain ever emits 2 or 3. A 6 is a reaction broadcast that
            // NextTurn fires directly, a 7 invokes a std::function queued by a
            // passive; both are regenerated locally and must not be replayed.
            if (a.type == TA_Ability || a.type == TA_EndTurn) {
                if (g_count < kMaxSteps) {
                    Step& s = g_steps[g_count];
                    memset(&s, 0, sizeof(s));
                    s.turn       = a.turn;
                    s.type       = a.type;
                    s.tx = a.target_x; s.ty = a.target_y;
                    s.dx = a.dir_x;    s.dy = a.dir_y;
                    s.b30 = a.b30;     s.b31 = a.b31;
                    s.slot_kind  = a.slot_kind;
                    s.slot_index = a.slot_index;
                    strncpy_s(s.gon,   sizeof(s.gon),   names->get(a.ability_name), _TRUNCATE);
                    strncpy_s(s.brain, sizeof(s.brain), classes->get(a.brain_cls),  _TRUNCATE);
                    s.replayable = brain_matches_filter(s.brain);
                    if (!s.replayable) ++skipped_ai;
                    ++g_count;
                }
            } else {
                ++skipped_types;
            }
        }
        off = payload + h.len;
    }

    VirtualFree(classes, 0, MEM_RELEASE);
    VirtualFree(names, 0, MEM_RELEASE);
    VirtualFree(buf, 0, MEM_RELEASE);

    if (g_count == 0) {
        VirtualFree(g_steps, 0, MEM_RELEASE);
        g_steps = nullptr;
        log_line("REPLAY", "capture holds no type-2/3 actions -- replay disabled");
        return false;
    }

    InitializeCriticalSection(&g_cs);
    g_cs_ready = true;
    g_head     = 0;
    g_active   = true;

    log_line("REPLAY", "loaded %u action(s) from a v%u capture "
                       "(%u injectable as '%s', %u left to the AI, %u type-6/7 skipped)",
             g_count, version, g_count - skipped_ai, g_brain_filter,
             skipped_ai, skipped_types);
    return true;
}

void replay_shutdown() {
    if (!g_active) return;
    uint32_t inj, mat, div, rem;
    replay_stats(&inj, &mat, &div, &rem);
    log_line("REPLAY", "done: %u injected, %u matched, %u diverged, %u never reached",
             inj, mat, div, rem);
    if (div == 0 && rem == 0)
        log_line("REPLAY", "every recorded action reproduced in order");

    g_active = false;
    if (g_steps) { VirtualFree(g_steps, 0, MEM_RELEASE); g_steps = nullptr; }
    if (g_cs_ready) { DeleteCriticalSection(&g_cs); g_cs_ready = false; }
}

bool replay_active()      { return g_active; }
bool replay_outstanding() { return g_active && g_outstanding; }

bool replay_fill_choice(void* brain, void* out) {
    if (!g_active || !brain || !out) return false;

    Guard g;
    if (g_outstanding || g_head >= g_count) return false;

    const Step& s = g_steps[g_head];
    if (!s.replayable) return false;      // an AI decision: let the AI make it

    // Only inject into the brain the action was recorded on. Comparing live
    // RTTI against the recorded class also catches the case where the turn
    // order itself diverged -- we would be about to hand a PatternBrain the
    // human's move.
    char live[96];
    rtti_class_name(brain, live, sizeof(live));
    if (strcmp(live, s.brain) != 0) return false;

    // Brain+0x38 is the owning Character (Hex-Rays types it as such in
    // Brain::UpdateDecision). Going through the brain rather than TurnControl
    // keeps this hook independent of which other hooks have fired.
    const void* actor = nullptr;
    if (!mem_read((const uint8_t*)brain + 0x38, &actor, sizeof(actor)) || !actor)
        return false;

    void* ability = nullptr;
    if (s.type == TA_Ability) {
        AbilitySlot slot{ s.slot_kind, s.slot_index };
        ability = ability_from_slot(actor, slot);
        if (!ability) {
            log_line("REPLAY", "step %u/%u: slot %u:%u empty on this actor -- "
                               "cannot inject, falling back to live input",
                     g_head, g_count, s.slot_kind, s.slot_index);
            return false;
        }
        // The second identity. If the slot resolves to an ability with a
        // different authored name, the two disagree and the run has already
        // diverged -- say so instead of quietly playing the wrong ability.
        char gon[64];
        if (s.gon[0] && ability_gon_name(ability, gon, sizeof(gon)) &&
            strcmp(gon, s.gon) != 0) {
            InterlockedIncrement(&g_diverged);
            log_line("REPLAY", "!! step %u/%u: slot %u:%u holds '%s', capture said "
                               "'%s' -- ability identity disagrees, NOT injecting",
                     g_head, g_count, s.slot_kind, s.slot_index, gon, s.gon);
            return false;
        }
    }

    // Build the decision. Zero first: a real GetChoice returns a fully
    // constructed TurnAction, and leaving the tail uninitialised would put
    // stale stack into the std::function slot at +0x78.
    TurnAction a{};
    memset(&a, 0, sizeof(a));
    a.type     = s.type;
    a.ability  = ability;
    a.target_x = s.tx; a.target_y = s.ty;
    a.dir_x    = s.dx; a.dir_y    = s.dy;
    a.actor    = nullptr;             // arrives null from a real brain too;
                                      // Character::DoAction fills it in later
    a.tail[0x30 - 0x28] = s.b30;
    a.tail[0x31 - 0x28] = s.b31;
    // +0x04 is left zero. It is uninitialised padding in the real struct, so
    // any value is as correct as any other -- but zero is at least stable.
    memcpy(out, &a, sizeof(a));

    g_outstanding = true;
    InterlockedIncrement(&g_injected);
    return true;
}

void replay_on_applied(const void* applied, const void* actor) {
    if (!g_active || !applied) return;

    TurnAction a{};
    if (!mem_read(applied, &a, sizeof(a))) return;

    // Types 6 and 7 are locally generated on every run; they are not in the
    // FIFO and must not advance it.
    if (a.type != TA_Ability && a.type != TA_EndTurn) return;

    Guard g;
    if (g_head >= g_count) return;
    const Step& s = g_steps[g_head];

    // Precedence must match the recorder's (mgmp_hooks.cpp, h_ApplyAction):
    // prefer the action's own actor, fall back to TurnControl's current one.
    // Inverting these two diverges only when an action carries an actor that
    // is not the turn's actor -- a Toss that throws a cat and then moves it --
    // and then reports a slot mismatch that is the instrument's, not the sim's.
    AbilitySlot slot = ability_slot_of(a.actor ? a.actor : actor, a.ability);
    bool match = (a.type == s.type) &&
                 (a.target_x == s.tx) && (a.target_y == s.ty) &&
                 (a.dir_x == s.dx) && (a.dir_y == s.dy) &&
                 (a.type != TA_Ability ||
                  (slot.kind == s.slot_kind && slot.index == s.slot_index));

    if (match) {
        InterlockedIncrement(&g_matched);
        ++g_head;
        g_outstanding = false;
        return;
    }

    // The result of the run. Reported at the action where it happened, with
    // both sides printed, because "the logs differ somewhere" is exactly the
    // answer run B exists to improve on.
    InterlockedIncrement(&g_diverged);
    log_line("REPLAY", "!! DIVERGED at step %u/%u (turn %u)", g_head, g_count, s.turn);
    log_line("REPLAY", "   expected: type=%u slot=%u:%u target=(%d,%d) dir=(%d,%d) gon=%s",
             s.type, s.slot_kind, s.slot_index, s.tx, s.ty, s.dx, s.dy,
             s.gon[0] ? s.gon : "-");
    log_line("REPLAY", "   applied : type=%u slot=%u:%u target=(%d,%d) dir=(%d,%d)",
             a.type, slot.kind, slot.index, a.target_x, a.target_y, a.dir_x, a.dir_y);

    // Advance anyway. Halting here would hide every later divergence, and the
    // first mismatch is rarely the only interesting one -- the desync HALT of
    // the real protocol belongs in phase 5, not in the instrument.
    ++g_head;
    g_outstanding = false;
}

void replay_stats(uint32_t* injected, uint32_t* matched,
                  uint32_t* diverged, uint32_t* remaining) {
    if (injected)  *injected  = (uint32_t)g_injected;
    if (matched)   *matched   = (uint32_t)g_matched;
    if (diverged)  *diverged  = (uint32_t)g_diverged;
    if (remaining) *remaining = (g_head < g_count) ? (g_count - g_head) : 0;
}

} // namespace mgmp
