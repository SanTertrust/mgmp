// mgmp_aim -- see mgmp_aim.h for why this is so small.
#include "mgmp_aim.h"

#include "mgmp_ability.h"
#include "mgmp_addresses.h"
#include "mgmp_hooks.h"
#include "mgmp_resolve.h"
#include "mgmp_config.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_rng.h"
#include "mgmp_tuning.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// iVec2D is 8 bytes -- {int x, int y} -- confirmed empirically and recorded in
// CLAUDE.md. Passed BY VALUE in a register, so it is declared as one here
// rather than as a pointer: getting that wrong would hand the callee an address
// where it expects two ints.
struct iVec2D { int32_t x, y; };
static_assert(sizeof(iVec2D) == 8, "iVec2D must be 8 bytes to travel in a register");

typedef void (__fastcall* fn_draw_aoe)(void* brain, void* ability,
                                       iVec2D target, iVec2D dir, int layer);

// sub_140138A10 -- the tiles a player is really looking at: the reachable set
// under a Move, the in-range set under an attack.
//
// IT IS NOT A DRAW, and everything about how it is called here follows from
// that. It sets [obj+0x118] on a collected set of objects and calls
// sub_140151CE0, which on a flag change calls glaiel::apply_status on a real
// Character* and stores the resulting Status* on the object. The tiles are a
// side effect of those statuses.
//
// Called plainly on the peer that does NOT own the cat, that is a simulation
// mutation on one side only, thousands of times a battle, and it cost a run
// within the hour of being added: facing on cat 31 ended (0,-1) on the host and
// (-1,0) on the client with every hashed component agreeing, and the next AI
// decision diverged from an identical board -- Move (5,7)/attack (5,2) against
// Move (6,8)/attack (8,5). The RNG fence could not see it; it guards the
// stream, and this moved state.
//
// So it is called with THREE guards, and the middle one is the actual fix:
//   - the RNG fence, as before;
//   - `T_HighlightRefresh` swallows sub_140151CE0 for the duration, so the
//     status pathway does not run at all;
//   - `lockstep_state_fence_*` snapshots every cat's simulation state across
//     the call, puts facing back and reports anything else -- because
//     sub_140138A10 still writes [obj+0x118] itself and nobody has established
//     what reads it.
typedef void (__fastcall* fn_highlight)(void* brain, void* ability, int mode);

// The two halves of the game's own "where could I attack from there" preview.
// See the C_TacticsMove block in mgmp_addresses.h: the game gets those tiles by
// really moving the cat to the hovered square and moving it back.
typedef void (__fastcall* fn_move)(void* tobj, iVec2D tile, char a, char b);
typedef void (__fastcall* fn_recompute)(void* self, void* ability, char flag);

// Character -> TacticsObject, and the TacticsObject's current tile. The same
// two offsets the per-turn state hash walks, and validated the same way: the
// round trip Character+0x60 -> TacticsObject+0x98 must come back to us or the
// preview is skipped rather than guessed at.
constexpr uintptr_t kChar_TObj  = 0x60;
constexpr uintptr_t kTObj_Back  = 0x98;
constexpr uintptr_t kTObj_Tile  = 0x48;

// The three counters the game brackets its displacement with. Restoring them is
// not optional: +0xD10 is what the range computation spends, and leaving it one
// short would make the peer's preview -- and, far worse, the peer's own idea of
// what this cat can still do -- wrong for the rest of the turn.
constexpr uintptr_t kChar_Guard    = 0xEC0;
constexpr uintptr_t kChar_MovePts  = 0xD10;
constexpr uintptr_t kChar_MovesUsed = 0xD3C;

// 0FFFFFFFFh at both of the game's own preview sites.
constexpr int kHighlightMode = -1;

// The layer the game itself passes at 0x14013777F. Not a guess and not a knob:
// a different layer would sort the peer's tiles against the local ones
// differently, and "the same as the local preview" is the whole point.
constexpr int kAoeLayer = 0x13;

// THE SELECTION, NOT THE DECISION.
//
// The first version of this module read Brain+0x220/0x228/0x230/0x238 -- the
// cached pending choice -- because that is what Brain::UpdateDecision draws
// from at 0x140137798. That was the wrong source, and the symptom was exact:
// the host sent 121 aims and the client drew none, because +0x220 only reads 2
// in the sliver of time between a decision being COMMITTED and it being
// applied. What a player looks at is the state before that: an ability picked,
// a mouse moving over the board, nothing committed at all.
//
// That state lives on the PlayerBrain and is drawn by its own update,
// sub_140775EB0 (vtable 0x140F82D78; DbgPlayerBrain and MountBrain share the
// implementation). The fields are read off its two DrawAbilityAOE sites,
// 0x14077659C and 0x1407771A4, where `this` is r14:
//     mov r9,  [r14+360h]     ; direction
//     mov r8,  [r14+358h]     ; target tile
//     mov rdx, [r14+3D8h]     ; the ability the player has selected
// and off the sub_140138A10 calls one instruction above each of them.
//
// Only ever read through the human-cat gate below, so the object really is a
// PlayerBrain when these offsets are touched.
constexpr uintptr_t kPBrain_Ability = 0x3D8;
constexpr uintptr_t kPBrain_Target  = 0x358;   // iVec2D
constexpr uintptr_t kPBrain_Dir     = 0x360;   // iVec2D

// On change, and never faster than this. The same numbers mgmp_cursor uses, and
// for the same reason: an aim moves as fast as a mouse, and a preview that
// arrives 50 ms late is a preview that arrived.
constexpr uint32_t kMinGapMs   = 50;
constexpr uint32_t kHeartbeatMs = 500;

struct Held {
    bool     have  = false;
    uint64_t battle_id = 0;
    uint8_t  cat   = 0;
    uint8_t  slot_kind = 0, slot_index = 0;
    iVec2D   target{}, dir{};
    char     gon[64] = {};
};

struct State {
    uintptr_t base = 0;
    bool      resolved = false;
    fn_draw_aoe   draw       = nullptr;
    fn_highlight  highlight  = nullptr;   // null = tiles off, the aim still draws
    fn_move       move       = nullptr;   // null = no attack range under a Move
    fn_recompute  recompute  = nullptr;   // both or neither -- see aim_set_base

    bool suppress  = false;   // raised only for the duration of one highlight call
    bool on        = false;
    bool announced = false;

    Held     held;                 // the peer's aim, waiting to be drawn

    // What we last put on the wire, so an unchanged aim costs nothing.
    bool     sent_active = false;
    uint8_t  sent_cat = 0, sent_kind = 0, sent_index = 0;
    iVec2D   sent_target{}, sent_dir{};
    uint32_t sent_at = 0;

    uint32_t sent = 0, drawn = 0, refused_name = 0, hilite = 0;
    uint32_t reach = 0, reach_lost = 0;
    bool     said_refuse = false;
    bool     said_draw   = false;
    bool     said_lost   = false;
};

State g;

// The call into the game, isolated the way every other one in this project is:
// no C++ objects in scope, __try around it, false instead of an exception.
// ONE call, and see the note at the top of this file for the one that was
// removed. This is the only entry point here that has ever been shown to build
// UI pieces and nothing else.
bool safe_draw(void* brain, void* ability, iVec2D t, iVec2D d, int layer) {
    __try { g.draw(brain, ability, t, d, layer); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Draw with the simulation stream pinned.
//
// The fence pass found exactly one RNG site reachable from DrawAbilityAOE
// (ResolveKnockbackDirection's `random` case) and exactly one shipped ability
// that can reach it, which cannot be selected or hovered. That is evidence, not
// a proof -- the reverse-reachability walk was capped and this codebase
// dispatches through vtables and std::function -- and this call happens on ONE
// peer, which is the shape a divergence takes. So the stream is saved and put
// back rather than argued about. Four qwords, once a frame, on the presentation
// side of the frame.
//
// ---------------------------------------------------------------------------
// The highlight, with the suppressor up and the whole roster fenced.
//
// The order matters and is the opposite of the intuitive one: the state fence
// opens FIRST and closes LAST, so it also covers anything the suppressor let
// through. The suppressor is raised as tightly as possible around the call
// itself, because while it is up the game's own highlight would be swallowed
// too if it ran -- it cannot, we are inside our own call on the same thread,
// but the window is kept to one call anyway.
void bump(void* self, uintptr_t off, int32_t by) {
    int32_t v = 0;
    if (!mem_read((const uint8_t*)self + off, &v, sizeof(v))) return;
    v += by;
    mem_write((uint8_t*)self + off, &v, sizeof(v));
}

// THE ATTACK RANGE UNDER A MOVE -- the game displaces the cat and puts it back.
//
// See the C_TacticsMove block in mgmp_addresses.h for the sequence this
// reproduces and for why it is a different shape of risk from the highlight.
// One substitution: where the game calls the attack ability's own vtable slot
// 0xA0 and then hand-builds the tile pieces, this calls the highlight with the
// attack ability, which computes the same set from wherever the cat is standing
// and draws it. Same tiles, and no reimplementation of the game's string and
// colour plumbing.
//
// Everything is best-effort and nothing here is allowed to leave the cat
// displaced: the unwind runs in its own __try so that a fault inside the
// preview still moves the cat home. The caller's state fence is what says
// whether that worked.
void attack_range_under_move(void* brain, void* self, iVec2D target) {
    if (!g.move || !g.recompute || !g.highlight) return;

    AbilitySlot as{}; as.kind = SLOT_ATTACK; as.index = 0;
    void* attack = ability_from_slot(self, as);
    if (!attack) return;   // a cat with no attack slot has no range to show

    void* tobj = nullptr;
    if (!mem_read((const uint8_t*)self + kChar_TObj, &tobj, sizeof(tobj)) || !tobj)
        return;
    // The same round trip the state hash requires before it will hash a cat.
    // A TacticsObject that does not point back at this Character is not the one
    // we may move.
    void* back = nullptr;
    if (!mem_read((const uint8_t*)tobj + kTObj_Back, &back, sizeof(back)) || back != self)
        return;

    iVec2D home{};
    if (!mem_read((const uint8_t*)tobj + kTObj_Tile, &home, sizeof(home))) return;
    if (home.x == target.x && home.y == target.y) return;   // nothing to preview

    __try {
        bump(self, kChar_Guard, +1);
        g.move(tobj, target, 1, 0);
        bump(self, kChar_MovePts, -1);
        bump(self, kChar_MovesUsed, +1);
        g.recompute(self, nullptr, 1);
        g.suppress = true;
        g.highlight(brain, attack, kHighlightMode);
        g.suppress = false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { g.suppress = false; }

    __try {
        bump(self, kChar_MovePts, +1);
        bump(self, kChar_MovesUsed, -1);
        g.move(tobj, home, 1, 0);
        g.recompute(self, nullptr, 1);
        bump(self, kChar_Guard, -1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    iVec2D now{};
    if (mem_read((const uint8_t*)tobj + kTObj_Tile, &now, sizeof(now)) &&
        (now.x != home.x || now.y != home.y)) {
        ++g.reach_lost;
        if (!g.said_lost) {
            g.said_lost = true;
            log_line_lvl(LogLevel::Error, "AIM",
                         "!! the move preview left cat %u at (%d,%d) instead of "
                         "(%d,%d) -- the round trip did NOT complete and this "
                         "peer's board now differs. The attack range under a "
                         "Move is off for the rest of this session.",
                         g.held.cat, now.x, now.y, home.x, home.y);
        }
        // One failure is enough. The feature is cosmetic; a displaced cat is not.
        g.move = nullptr;
        return;
    }
    ++g.reach;
}

// The peer's preview, with the suppressor up and the whole roster fenced.
//
// The order matters and is the opposite of the intuitive one: the state fence
// opens FIRST and closes LAST, so it also covers anything the suppressor let
// through. The suppressor is raised as tightly as possible around each call,
// because while it is up the game's own highlight would be swallowed too if it
// ran -- it cannot, we are inside our own call on the same thread, but the
// window is kept to one call anyway.
//
// ONE fence spans both the highlight and the move round trip, because the fence
// has a single slot and because a trip that fails between them is exactly what
// it is here to catch.
void highlight_fenced(void* brain, void* self, void* ability, uint8_t slot_kind,
                      iVec2D target) {
    if (!g.highlight) return;

    lockstep_state_fence_begin();

    uint64_t* s = rng_global_stream();
    uint64_t  before[4] = {};
    const bool pinned = (s != nullptr);
    if (pinned) memcpy(before, s, sizeof(before));

    g.suppress = true;
    __try { g.highlight(brain, ability, kHighlightMode); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    g.suppress = false;

    // Only under a Move, because only a Move relocates the cat -- for every
    // other ability the game shows the range from where the cat already is,
    // which the call above has already drawn.
    if (slot_kind == SLOT_MOVE) attack_range_under_move(brain, self, target);

    if (pinned && memcmp(before, s, sizeof(before)) != 0) {
        memcpy(s, before, sizeof(before));
        log_line("AIM", "!! the ability highlight moved the simulation stream and "
                        "it was put back -- '%s'", g.held.gon);
    }

    lockstep_state_fence_end("the peer's ability highlight");
    ++g.hilite;
}

// The AOE shape, with the simulation stream pinned -- see the note above.
bool draw_fenced(void* brain, void* ability, iVec2D t, iVec2D d, int layer) {
    uint64_t* s = rng_global_stream();
    uint64_t  before[4] = {};
    const bool pinned = (s != nullptr);
    if (pinned) memcpy(before, s, sizeof(before));

    const bool ok = safe_draw(brain, ability, t, d, layer);

    if (pinned && memcmp(before, s, sizeof(before)) != 0) {
        memcpy(s, before, sizeof(before));
        // Worth a line every time it happens, not once: this would mean the
        // peer's preview reached the simulation stream, which is the one thing
        // this module promised it could not do.
        log_line("AIM", "!! DrawAbilityAOE moved the simulation stream and it was "
                        "put back -- '%s' reaches RNG, which the fence pass said "
                        "no selectable ability does", g.held.gon);
    }
    return ok;
}

// A null ability is "nothing selected", which is the ordinary resting state and
// not an error -- the game's own sites pass +0x3D8 straight to a routine whose
// first instruction null-checks it.
bool read_brain_aim(const void* brain, void*& ability,
                    iVec2D& target, iVec2D& dir) {
    ability = nullptr;
    if (!mem_read((const uint8_t*)brain + kPBrain_Ability, &ability, sizeof(ability)))
        return false;
    if (!ability) return true;
    if (!mem_read((const uint8_t*)brain + kPBrain_Target, &target, sizeof(target)))
        return false;
    if (!mem_read((const uint8_t*)brain + kPBrain_Dir, &dir, sizeof(dir)))
        return false;
    return true;
}

void publish(void* brain, uint8_t cat) {
    void* ability = nullptr;
    iVec2D target{}, dir{};
    if (!read_brain_aim(brain, ability, target, dir)) return;

    const bool active = (ability != nullptr);

    AimMsg m{};
    m.battle_id = lockstep_battle_id();
    m.cat       = cat;
    m.active    = active ? 1 : 0;

    if (active) {
        void* self = nullptr;
        if (!mem_read((const uint8_t*)brain + kBrain_Character, &self, sizeof(self)) || !self)
            return;
        const AbilitySlot slot = ability_slot_of(self, ability);
        // An ability the actor does not own in any slot cannot be named on the
        // wire, and guessing is worse than not drawing -- the receiver would
        // resolve some OTHER ability and preview the wrong tiles. Publish the
        // stop instead, so the peer's stale preview clears.
        if (slot.kind == SLOT_UNKNOWN) {
            m.active = 0;
        } else {
            m.slot_kind  = slot.kind;
            m.slot_index = slot.index;
            m.tx = target.x; m.ty = target.y;
            m.dx = dir.x;    m.dy = dir.y;
            ability_gon_name(ability, m.gon, sizeof(m.gon));
        }
    }

    const uint32_t now = GetTickCount();
    const bool changed =
        (m.active != (g.sent_active ? 1 : 0)) ||
        m.cat != g.sent_cat || m.slot_kind != g.sent_kind ||
        m.slot_index != g.sent_index ||
        m.tx != g.sent_target.x || m.ty != g.sent_target.y ||
        m.dx != g.sent_dir.x    || m.dy != g.sent_dir.y;

    const uint32_t since = now - g.sent_at;
    if (!changed && since < kHeartbeatMs) return;
    if (changed && since < kMinGapMs)     return;

    if (!net_send_aim(m)) return;
    ++g.sent;
    g.sent_active   = (m.active != 0);
    g.sent_cat      = m.cat;
    g.sent_kind     = m.slot_kind;
    g.sent_index    = m.slot_index;
    g.sent_target.x = m.tx; g.sent_target.y = m.ty;
    g.sent_dir.x    = m.dx; g.sent_dir.y    = m.dy;
    g.sent_at       = now;
}

void draw_held(void* brain, uint8_t cat) {
    if (!g.held.have || g.held.cat != cat) return;
    // A held aim from another battle draws nothing. Cheap, and it is the same
    // rule every other battle-scoped message follows.
    if (g.held.battle_id != lockstep_battle_id()) return;

    void* self = nullptr;
    if (!mem_read((const uint8_t*)brain + kBrain_Character, &self, sizeof(self)) || !self)
        return;

    AbilitySlot slot{};
    slot.kind  = g.held.slot_kind;
    slot.index = g.held.slot_index;
    void* ability = ability_from_slot(self, slot);
    if (!ability) return;

    // Resolve by slot, validate by name -- the same cross-check ACTION uses,
    // and the one that caught the equipment bug. Here a disagreement is not a
    // desync signal (nothing about an aim is simulation state), so it warns
    // once and draws nothing rather than previewing the wrong ability.
    if (g.held.gon[0]) {
        char have[64] = {};
        if (ability_gon_name(ability, have, sizeof(have)) &&
            strcmp(have, g.held.gon) != 0) {
            ++g.refused_name;
            if (!g.said_refuse) {
                g.said_refuse = true;
                log_line("AIM", "!! cat %u slot %u:%u is '%s' here and '%s' on the "
                                "peer -- not drawing their aim. The two peers "
                                "disagree about what that cat can do.",
                         cat, slot.kind, slot.index, have, g.held.gon);
            }
            return;
        }
    }

    const iVec2D t{ g.held.target.x, g.held.target.y };
    const iVec2D d{ g.held.dir.x,    g.held.dir.y };
    if (!draw_fenced(brain, ability, t, d, kAoeLayer)) return;

    // The tiles the player is actually looking at: the reachable set under a
    // Move, the in-range set under an attack, and -- under a Move -- the attack
    // range from the square they are hovering. See highlight_fenced.
    highlight_fenced(brain, self, ability, g.held.slot_kind, t);

    ++g.drawn;
    if (!g.said_draw) {
        g.said_draw = true;
        log_line("AIM", "drawing the peer's aim: cat %u '%s' at (%d,%d) dir (%d,%d)",
                 cat, g.held.gon, t.x, t.y, d.x, d.y);
    }
}

} // namespace

void aim_set_base(uintptr_t base) {
    g.base = base;
    g.resolved = false;
    const uintptr_t addr = addr_of_call(C_DrawAbilityAOE);
    if (!addr) {
        log_line("AIM", "!! %s did not resolve by signature -- the "
                        "peer's aim preview is OFF", kCalls[C_DrawAbilityAOE].name);
        return;
    }
    g.draw = (fn_draw_aoe)addr;
    g.resolved = true;

    // Graded softer than the aim itself: without it the peer sees the shape
    // under the cursor but not the tiles the cat can reach, which is a smaller
    // loss than no preview at all.
    const uintptr_t hi = addr_of_call(C_AbilityHighlight);
    if (!hi) {
        log_line("AIM", "!! %s did not resolve by signature -- the peer's aim "
                        "will show its shape but not its reachable tiles",
                 kCalls[C_AbilityHighlight].name);
        return;
    }
    g.highlight = (fn_highlight)hi;

    // Softer again, and BOTH OR NEITHER: half of a displacement is a cat left
    // standing somewhere it never walked to.
    const uintptr_t mv = addr_of_call(C_TacticsMove);
    const uintptr_t rc = addr_of_call(C_RecomputeStats);
    if (!mv || !rc) {
        log_line("AIM", "!! %s did not resolve by signature -- the peer's Move "
                        "preview will show its reachable tiles but not the "
                        "attack range from the hovered square",
                 mv ? kCalls[C_RecomputeStats].name : kCalls[C_TacticsMove].name);
        return;
    }
    g.move      = (fn_move)mv;
    g.recompute = (fn_recompute)rc;
}

void aim_init() {
    const Config& c = config();
    const bool netted = _stricmp(c.net_role, "host") == 0 ||
                        _stricmp(c.net_role, "client") == 0;
    g.on = tune::kAimPreview && netted && g.resolved;
    if (!g.on || g.announced) return;
    g.announced = true;
    log_line("AIM", "armed -- the range and AOE tiles the peer is aiming at will be "
                    "drawn here, by the game's own DrawAbilityAOE");
}

void aim_shutdown() {
    if (!g.announced) return;
    log_line("AIM", "done: %u aim(s) sent, %u frame(s) drawn, %u highlight(s) with "
                    "the status refresh swallowed, %u move preview(s) round-tripped "
                    "(%u LOST) (the state fence caught %u cat change(s)), "
                    "%u refused on a name mismatch",
             g.sent, g.drawn, g.hilite, g.reach, g.reach_lost,
             lockstep_state_fence_hits(), g.refused_name);
}

void aim_on_message(const AimMsg& m) {
    if (!g.on) return;
    if (!m.active) { g.held.have = false; return; }

    g.held.have       = true;
    g.held.battle_id  = m.battle_id;
    g.held.cat        = m.cat;
    g.held.slot_kind  = m.slot_kind;
    g.held.slot_index = m.slot_index;
    g.held.target.x   = m.tx; g.held.target.y = m.ty;
    g.held.dir.x      = m.dx; g.held.dir.y    = m.dy;
    strncpy_s(g.held.gon, m.gon, _TRUNCATE);
}

void aim_on_update_decision(void* brain) {
    if (!g.on || !brain || !lockstep_in_battle()) return;

    void* self = nullptr;
    if (!mem_read((const uint8_t*)brain + kBrain_Character, &self, sizeof(self)) || !self)
        return;

    uint8_t cat = 0;
    bool    peer_owns = false;
    if (!lockstep_aim_subject(self, cat, peer_owns)) return;

    if (peer_owns) draw_held(brain, cat);
    else           publish(brain, cat);
}

uint32_t aim_sent()  { return g.sent; }
uint32_t aim_drawn() { return g.drawn; }

bool aim_highlight_suppressed() { return g.suppress; }

void aim_on_hooks_installed() {
    if (!g.highlight) return;
    if (hooks_is_live(T_HighlightRefresh)) return;

    g.highlight = nullptr;
    log_line_lvl(LogLevel::Warn, "AIM",
                 "!! the highlight suppressor (%s) is not live, so the peer's "
                 "reachable tiles are OFF -- calling the highlight without it "
                 "would apply statuses on a cat this peer does not own, which "
                 "is the mutation that cost a run on 2026-08-26",
                 kTargets[T_HighlightRefresh].symbol);
}

} // namespace mgmp
