// mgmp_lockstep.cpp -- see mgmp_lockstep.h for the design.

#include "mgmp_lockstep.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_barrier.h"
#include "mgmp_battleid.h"
#include "mgmp_hashring.h"
#include "mgmp_net.h"
#include "mgmp_catsync.h"
#include "mgmp_cursor.h"
#include "mgmp_overlay.h"
#include "mgmp_invsync.h"
#include "mgmp_aim.h"
#include "mgmp_nodehash.h"
#include "mgmp_runhist.h"
#include "mgmp_follow.h"
#include "mgmp_choice.h"
#include "mgmp_savefile.h"
#include "mgmp_leave.h"
#include "mgmp_session.h"
#include "mgmp_ability.h"
#include "mgmp_turnaction.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_mem.h"
#include "mgmp_split.h"
#include "mgmp_rtti.h"
#include "mgmp_rng.h"
#include "mgmp_log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>

namespace mgmp {
namespace {

// This counts ENTRIES IN THE BATTLE'S CHARACTER LIST, which is mostly scenery.
// Measured over two real battles: 45 NoBrain against 8 PlayerBrain, 6
// GenericBrain and 2 PatternBrain. A boss arena logged 45 entries rising to 46
// mid-fight, and on screen that was four cats and one boss -- the other forty
// were props. So this number tracks ARENA SIZE, not how many things can act,
// and sizing it against "a boss plus its summons" is the wrong model: a fully
// dressed 10x10 grid is already 100 tiles before a single combatant.
//
// It is therefore set to the hard protocol limit rather than to a guess with
// margin. Roster indices travel as uint8_t in ACTION and CONTROL and kNoCat is
// 0xFF, so 254 is the largest index that can exist on the wire; anything under
// that is an arbitrary line waiting to be crossed by a bigger room. The cost is
// about 2 KB of static arrays.
//
// 32 was the old value. Over it the snapshot bailed, which meant no roster, no
// control split, no actions on the wire and no hashes -- both peers quietly
// played their own boss fight and the session summary still said "0 desync(s)".
//
// ControlMsg::cats[32] is unrelated: it carries HUMAN cats only, of which there
// are four.
constexpr uint32_t kMaxCats    = 254;

// One BATTLE's worth of decisions, not one turn's.
//
// 64 was enough while the only thing this queue ever held was the handful of
// decisions in flight around a turn boundary. It is not enough now that a peer
// joining mid-fight is sent the whole battle's history at once: a 12-turn fight
// with four human cats replays far past 64, and pend_push drops the overflow --
// which would leave the joiner short of exactly the decisions it needs, having
// said so in one line that is easy to miss.
//
// Sized to match the catch-up log it has to be able to receive, so the two
// cannot disagree.
constexpr uint32_t kMaxSentLog = 512;
constexpr uint32_t kMaxPending = kMaxSentLog;
constexpr uint8_t  kNoCat      = 0xFF;

// Character list, reached from TurnControl. See the header for provenance.
constexpr uintptr_t kTC_Scene      = 0x18;
constexpr uintptr_t kScene_Sub     = 0x08;
constexpr uintptr_t kSub_Holder    = 0x20;
constexpr uintptr_t kHolder_List   = 0x1F90;
constexpr uintptr_t kList_Count    = 12;
constexpr uintptr_t kList_Data     = 16;

// Brain+0x38 is the owning Character (Hex-Rays types it as such in
// Brain::UpdateDecision); Character+0x68 is the reverse edge, the Brain*
// TurnControl::update reads to call UpdateDecision. TurnControl+0x60 is the
// decision queue's element count.
constexpr uintptr_t kBrain_Owner   = 0x38;
constexpr uintptr_t kChar_Brain    = 0x68;
constexpr uintptr_t kTC_QueueCount = 0x60;

// --- Character fields, for the state hash (recovered 2026-08-24) ------------
//
// Character+0x60 points at a TacticsObject and TacticsObject+0x98 points back
// at the Character. That pair is not a guess to be trusted: the snapshot walks
// it in BOTH directions per cat and refuses to hash a cat whose pointer does
// not round trip. Provenance, one independent reading each:
//
//   +0x60  TacticsObject*  AbilityHealthThreshold::late_update reads
//                          *(char+96)+96 -- TacticsObject's "removed" flag, the
//                          guard at the top of both TacticsObject::Move and
//                          ::ReceiveDamage. Character::get_standing_tile_elements
//                          reads *(char+96)+99, another TacticsObject bool.
//   +0x388 iVec2D facing   Character::Face's only durable write (this+113).
//   +0x4B0 int32  HP       Character::Die zeroes +0x4B0 as a QWORD -- i.e. HP
//                          and shield in one store; ReceiveDamage__inner_0
//                          touches it 21 times; EndTurn and RunAway read it as
//                          a dword.
//   +0x4B4 int32  shield   Character::GainShield writes this+301, and it is the
//                          same field AbilityHealthThreshold adds to HP when
//                          the ability's GON says `count_shield`. Two unrelated
//                          functions, one offset.
//   +0x4BC int32  max HP   the deval base passed for `threshold` /
//                          `threshold_min` / `threshold_max`, so a GON "50%"
//                          means 50% of this; recompute_stats reads it 4x.
//   +0x4C2 bool   dead     Character::Die's entry guard, set by Die and
//                          OnCorpsePop, read by BeginTurn, EndTurn, DoAction,
//                          RoundUpkeep, GainShield and update.
//
// TacticsObject+0x48 is the current tile: ::Move copies the old value to +0x50
// and writes the destination here, and ::ReceiveDamage passes it as the tile
// argument to the splash walk.
constexpr uintptr_t kChar_TObj     = 0x60;
constexpr uintptr_t kChar_Facing   = 0x388;
constexpr uintptr_t kChar_HP       = 0x4B0;
constexpr uintptr_t kChar_Shield   = 0x4B4;
constexpr uintptr_t kChar_MaxHP    = 0x4BC;
constexpr uintptr_t kChar_Dead     = 0x4C2;
constexpr uintptr_t kTObj_Tile     = 0x48;
constexpr uintptr_t kTObj_Owner    = 0x98;

// glaiel::Character::get_affecting_elements(Character*, ElementList* out).
// Returns `out`. See the kCalls entry for why this is worth calling: it is the
// value Character::receive_damage_passives branches on, and its tile half is
// the only damage input that lives on the GRID rather than on the character --
// so it is the one thing a per-character state hash structurally cannot see.
typedef void* (__fastcall *fn_affecting_elements)(const void* chr, uint32_t* out);

// --- per-cat simulation state ----------------------------------------------
//
// Packed deliberately: this is hashed byte-for-byte, so it must contain no
// padding and no pointer. A heap address in here would differ between two
// processes by construction and desync every single turn -- the exact way a
// state hash goes from "detects divergence" to "manufactures it".
#pragma pack(push, 1)
struct CatState {
    int32_t hp     = 0, shield = 0, maxhp = 0;
    int32_t tx     = 0, ty     = 0;
    int32_t fx     = 0, fy     = 0;
    uint32_t e0    = 0, e1     = 0;   // ElementList, or 0/0 if we could not read it
    uint8_t dead   = 0;
    uint8_t linked = 0;   // the TacticsObject round trip held for this cat
    uint8_t elems  = 0;   // the ElementList above is real, not a fallback zero
    uint8_t in_battle = 1; // still in the live character list; see read_cat_state
    // Whether the fields above were read at all. Only meaningful in the copy
    // build_hash keeps for the desync dump: a row that could not be read is a
    // fact worth sending, and a peer that reads a cat the other one cannot is
    // itself a difference the diff should name rather than skip over.
    uint8_t readable = 0;
};
#pragma pack(pop)

struct State {
    bool active = false;

    // Null unless the prologue verified against the pinned build. Every use is
    // null-checked; a build drift turns the element hash off rather than
    // calling into whatever moved there.
    fn_affecting_elements affecting_elements = nullptr;

    const void* cats[kMaxCats] = {};
    uint32_t    cat_count      = 0;
    bool        local_cat[kMaxCats] = {};   // this peer's input decides for it
    bool        human_cat[kMaxCats] = {};   // a human brain drives it at all
    bool        snapped        = false;
    const void* snapped_list   = nullptr;   // the vector object we snapshotted

    // --- what the last hash was actually taken over ----------------------
    //
    // Kept so that a desync dump reports the state that WAS hashed rather than
    // the state that happens to be live when the peer's hash lands. Those are
    // not the same instant: a mismatch is noticed either at our own boundary or
    // on arrival, and only the first of those is the moment the numbers were
    // read. Comparing a stale row against a fresh one invents differences.
    CatState hashed[kMaxCats];
    uint32_t hashed_count = 0;
    uint32_t hashed_turn  = 0;
    bool     hashed_valid = false;
    // One dump per divergence in each direction. After the first there is
    // nothing new to learn and the peer is halted anyway.
    bool     dump_sent = false;
    bool     dump_seen = false;

    ActionMsg pending[kMaxPending];
    uint32_t  pend_head = 0, pend_count = 0;

    bool     outstanding = false;   // a local decision is sent but not applied
    uint32_t turn        = 0;

    // Which battle. Turn numbering restarts at 0 in each one, so the turn index
    // alone identifies nothing and a message in flight across a battle boundary
    // would be taken for one about the battle we just entered. See the note
    // above ActionMsg in mgmp_proto.h.
    // The WIRE identity: the node seed both peers read out of MapNode+0x118.
    // Set by lockstep_enter_battle from the map layer, which is the only thing
    // that knows a node was entered. See mgmp_battleid.h.
    BattleTracker<> battles;

    // Every decision taken in the current battle by ANY peer -- ours as we send
    // them, the peer's as they arrive -- for replaying to a peer that joins
    // mid-fight. Cleared at every battle boundary, so it is bounded by one
    // battle rather than by the run.
    ActionMsg sent_log[kMaxSentLog];
    uint32_t  sent_log_n    = 0;
    bool      sent_log_full = false;

    // A LOCAL counter, kept only for the log lines and for "how many battles
    // has this peer played". It is deliberately never compared with a peer's:
    // that is what broke on reconnect.
    uint32_t epoch       = 0;
    uint32_t stale_drops = 0;      // messages dropped this battle as stale
    bool     said_stale  = false;  // ...and whether we have said so yet

    // Whether the cat last polled for a decision is one of ours. Cosmetic only
    // -- it drives the cursor overlay's "whose turn is it" fade and nothing
    // else -- but it lives here because this is the only place that already
    // resolves a Brain back to a roster index. False during an enemy or AI
    // turn, which is correct: that turn belongs to nobody, so no cursor lights.
    bool     local_actor = false;

    // The two directions a counterpart can arrive from; see mgmp_hashring.h.
    //
    // One held ring PER PEER, because with four players the peers are not in
    // lockstep with each other's frame rates any more than with ours: peer 1 may
    // be three turns ahead while peer 2 is one behind, and a single shared ring
    // would interleave their turn numbers and match the wrong pair. `my_hash`
    // stays single -- there is only one of us.
    HashRing<HashMsg> peer_hash[kMaxPeers];
    HashRing<HashMsg> my_hash;
    bool     peer_hash_full_warned[kMaxPeers] = {};

    bool     halted = false;
    LockstepStats stats;

    // --- the per-turn state trace (net_state_trace) ---
    //
    // The previous boundary's reading of every snapshot cat, so that this
    // boundary can report what MOVED rather than only what IS. Reset with the
    // roster, because a new battle's cat 3 is a different cat.
    CatState prev_state[kMaxCats]{};
    bool     have_prev = false;

    // --- mismatch bookkeeping when halting is off (net_desync_halt = 0) ---
    //
    // Without a halt a divergence can be reported on every remaining turn of
    // the battle, and 45 cats of table each time buries the one line that
    // matters. Dump the table for the first few, then stay terse -- and say
    // loudly when a later turn AGREES again, because that single line is the
    // whole reason the setting exists.
    uint32_t mismatches      = 0;   // this battle
    uint32_t first_mismatch  = 0;   // ...and the turn of the first one
    bool     diverged        = false;

    // Turn hashes that were actually COMPARED against a peer's and agreed,
    // counted for the whole session rather than per battle.
    //
    // Without it the shutdown summary said `0 desync(s)` whether the peers had
    // agreed on sixty turns or had never compared a single one -- CLAUDE.md's
    // rule 3, and the exact hole a reconnect left. It is also what decides
    // whether that line is quiet or a warning: a session that compared nothing
    // has to say so, because it is the reading a player takes for a pass.
    uint32_t agreements      = 0;

    uint32_t humans          = 0;   // cats a human brain drives, either peer's
    bool     state_hash_on   = false;
    bool     control_checked = false;
    // One slot per peer id, which is why ids are allocated lowest-free and stay
    // under kMaxPeers. Our own slot is filled in locally when we publish, so the
    // coverage check below can treat every player identically instead of
    // special-casing "us" -- with four players, "the peer" is not a thing.
    ControlMsg peer_control[kMaxPeers]{};
    bool       have_peer_control[kMaxPeers] = {};

    // --- the join barrier ---
    uint32_t barrier_waits     = 0;      // GetChoice polls parked in this battle
    bool     barrier_said_open = false;  // ...and whether we have said it lifted

    CRITICAL_SECTION cs;
    bool cs_ready = false;
};

State g;

struct Guard {
    Guard()  { if (g.cs_ready) EnterCriticalSection(&g.cs); }
    ~Guard() { if (g.cs_ready) LeaveCriticalSection(&g.cs); }
};

// Defined below, next to the desync compare; declared here because the control
// check runs earlier in the file and halting is the right answer to a split
// that does not add up.
void halt(const char* why);

// Defined below with the rest of the split handling; the snapshot calls it as
// its last act, because the peer's CONTROL may already be sitting in the queue
// by the time this side finally has a roster to check it against.
void verify_control();

// --- FNV-1a, the same hash the protocol note specifies ----------------------
uint64_t fnv1a(const void* p, size_t n, uint64_t h = 1469598103934665603ULL) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}


// Wildly-out-of-range values mean the offsets are wrong on this build, not that
// a cat is unusual. Bounds are loose on purpose: they are here to catch reading
// a pointer or a float as an int, not to police game balance.
bool plausible(const CatState& c) {
    if (c.maxhp <= 0 || c.maxhp > 100000) return false;
    if (c.hp < -100000 || c.hp > 100000)  return false;
    if (c.shield < -100000 || c.shield > 100000) return false;
    if (c.tx < -4096 || c.tx > 4096 || c.ty < -4096 || c.ty > 4096) return false;
    return true;
}

// Kept separate from read_cat_state purely so the __try/__except lives in a
// function with nothing but PODs in it -- MSVC refuses SEH in a frame that
// needs C++ unwinding, and this is the only place in the mod that calls into
// the game from the hash path.
bool read_affecting_elements(const void* chr, uint32_t out[2]) {
    out[0] = out[1] = 0;
    if (!g.affecting_elements || !chr) return false;
    __try {
        g.affecting_elements(chr, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = out[1] = 0;
        return false;
    }
}

// `in_battle` is whether this character is still in the live character list.
//
// It gates the one thing here that is a CALL rather than a read. A character
// the battle has dropped is parked off the board at the (-5000,-5000) sentinel,
// and get_affecting_elements feeds that raw tile straight into a grid walk
// (sub_14082CB30) with no bounds check -- so the result is whatever the heap
// happens to hold. Measured on the same boss fight twice: the host read
// 0x00000400 and the client faulted, then on the rerun the two swapped. Which
// peer "wins" is a coin flip, which is exactly what a state hash must never
// contain.
bool read_cat_state(const void* chr, CatState& out, bool in_battle = true) {
    out = CatState{};
    out.in_battle = in_battle ? 1 : 0;
    if (!chr) return false;

    const uint8_t* c = (const uint8_t*)chr;
    bool ok = mem_read(c + kChar_HP,     &out.hp,     sizeof(out.hp))
           && mem_read(c + kChar_Shield, &out.shield, sizeof(out.shield))
           && mem_read(c + kChar_MaxHP,  &out.maxhp,  sizeof(out.maxhp))
           && mem_read(c + kChar_Dead,   &out.dead,   sizeof(out.dead))
           && mem_read(c + kChar_Facing,     &out.fx, sizeof(out.fx))
           && mem_read(c + kChar_Facing + 4, &out.fy, sizeof(out.fy));
    if (!ok) return false;

    // The tile lives on the TacticsObject, and we only trust it if the object
    // points back at this Character. A cat that fails this still gets hashed --
    // with linked = 0 and a zero tile -- so that a peer where the link holds and
    // a peer where it does not produce different hashes rather than silently
    // agreeing on less state than they think.
    const void* tobj = nullptr;
    if (mem_read(c + kChar_TObj, &tobj, sizeof(tobj)) && tobj) {
        const void* back = nullptr;
        if (mem_read((const uint8_t*)tobj + kTObj_Owner, &back, sizeof(back)) && back == chr) {
            if (mem_read((const uint8_t*)tobj + kTObj_Tile,     &out.tx, sizeof(out.tx)) &&
                mem_read((const uint8_t*)tobj + kTObj_Tile + 4, &out.ty, sizeof(out.ty)))
                out.linked = 1;
        }
    }

    // The elements affecting this cat. Unlike everything above, this is a CALL
    // into the game rather than a read, so it gets its own guard: the pointer is
    // null unless the prologue matched, and the call is wrapped because a
    // Character with a torn equipment list would fault inside the callee where
    // mem_read cannot help us. A cat that fails leaves elems = 0, which hashes
    // differently from a cat that succeeded and read zero -- the same
    // distinction `linked` makes, and for the same reason.
    uint32_t el[2] = { 0, 0 };
    if (in_battle && read_affecting_elements(chr, el)) {
        out.e0 = el[0];
        out.e1 = el[1];
        out.elems = 1;
    }
    return true;
}

// Dumps every cat's state to the log. Called on a state-only hash mismatch,
// because the hash says only THAT the peers disagree. Two logs each holding
// this table, diffed, say WHICH cat and which field -- which is exactly how the
// one suspected desync so far was settled, by diffing the raw traces rather
// than by staring at hashes.
// Membership, not length.
//
// A turn that APPENDS a summon and REMOVES something else leaves the count
// unchanged, so comparing lengths reports "nothing was summoned or removed"
// while both things happened. That is not hypothetical: a RatBomb boss fight
// does it, and the compensating pair hid a real roster change behind a
// reassuring log line while a snapshot entry that had left the battle went on
// being hashed.
//
// So compare by POINTER. `still_in[i]` answers "is snapshot cat i still in the
// live list", and `appeared` counts live entries that are not in the snapshot.
//
// O(n^2) over at most kMaxCats entries, once per turn boundary -- far cheaper
// than the hash it sits beside.
bool snapshot_membership(const void* list, uint32_t& live,
                         bool still_in[kMaxCats], uint32_t& appeared) {
    live = appeared = 0;
    for (uint32_t i = 0; i < kMaxCats; ++i) still_in[i] = false;

    const void* data = nullptr;
    if (!list ||
        !mem_read((const uint8_t*)list + kList_Count, &live, sizeof(live)) ||
        !mem_read((const uint8_t*)list + kList_Data,  &data, sizeof(data)) || !data)
        return false;

    const uint32_t n = live < kMaxCats ? live : kMaxCats;
    const void* cur[kMaxCats] = {};
    for (uint32_t i = 0; i < n; ++i)
        if (!mem_read((const uint8_t*)data + i * sizeof(void*), &cur[i], sizeof(cur[i])))
            return false;

    for (uint32_t i = 0; i < n; ++i) {
        bool found = false;
        for (uint32_t j = 0; j < g.cat_count && !found; ++j) {
            if (cur[i] == g.cats[j]) { still_in[j] = true; found = true; }
        }
        if (!found) ++appeared;
    }
    return true;
}

uint32_t snapshot_departed(const bool still_in[kMaxCats]) {
    uint32_t left = 0;
    for (uint32_t j = 0; j < g.cat_count; ++j) if (!still_in[j]) ++left;
    return left;
}

void dump_cat_states(const char* why) {
    log_line("LOCKSTEP", "cat state table (%s):", why);

    // Worked out up front so each row can say whether that cat is still in the
    // battle at all. A departed entry is the one case where the fields below
    // are not trustworthy: our snapshot pointer outlives its membership.
    uint32_t live_now = 0, appeared_now = 0;
    bool still_in[kMaxCats];
    const bool have_membership =
        snapshot_membership(g.snapped_list, live_now, still_in, appeared_now);

    for (uint32_t i = 0; i < g.cat_count; ++i) {
        CatState st{};
        const bool present = !have_membership || still_in[i];
        if (!read_cat_state(g.cats[i], st, present)) {
            log_line("LOCKSTEP", "  cat %2u  <unreadable>", i);
            continue;
        }
        log_line("LOCKSTEP", "  cat %2u  hp=%d/%d shield=%d tile=(%d,%d) face=(%d,%d) elem=%08X:%08X%s%s%s%s",
                 i, st.hp, st.maxhp, st.shield, st.tx, st.ty, st.fx, st.fy,
                 st.e0, st.e1,
                 st.dead ? " DEAD" : "", st.linked ? "" : " [unlinked]",
                 st.elems ? "" : " [no-elem]",
                 (have_membership && !still_in[i]) ? " [GONE -- no longer in the live list]" : "");
    }

    // Everything the snapshot does NOT cover: summons, and anything else the
    // battle appended to the character list after it started.
    //
    // These are invisible to the state hash by design -- the snapshot is frozen
    // at battle start so that a summon cannot renumber every cat mid-fight --
    // and each peer's own AI drives its own copies. That is fine while both
    // peers summon the same things at the same turns, and NOTHING CHECKED IT.
    //
    // It matters because a type-6 reaction broadcast walks the live character
    // list, not our snapshot. If the tails differ, a reaction hits a different
    // set of targets and a hashed cat loses a different amount of HP -- with no
    // RNG difference at all, because nothing rolled. That is exactly the shape
    // of the halt this was added for: 45 identical cats and cat 38 two HP
    // apart, in a turn whose only event was a type-6.
    //
    // Printed rather than hashed, for the same reason passive counts are: a
    // summon appearing a frame apart on two peers is not a desync, and a hash
    // would make it one.
    const void* list = g.snapped_list;
    uint32_t    live = 0;
    const void* data = nullptr;
    if (!list ||
        !mem_read((const uint8_t*)list + kList_Count, &live, sizeof(live)) ||
        !mem_read((const uint8_t*)list + kList_Data,  &data, sizeof(data)) || !data) {
        log_line("LOCKSTEP", "  (could not read the live character list)");
        return;
    }

    const uint32_t departed = have_membership ? snapshot_departed(still_in) : 0;

    if (have_membership && appeared_now == 0 && departed == 0) {
        log_line("LOCKSTEP", "  live list is %u and every snapshot entry is still in"
                             " it -- nothing was summoned or removed", live);
        return;
    }

    if (have_membership && (appeared_now != 0 || departed != 0)) {
        log_line("LOCKSTEP", "  ROSTER CHANGED: %u entr(ies) appeared after battle"
                             " start and %u snapshot entr(ies) left the live list"
                             " (live %u, snapshot %u). COMPARE BOTH NUMBERS WITH THE"
                             " PEER'S FIRST: if they differ the divergence is here"
                             " and not in the %u cats above.",
                 appeared_now, departed, live, g.cat_count, g.cat_count);
    }

    if (!have_membership) {
        log_line("LOCKSTEP", "  live list is %u, snapshot was %u (membership could not"
                             " be read -- counts only)", live, g.cat_count);
    }

    // The entries the battle ADDED, found by membership rather than by index.
    // Walking from g.cat_count upward only works while the tail is pure growth:
    // an append paired with a removal leaves the count equal and puts the
    // newcomer somewhere in the middle, where an index scan never looks.
    for (uint32_t i = 0; i < live && i < kMaxCats; ++i) {
        const void* ch = nullptr;
        if (!mem_read((const uint8_t*)data + i * sizeof(void*), &ch, sizeof(ch)) || !ch)
            continue;

        bool in_snapshot = false;
        for (uint32_t j = 0; j < g.cat_count && !in_snapshot; ++j)
            in_snapshot = (ch == g.cats[j]);
        if (in_snapshot) continue;
        char cls[96];
        strcpy_s(cls, "?");
        const void* brain = nullptr;
        if (mem_read((const uint8_t*)ch + kChar_Brain, &brain, sizeof(brain)) && brain)
            rtti_class_name(brain, cls, sizeof(cls));

        CatState st{};
        if (!read_cat_state(ch, st)) {
            log_line("LOCKSTEP", "  +%2u  <unreadable>  brain=%s", i, cls);
            continue;
        }
        log_line("LOCKSTEP", "  +%2u  hp=%d/%d shield=%d tile=(%d,%d) brain=%s%s",
                 i, st.hp, st.maxhp, st.shield, st.tx, st.ty, cls,
                 st.dead ? " DEAD" : "");
    }
}

// --- the per-turn state trace (net_state_trace) -----------------------------
//
// One line per cat whose state moved since the previous turn boundary.
//
// This exists because the hash and the mismatch dump between them answer only
// two of the three questions a state-only desync raises. The hash says THAT the
// peers disagree; the dump says WHICH FIELD, at the moment it was noticed. What
// neither says is WHEN that field last moved -- and for a difference of a
// couple of HP that is the question the diagnosis turns on:
//
//   * both peers show `cat 38 hp 36->33`, on different turns
//         -> the same effect landed on opposite sides of a turn boundary.
//            A timing artefact of the kind facing already produces; the fix is
//            where the boundary is taken, not what the hash covers.
//   * one peer shows `hp 36->33` and the other `hp 36->31`
//         -> the two simulations really did compute different damage, and the
//            RNG hash agreeing means nothing rolled to cause it.
//
// Diff the two peers' delta lines and both readings are there without another
// run.
//
// Facing USED to be excluded here as known noise, because the aim preview
// rewrote it every frame under a wall-clock gate. That is no longer true --
// lockstep_preview_facing_* puts the preview's write back -- and the exclusion
// turned out to be expensive: facing decides backstab damage, so a divergence
// in it surfaced as three missing HP on turn 49 of a fight whose first 48 turns
// hashed identically. It is traced now.

// Append "<space>" + one formatted field to a fixed buffer, carrying the write
// position in `n`. Truncation is silent and bounded, which is the right answer
// for a diagnostic line: a clipped delta is still a delta, and a cat whose
// every field moved at once is not the interesting case.
void appendf(char* buf, size_t cap, int& n, const char* fmt, ...) {
    if (n < 0 || (size_t)n >= cap - 1) return;
    if (n > 0) { buf[n++] = ' '; buf[n] = 0; }
    va_list ap;
    va_start(ap, fmt);
    int w = _vsnprintf_s(buf + n, cap - n, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (w > 0) n += w;
    else       n = (int)(cap - 1);   // truncated: stop appending
}

void trace_state_deltas() {
    if (!tune::kStateTrace || !g.snapped) return;
    // Gated on the same evidence the hash is. With state_hash_on false at least
    // one cat failed its link or range check, so every reading here is heap luck
    // -- and a trace of heap luck is worse than silence: it prints movement that
    // never happened, in the one place a reader is looking for movement that
    // did. Nothing is lost, either: a zero state hash cannot mismatch.
    if (!g.state_hash_on) return;

    uint32_t live = 0, appeared = 0;
    bool still_in[kMaxCats];
    const bool have_membership =
        snapshot_membership(g.snapped_list, live, still_in, appeared);

    for (uint32_t i = 0; i < g.cat_count; ++i) {
        const bool present = !have_membership || still_in[i];

        CatState now{};
        // A cat that cannot be read at all gets a default-constructed entry, so
        // "became unreadable" shows up as a change like any other rather than
        // silently freezing its last known values.
        read_cat_state(g.cats[i], now, present);

        const CatState& was = g.prev_state[i];
        if (!g.have_prev) { g.prev_state[i] = now; continue; }

        // Field by field, and only the fields the HASH covers -- a trace that
        // reports movement the hash cannot see would send the next reader
        // chasing a difference that never mattered.
        char what[256];
        int  n = 0;
        what[0] = 0;

        if (now.hp     != was.hp)     appendf(what, sizeof(what), n, "hp %d->%d", was.hp, now.hp);
        if (now.shield != was.shield) appendf(what, sizeof(what), n, "shield %d->%d", was.shield, now.shield);
        if (now.maxhp  != was.maxhp)  appendf(what, sizeof(what), n, "maxhp %d->%d", was.maxhp, now.maxhp);
        if (now.tx != was.tx || now.ty != was.ty)
            appendf(what, sizeof(what), n, "tile (%d,%d)->(%d,%d)", was.tx, was.ty, now.tx, now.ty);
        if (now.dead   != was.dead)   appendf(what, sizeof(what), n, "dead %u->%u", was.dead, now.dead);
        if (now.linked != was.linked) appendf(what, sizeof(what), n, "linked %u->%u", was.linked, now.linked);
        if (now.e0 != was.e0 || now.e1 != was.e1)
            appendf(what, sizeof(what), n, "elem %08X:%08X->%08X:%08X", was.e0, was.e1, now.e0, now.e1);
        if (now.elems  != was.elems)  appendf(what, sizeof(what), n, "elem-read %u->%u", was.elems, now.elems);
        if (now.in_battle != was.in_battle)
            appendf(what, sizeof(what), n, "in-battle %u->%u", was.in_battle, now.in_battle);
        // Facing is traced even though the hash does not cover it -- the one
        // exception to the "only hashed fields" rule above, and it is earned.
        // The backstab test reads facing, so it decides damage; the hash cannot
        // cover it because presentation also writes it (CombatAnimation::update
        // turns a cat mid-animation, which is in flight at different points on
        // two machines). Trace-only is the honest middle: a facing difference
        // shows up in a log diff instead of surfacing as three missing HP forty
        // turns later, which is exactly how it surfaced on 2026-08-26.
        if (now.fx != was.fx || now.fy != was.fy)
            appendf(what, sizeof(what), n, "face (%d,%d)->(%d,%d)",
                    was.fx, was.fy, now.fx, now.fy);

        if (n > 0)
            log_line("LOCKSTEP", "turn %u delta: cat %u %s", g.turn, i, what);

        g.prev_state[i] = now;
    }
    g.have_prev = true;
}

// --- cat identity -----------------------------------------------------------

uint8_t cat_index_of(const void* character) {
    if (!character) return kNoCat;
    for (uint32_t i = 0; i < g.cat_count; ++i)
        if (g.cats[i] == character) return (uint8_t)i;
    return kNoCat;
}

// Snapshot the battle's character list. Called once per battle, at the first
// turn boundary -- by then Level::init has run and every starting cat exists.
// Resolve TurnControl -> the battle's character list. Split out from the
// snapshot because it is also the new-battle detector: a different vector
// object means a different battle.
const void* resolve_char_list(void* turn_control) {
    const void* scene  = nullptr;
    const void* sub    = nullptr;
    const void* holder = nullptr;
    const void* list   = nullptr;
    if (!turn_control) return nullptr;
    if (!mem_read((const uint8_t*)turn_control + kTC_Scene, &scene, sizeof(scene)) || !scene) return nullptr;
    if (!mem_read((const uint8_t*)scene + kScene_Sub, &sub, sizeof(sub)) || !sub) return nullptr;
    if (!mem_read((const uint8_t*)sub + kSub_Holder, &holder, sizeof(holder)) || !holder) return nullptr;
    if (!mem_read((const uint8_t*)holder + kHolder_List, &list, sizeof(list)) || !list) return nullptr;
    return list;
}

void snapshot_cats(void* turn_control) {
    if (g.snapped || !turn_control) return;

    const void* list = resolve_char_list(turn_control);
    if (!list) return;

    uint32_t    count = 0;
    const void* data  = nullptr;
    if (!mem_read((const uint8_t*)list + kList_Count, &count, sizeof(count))) return;
    if (!mem_read((const uint8_t*)list + kList_Data, &data, sizeof(data)) || !data) return;
    if (count == 0 || count > kMaxCats) {
        // NOT a warning-and-continue. Without a roster there is no control
        // split, so nothing is sent, nothing is injected and no hash is
        // compared -- the two peers play the same battle independently and
        // every summary line reports success. A boss fight ran that way for
        // four turns and the session still ended "0 desync(s)".
        //
        // A count of 0 is the reverse case: the list is not ready yet, which
        // happens legitimately before a battle is built. That one just returns.
        if (count == 0) return;
        char why[128];
        _snprintf_s(why, sizeof(why), _TRUNCATE,
                    "character list count %u exceeds kMaxCats (%u) -- lockstep "
                    "cannot run this battle", count, kMaxCats);
        log_line("LOCKSTEP", "!! %s", why);
        halt(why);
        return;
    }

    g.cat_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const void* c = nullptr;
        if (!mem_read((const uint8_t*)data + i * sizeof(void*), &c, sizeof(c)) || !c) continue;
        g.cats[g.cat_count++] = c;
    }
    g.snapped      = true;
    g.snapped_list = list;

    // A new roster invalidates the table the last hash was taken over, and
    // re-arms the one-shot desync dump for this battle.
    g.hashed_valid = false;
    g.hashed_count = 0;
    g.dump_sent    = false;
    g.dump_seen    = false;

    // --- who is human -------------------------------------------------
    //
    // First, because the automatic split is derived from it. Only human-driven
    // cats are split between the peers; an AI cat is left to each peer's own
    // brain to re-derive, which is not a compromise but the stronger
    // arrangement -- phase 2B deliberately left 12 of 29 decisions to
    // PatternBrain rather than injecting them, and all 12 matched.
    const Config& cfg = config();
    char bcls[kMaxCats][96];
    g.humans = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        const void* brain = nullptr;
        strcpy_s(bcls[i], "?");
        if (mem_read((const uint8_t*)g.cats[i] + kChar_Brain, &brain, sizeof(brain)) && brain)
            rtti_class_name(brain, bcls[i], sizeof(bcls[i]));
        g.human_cat[i] = (strstr(bcls[i], tune::kReplayBrains) != nullptr);
        if (g.human_cat[i]) ++g.humans;
    }

    // --- the control split ---------------------------------------------
    for (uint32_t i = 0; i < kMaxCats; ++i) g.local_cat[i] = false;

    if (cfg.net_control_auto) {
        // Derived identically on both peers rather than negotiated, which is
        // why it needs no message and no waiting: the roster is byte-identical
        // (measured -- 29 cats, same brain class per index, two processes with
        // completely different heap addresses), the human filter is the same
        // string compare, and the rule below is pure arithmetic. Both peers
        // still exchange CONTROL afterwards, but as a check on the result --
        // not as the source of it.
        //
        // Spread the human cats over however many players are in the session,
        // in roster order, host first, with the remainder going to the earliest
        // positions. Four cats over three players is 2/1/1; three over two is
        // 2/1, which is what the two-player rule always did -- the old
        // `(humans+1)/2` is exactly this formula at P=2, so nothing changes for
        // a two-player session.
        //
        // Position, not peer id: see net_peer_pos. And contiguous rather than
        // interleaved, so each player's cats are adjacent in the roster and the
        // log is readable by eye.
        uint32_t P   = net_peer_count();
        uint32_t pos = net_peer_pos();
        if (P == 0) {
            // No PEERS yet. Claim nothing rather than guess: claiming
            // everything would have two peers driving the same cat, and
            // claiming half assumes a player count we have not been told.
            P = 1; pos = 0;
            log_line("LOCKSTEP", "!! no peer list yet -- deferring the split; if "
                                 "this repeats, PEERS never arrived");
        }
        const SplitRange range = split_for(g.humans, P, pos);
        const uint32_t   mine  = range.count;

        uint32_t seen = 0;
        for (uint32_t i = 0; i < g.cat_count; ++i) {
            if (!g.human_cat[i]) continue;
            g.local_cat[i] = split_owns(range, seen);
            ++seen;
        }

        if (mine == 0 && g.humans)
            log_line("LOCKSTEP", "!! %u human cat(s) over %u player(s) leaves this "
                                 "peer (position %u) with none -- it will watch "
                                 "this battle", g.humans, P, pos);
    } else {
        for (uint32_t i = 0; i < cfg.net_control_count; ++i) {
            uint8_t idx = cfg.net_control[i];
            if (idx < g.cat_count) g.local_cat[idx] = true;
            else log_line("LOCKSTEP", "!! net_control names cat %u but the battle has %u",
                          (unsigned)idx, g.cat_count);
        }
    }

    uint32_t local = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) if (g.local_cat[i]) ++local;
    g.stats.cats       = g.cat_count;
    g.stats.local_cats = local;

    // --- the roster ----------------------------------------------------
    //
    // The one artefact that lets two peers be compared by eye before anything
    // has gone wrong. If the two logs disagree here, nothing downstream is
    // worth debugging. HP and tile are printed alongside so the field offsets
    // can be checked against what is actually on screen -- a state hash built
    // on a wrong offset agrees with itself forever and proves nothing.
    log_line("LOCKSTEP", "battle roster: %u cats, %u human, %u local (%s)",
             g.cat_count, g.humans, local,
             cfg.net_control_auto ? "auto split" : "net_control list");

    uint32_t linked = 0, sane = 0;
    for (uint32_t i = 0; i < g.cat_count; ++i) {
        const char* who = !g.human_cat[i] ? "ai    "
                        : g.local_cat[i]  ? "LOCAL "
                                          : "peer  ";
        CatState st{};
        bool got = read_cat_state(g.cats[i], st);
        if (got && st.linked)     ++linked;
        if (got && plausible(st)) ++sane;

        if (got)
            log_line("LOCKSTEP", "  cat %2u  %s  hp=%d/%d tile=(%d,%d)%s%s%s  brain=%s",
                     i, who, st.hp, st.maxhp, st.tx, st.ty,
                     st.dead ? " DEAD" : "",
                     st.linked      ? "" : " [unlinked]",
                     plausible(st)  ? "" : " [odd]", bcls[i]);
        else
            log_line("LOCKSTEP", "  cat %2u  %s  <state unreadable>  brain=%s", i, who, bcls[i]);
    }

    // --- is the state hash trustworthy on this build? ------------------
    //
    // Gated on evidence rather than on the offsets having been written down
    // correctly. Two failures are caught: the TacticsObject back-pointer not
    // round-tripping (so the tile is being read out of the wrong object), and
    // HP/tile outside any plausible range (so the offsets belong to a different
    // build). Either would have the two peers hashing unrelated bytes -- and
    // since heap layout differs between processes, that is a guaranteed false
    // desync on turn 1, which is strictly worse than no state hash at all.
    //
    // Strict on purpose: every cat must pass, not most of them. A partial pass
    // is precisely the ambiguous case, and the roster above marks each failing
    // cat [unlinked] or [odd], so relaxing this later is a one-line edit made
    // with the data in hand rather than a guess made now.
    g.state_hash_on = tune::kStateHash && g.cat_count > 0 &&
                      linked == g.cat_count && sane == g.cat_count;
    if (!tune::kStateHash)
        log_line("LOCKSTEP", "state hash disabled by net_state_hash = 0");
    else if (g.state_hash_on)
        log_line("LOCKSTEP", "state hash ON -- all %u cats linked and in range", g.cat_count);
    else
        log_line("LOCKSTEP", "!! state hash OFF -- %u/%u cats linked, %u/%u in range."
                             " Look for [unlinked] / [odd] in the roster above: those"
                             " Character offsets do not fit this build, and hashing"
                             " them would desync both peers over unrelated bytes",
                 linked, g.cat_count, sane, g.cat_count);

    if (g.humans == 0)
        log_line("LOCKSTEP", "!! no human-driven cats in this battle -- lockstep has"
                             " nothing to split (matched brain '%s')", tune::kReplayBrains);

    // --- publish the split ---------------------------------------------
    //
    // Both peers send their own list, and both check it against the other's.
    // Symmetric rather than host-authoritative because nobody is waiting on it:
    // each side already derived its half locally, so this is purely the check
    // that used to be impossible -- a human cat claimed by NEITHER peer, which
    // otherwise shows up as a battle stalling on that cat's turn with nothing
    // in either log to say why. Making it one-way would put that check in only
    // one of the two logs.
    {
        ControlMsg c{};
        c.battle_id = g.battles.current;
        c.humans = g.humans;
        for (uint32_t i = 0; i < g.cat_count && c.count < 32; ++i)
            if (g.local_cat[i]) c.cats[c.count++] = (uint8_t)i;
        net_send_control(c);

        // File our own claim in the same table as everyone else's. The coverage
        // check is a tally over all players, and leaving ourselves out of it
        // would make every cat we own look unclaimed.
        uint8_t self = net_self();
        if (self < kMaxPeers) {
            g.peer_control[self]      = c;
            g.have_peer_control[self] = true;
        }
    }

    // The host may already have told us its split before we had a roster to
    // check it against.
    verify_control();
}

// Check the peer's half of the split against ours. Both sides ran the same rule
// over the same roster, so agreement should be a formality -- and when it is
// not, this is the earliest possible warning that the two peers are not in the
// same battle.
//
// Deferred rather than checked on arrival, because each peer reaches its first
// turn boundary whenever it does: CONTROL can land before this side has a
// roster to compare it against, and a check that silently does not run is worse
// than no check at all.
void verify_control() {
    if (g.control_checked || !g.snapped) return;

    // Every member must have spoken for THIS battle before anything can be
    // concluded. With two players that was one message; with four it is three,
    // and checking coverage against a partial set would halt on cats whose
    // owner simply has not reported yet.
    uint8_t ids[kMaxPeers] = {};
    uint8_t count = net_peer_count();
    if (count == 0 || !net_peer_ids(ids, kMaxPeers)) return;

    for (uint8_t i = 0; i < count; ++i) {
        uint8_t id = ids[i];
        if (id >= kMaxPeers) return;
        if (!g.have_peer_control[id]) return;
        // Held until the epochs line up. A peer may have sent its split for a
        // battle we have not reached yet; checking it against THIS battle's
        // roster would compare two different rosters and halt on a difference
        // that is only our own lag.
        if (g.peer_control[id].battle_id != g.battles.current) return;
    }
    g.control_checked = true;

    // Everyone must agree how many human cats there are before their claims
    // mean anything -- a peer counting a different number is looking at a
    // different battle, and its indices are not comparable to ours.
    for (uint8_t i = 0; i < count; ++i) {
        const ControlMsg& c = g.peer_control[ids[i]];
        if (c.humans != g.humans) {
            char why[192];
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "roster disagreement: peer %u counted %u human cat(s), we see %u",
                        (unsigned)ids[i], c.humans, g.humans);
            halt(why);
            return;
        }
    }

    // Claim tally. Every human cat must be claimed by EXACTLY ONE player, and
    // both failures are fatal for opposite reasons: claimed twice and two
    // players drive one brain, which desyncs on the first turn it acts; claimed
    // by nobody and it never acts at all, so the battle stalls forever with
    // every peer politely waiting for someone else. The second is the one that
    // used to be invisible, and with more players there are more ways to
    // produce it.
    uint8_t claims[kMaxCats] = {};
    uint8_t owner[kMaxCats];
    for (uint32_t i = 0; i < kMaxCats; ++i) owner[i] = kNoPeer;

    for (uint8_t i = 0; i < count; ++i) {
        const ControlMsg& c = g.peer_control[ids[i]];
        for (uint8_t j = 0; j < c.count; ++j) {
            uint8_t cat = c.cats[j];
            if (cat >= kMaxCats) continue;
            ++claims[cat];
            if (owner[cat] == kNoPeer) owner[cat] = ids[i];
        }
    }

    for (uint32_t i = 0; i < g.cat_count; ++i) {
        if (!g.human_cat[i]) continue;
        if (claims[i] > 1) {
            char why[192];
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "cat %u is claimed by %u players -- two people driving one"
                        " brain desyncs on its first turn", i, (unsigned)claims[i]);
            halt(why);
            return;
        }
        if (claims[i] == 0) {
            char why[192];
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "cat %u is human but claimed by no player -- its turn"
                        " would stall forever", i);
            halt(why);
            return;
        }
    }

    // Who ended up with what, in one line, because with four players "the split
    // agreed" is no longer enough to picture it.
    char who[192] = {};
    int  off = 0;
    for (uint8_t i = 0; i < count && off < (int)sizeof(who) - 24; ++i) {
        const ControlMsg& c = g.peer_control[ids[i]];
        off += _snprintf_s(who + off, sizeof(who) - off, _TRUNCATE,
                           "%speer %u:%u", i ? "  " : "", (unsigned)ids[i],
                           (unsigned)c.count);
    }
    log_line("LOCKSTEP", "control split agreed across %u player(s): %u human cat(s)"
                         " -- %s", (unsigned)count, g.humans, who);

    // Reaching here is exactly the moment the join barrier opens -- every
    // player has a roster for this battle and they agree about it. Said
    // explicitly, and with the wait, because "the battle started" and "the
    // battle started with everyone in it" are the two things the logs
    // previously could not be used to tell apart.
    if (config().net_join_barrier && !g.barrier_said_open) {
        g.barrier_said_open = true;
        if (g.barrier_waits)
            log_line("LOCKSTEP", "join barrier OPEN for battle %u after %u poll(s)"
                                 " -- all %u player(s) are in this battle",
                     g.epoch, g.barrier_waits, (unsigned)count);
        else
            log_line("LOCKSTEP", "join barrier OPEN for battle %u -- all %u player(s)"
                                 " were already here, nothing was held",
                     g.epoch, (unsigned)count);
    }
}

// --- the join barrier -------------------------------------------------------
//
// Neither peer may decide anything until BOTH have snapshotted this battle's
// roster. Without it, a peer that is not yet in the battle is simply absent
// while the other plays on -- the late-join gap, reproduced 2026-08-24:
//
//   host    -> cat 20 ability slot=1:0 target=(4,3) gon=DefaultMove
//   host    -> cat 20 ability slot=2:0 target=(6,6) gon=BasicMagicShortRanged
//   host    -> cat 20 endturn
//   client  <- cat 20 endturn                     <- the only one it applied
//   client  !! HALT at turn 2: hash mismatch (rng)
//
// The client's turn-2 rng_hash was byte-identical to its own turn 1: its stream
// never advanced, because the move and the spell never ran there.
//
// The queue half of that was fixed by the battle epoch -- pending actions are
// now purged rather than cleared, so nothing is thrown away. This is the other
// half, and it is the one that matters: keeping the actions is no use if the
// peer that missed them has already computed a hash without them. The barrier
// makes "both peers are here" a precondition for anything happening at all,
// rather than something we hope was true.
//
// It costs nothing to wait. Brain::GetChoice is a POLL -- it returns type=1
// ("nothing decided yet") every frame while waiting on a human, and did so for
// 1695 of 1711 calls in one tutorial battle -- so parking a brain is exactly
// the state the game is already in between clicks. No timeout, no blocking
// recv, no frame budget: the same reason the transport needs none.
//
// The condition is already computed for us. verify_control() sets
// control_checked only when this peer has a roster AND the peer's CONTROL for
// the SAME epoch has arrived and agreed -- which is precisely "both of us are
// in this battle, and we concur about who drives what".

// The branches themselves live in mgmp_barrier.h as a pure function, tested by
// tests/test_barrier.cpp; this is only the gather. Same split as td_decide and
// HashRing, and for the same reason -- exercising it in game means arranging
// for two peers to drift apart, which is how the gap was found in the first
// place and is no way to check the edges.
// Collapse "every player has reported" into the two facts barrier_decide takes.
// It stays a two-value question -- have we heard from everyone, and for which
// battle -- so the decision table and its tests are unchanged; what "everyone"
// means is what grew.
//
// The battle reported is ANY member's that differs from ours, because the
// barrier must wait for the peer that is not here yet. Reporting a matching one
// while another peer is elsewhere would open the gate on the strength of a peer
// that is already in step. (With a counter this was "the earliest epoch"; ids
// do not order, so "not ours" is the test, and it is the one that was always
// meant.)
void aggregate_controls(bool& all_in, uint64_t& odd_battle) {
    all_in = false;
    odd_battle = g.battles.current;

    uint8_t ids[kMaxPeers] = {};
    uint8_t count = net_peer_count();
    if (count == 0 || !net_peer_ids(ids, kMaxPeers)) return;

    bool     every = true;
    uint64_t odd   = g.battles.current;
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t id = ids[i];
        if (id >= kMaxPeers || !g.have_peer_control[id]) { every = false; continue; }
        if (g.peer_control[id].battle_id != g.battles.current)
            odd = g.peer_control[id].battle_id;
    }
    all_in = every;
    odd_battle = every ? odd : g.battles.current;
}

const char* barrier_blocking() {
    bool     all_in = false;
    uint64_t peer_battle = 0;
    aggregate_controls(all_in, peer_battle);

    BarrierFacts f;
    f.enabled         = config().net_join_barrier;
    f.control_checked = g.control_checked;
    f.snapped         = g.snapped;
    f.have_peer_ctrl  = all_in;
    f.peer_battle     = peer_battle;
    f.battle          = g.battles.current;
    f.peer_battle_retired = g.battles.is_retired(peer_battle);
    return barrier_decide(f);
}

void barrier_wait_tick(const char* why) {
    if (g.barrier_waits == 0)
        log_line("LOCKSTEP", "join barrier: holding every decision until both peers"
                             " are in battle %u -- %s", g.epoch, why);
    ++g.barrier_waits;
    // ~10 s at 60 fps. Repeated on purpose: a barrier that never opens looks
    // exactly like a hung game, so it has to keep saying which side it is
    // waiting on for as long as it waits.
    if (g.barrier_waits % 600 == 0)
        log_line("LOCKSTEP", "join barrier: still holding after %u poll(s) -- %s",
                 g.barrier_waits, why);
}

// --- battle identity --------------------------------------------------------

// True if the message belongs to a battle we have already LEFT, and must be
// dropped. A message for a battle we have not reached is NOT stale -- it is
// held, which is what lets a peer arrive late without losing what happened
// without it.
//
// Reported once per battle rather than per message: a boundary crossed
// mid-exchange drops a small burst, and one line with a count says everything
// the per-message spam would, without burying the turn hashes around it.
bool stale_battle(uint64_t battle_id, const char* what) {
    if (!g.battles.is_retired(battle_id)) return false;
    ++g.stale_drops;
    if (!g.said_stale) {
        g.said_stale = true;
        log_line("LOCKSTEP", "dropping %s for battle %016llx -- we finished that one"
                             " and are in %016llx now (in flight across the battle"
                             " boundary; expected, and the reason messages carry a"
                             " battle id)",
                 what, (unsigned long long)battle_id,
                 (unsigned long long)g.battles.current);
    }
    return true;
}

// --- the outbound log, for a peer that joins mid-battle ---------------------
//
// Every decision this peer has taken in the CURRENT battle, in order. A peer
// arriving at turn 12 re-enters the same node, so its battle rebuilds
// identically from MapNode+0x118 and its AI re-derives -- but the human
// decisions taken before it arrived exist nowhere except here. Replaying them
// is what lets it fast-forward turns 0..11 instead of playing a different
// battle from turn 0.
//
// They are replayed as ORDINARY ACTION messages: the joining peer's pend_push
// accepts them and pend_take hands them out cat by cat exactly as if they had
// arrived live, so the whole injection path is the one already proven. That is
// why there is no ACTIONLOG message type -- there is nothing for one to say.
void sent_log_push(const ActionMsg& a) {
    if (g.sent_log_n >= kMaxSentLog) {
        if (!g.sent_log_full) {
            g.sent_log_full = true;
            log_line("LOCKSTEP", "!! this battle has run past %u recorded decisions --"
                                 " a peer joining from here cannot be caught up and"
                                 " will be told so rather than desync quietly",
                     kMaxSentLog);
        }
        return;
    }
    g.sent_log[g.sent_log_n++] = a;
}

// --- pending remote actions -------------------------------------------------

void pend_push(const ActionMsg& a) {
    // A decision the PEER took also belongs in the catch-up log. The log has to
    // be the whole battle's history, not this peer's half of it: a joiner
    // rebuilding the fight has to replay what every human did, and a human cat
    // cannot be re-derived the way an AI one can.
    //
    // Logged in ARRIVAL order rather than execution order, which is enough
    // because pend_take resolves per cat and the order within one cat is
    // preserved. Two cats' decisions being swapped relative to each other does
    // not change what either cat does.
    if (a.battle_id == g.battles.current) sent_log_push(a);

    if (g.pend_count >= kMaxPending) {
        log_line("LOCKSTEP", "!! pending queue full -- dropping action for cat %u", a.actor);
        return;
    }
    g.pending[(g.pend_head + g.pend_count) % kMaxPending] = a;
    ++g.pend_count;
}

// Drops queued actions from a battle we have left, keeping this battle's and
// anything already sent for a battle we have NOT reached. Keeping the latter is
// the half that also closes the late-join drop: this queue used to be emptied
// wholesale at the reset, which is what lost a joining peer the actions taken
// before it arrived.
uint32_t pend_purge_retired() {
    // Compacted through a temporary rather than in place. The queue is a RING:
    // once pend_head is non-zero the read cursor wraps, so writing survivors
    // straight to the front would overwrite entries this loop has not read yet.
    // (Only ever called on the game thread, like every other pend_* function,
    // so the buffer needs no guard of its own.)
    static ActionMsg keep[kMaxPending];
    uint32_t kept = 0, dropped = 0;
    for (uint32_t i = 0; i < g.pend_count; ++i) {
        const ActionMsg& a = g.pending[(g.pend_head + i) % kMaxPending];
        if (g.battles.is_retired(a.battle_id)) { ++dropped; continue; }
        keep[kept++] = a;
    }
    for (uint32_t i = 0; i < kept; ++i) g.pending[i] = keep[i];
    g.pend_head  = 0;
    g.pend_count = kept;
    return dropped;
}

// Pops the oldest pending action for `cat`, if any. Actions for different cats
// can legitimately be in flight at once, so this is not a plain FIFO pop.
bool pend_take(uint8_t cat, ActionMsg& out) {
    for (uint32_t i = 0; i < g.pend_count; ++i) {
        uint32_t slot = (g.pend_head + i) % kMaxPending;
        if (g.pending[slot].actor != cat) continue;
        // The battle id is part of the key, not a filter applied afterwards. An
        // action held for the NEXT battle names a cat index that exists in this
        // one too, so matching on the cat alone would inject a decision from
        // the wrong battle -- the same bug battle identity exists to stop, one
        // layer down.
        if (g.pending[slot].battle_id != g.battles.current) continue;
        out = g.pending[slot];
        // Close the gap by shifting the entries behind it forward one.
        for (uint32_t j = i; j + 1 < g.pend_count; ++j) {
            uint32_t a = (g.pend_head + j) % kMaxPending;
            uint32_t b = (g.pend_head + j + 1) % kMaxPending;
            g.pending[a] = g.pending[b];
        }
        --g.pend_count;
        return true;
    }
    return false;
}

// --- hashing ----------------------------------------------------------------

HashMsg build_hash(void* turn_control) {
    HashMsg h{};
    h.battle_id = g.battles.current;
    h.turn  = g.turn;

    // The simulation stream. 32 bytes, and the only RNG state lockstep has to
    // agree on -- TLS+0x198 is presentation and is *meant* to differ (measured:
    // 10x variation between runs C and D while all 44 sim draws stayed
    // byte-identical).
    if (uint64_t* s = rng_global_stream())
        h.rng_hash = fnv1a(s, 32);

    // The decision queue. Cheapest and earliest divergence signal there is:
    // types 6 and 7 exist *because* a passive fired, so a differing proc roll
    // changes the queue population before it changes any visible state, and it
    // costs no serialization of the ~1390 component classes.
    uint32_t depth = 0;
    if (turn_control)
        mem_read((const uint8_t*)turn_control + kTC_QueueCount, &depth, sizeof(depth));
    h.queue_depth = depth;
    h.queue_sig   = (uint32_t)fnv1a(&depth, sizeof(depth));

    // Character state: HP, shield, max HP, tile and the dead flag, in roster
    // order. Only runs when the snapshot's self-check passed -- see there for
    // why a state hash that cannot prove its own offsets is worse than none at
    // all.
    //
    // Status/passive counts are deliberately NOT in here. The Character's
    // passive list at +0xE70 is a lazily rebuilt CACHE with a dirty flag, and
    // anything that enumerates passives -- including a tooltip on one peer and
    // not the other -- can rebuild it. Hashing a cache whose freshness depends
    // on where the mouse is would manufacture desyncs, which is the one failure
    // this hash must not have.
    //
    // FACING IS EXCLUDED FOR THE SAME REASON, and it was measured rather than
    // reasoned. Two peers on different machines halted at turn 5 with identical
    // rng and queue hashes and a state hash differing on exactly one field of
    // one cat: the acting cat's facing, (0,1) against (-1,0), with HP, shield
    // and tile agreeing on all 29 cats. Nothing had rolled or resolved
    // differently -- which is precisely what an identical rng hash means.
    //
    // Character::Face is called from glaiel::Brain::UpdateDecision @ 0x1401377C9
    // under this guard:
    //
    //     cmp   dword ptr [rdi+220h], 2      ; a type-2 decision is cached
    //     jnz   skip
    //     comisd xmm7, qword ptr [rdi+2A8h]  ; the WALL-CLOCK dt timer
    //     jnb   skip                         ; already elapsed -> no Face at all
    //     mov   rdx, [rdi+238h]              ; the pending decision's direction
    //     call  glaiel::Character::Face
    //
    // and the statement immediately above it, under the same two branches, is
    // Brain::DrawAbilityAOE -- a DRAWING call. This whole block is the aim
    // preview: while a decision is held, the cat turns toward where it is being
    // aimed. Whether it turns at all depends on how many frames fit inside the
    // dt window, so a peer at a different frame rate lands on the other side of
    // that `jnb`. Run E measured the same battle at 23,211 frames and 12,230.
    //
    // This never appeared in the loopback test because both instances run on one
    // box at the same frame rate. It is the same class of bug as
    // TimeDelayStatusApplication: wall-clock time reaching something we treat as
    // simulation state.
    //
    // Excluding it is safe rather than merely convenient, and the reason is
    // narrow: every SIMULATION write to facing is made by the action itself --
    // Ability::trigger and Character::DoAction both call Face with the committed
    // direction. The preview write only survives in the gap BETWEEN actions, so
    // dropping it from the hash loses no divergence that an action would not
    // re-establish. Facing is still read and still printed in the halt dump,
    // where it costs nothing and occasionally explains one.
    if (g.state_hash_on) {
        uint64_t sh = 1469598103934665603ULL;

        // Which snapshot entries are still in the battle. A cat the battle has
        // dropped keeps its slot in our frozen snapshot but its object is no
        // longer meaningfully owned, so every field we could read off it is
        // heap luck rather than game state. Hash the MEMBERSHIP -- so two peers
        // that disagree about whether a cat left still diverge -- and then stop,
        // because nothing after that point is trustworthy on either side.
        uint32_t live_h = 0, appeared_h = 0;
        bool still_h[kMaxCats];
        const uint8_t memb =
            snapshot_membership(g.snapped_list, live_h, still_h, appeared_h) ? 1 : 0;
        // Hashed too: a peer that can read membership and one that cannot are
        // computing different things, and that must not pass as agreement.
        sh = fnv1a(&memb, sizeof(memb), sh);

        for (uint32_t i = 0; i < g.cat_count; ++i) {
            const bool present = !memb || still_h[i];
            const uint8_t in_battle = present ? 1 : 0;
            sh = fnv1a(&in_battle, sizeof(in_battle), sh);

            CatState st{};
            uint8_t got = read_cat_state(g.cats[i], st, present) ? 1 : 0;
            sh = fnv1a(&got, sizeof(got), sh);

            // Keep the row for the desync dump. Written here rather than
            // re-read later because HERE is the instant the hash describes;
            // `readable` and `in_battle` ride along because a peer that could
            // read a cat the other could not is a difference in its own right.
            st.in_battle = in_battle;
            st.readable  = got;
            g.hashed[i]  = st;

            // Everything below reads the object itself, which a departed entry
            // no longer backs. Membership above already carries the divergence
            // that matters.
            if (!present) continue;
            // Field by field, skipping fx/fy. Deliberately not a memcpy of a
            // trimmed struct: the next person to add a field should have to
            // decide which side of this line it goes on.
            sh = fnv1a(&st.hp,     sizeof(st.hp),     sh);
            sh = fnv1a(&st.shield, sizeof(st.shield), sh);
            sh = fnv1a(&st.maxhp,  sizeof(st.maxhp),  sh);
            sh = fnv1a(&st.tx,     sizeof(st.tx),     sh);
            sh = fnv1a(&st.ty,     sizeof(st.ty),     sh);
            sh = fnv1a(&st.dead,   sizeof(st.dead),   sh);
            sh = fnv1a(&st.linked, sizeof(st.linked), sh);
            // The elements affecting this cat -- passives, statuses, equipment
            // and, decisively, the TILE it is standing on. Everything else in
            // this hash is a per-character field, so grid state has been
            // invisible to it from the start: two peers whose battlefield is
            // wet in different places agree on every cat until a Fire ability
            // resolves differently, and then disagree by a few HP with an
            // IDENTICAL rng hash, because nothing rolled.
            //
            // This also drags equipment into the hash for the first time, which
            // is the gap that let a mismatched trinket survive to turn 4.
            sh = fnv1a(&st.e0,    sizeof(st.e0),    sh);
            sh = fnv1a(&st.e1,    sizeof(st.e1),    sh);
            sh = fnv1a(&st.elems, sizeof(st.elems), sh);
        }
        h.state_hash = sh;
        g.hashed_count = g.cat_count;
        g.hashed_turn  = h.turn;
        g.hashed_valid = true;
    }
    return h;
}

void halt(const char* why) {
    if (g.halted) return;
    g.halted = true;
    g.stats.halted = true;
    ++g.stats.desyncs;
    log_line("LOCKSTEP", "!! HALT at turn %u: %s", g.turn, why);

    HaltMsg m{};
    m.turn = g.turn;
    _snprintf_s(m.reason, sizeof(m.reason), _TRUNCATE, "%s", why);
    net_send_halt(m);
}

// --- the desync dump --------------------------------------------------------
//
// Two halves of one idea: put the table this peer hashed on the wire, and print
// a real field-by-field diff when the other peer's arrives. Sent only after a
// hash has already disagreed, so it costs nothing in a healthy session.

void send_state_dump(uint32_t turn) {
    if (g.dump_sent) return;

    if (!g.hashed_valid) {
        log_line("LOCKSTEP", "   (no state dump: the state hash is off, so there "
                             "is no per-cat table to send -- only rng and queue "
                             "were ever compared)");
        g.dump_sent = true;
        return;
    }
    // Refusing here rather than sending the live table is the point. A dump
    // labelled turn N that actually holds turn N+1's numbers would produce a
    // diff full of differences that were never in the hash, and it would look
    // exactly like a real one.
    if (g.hashed_turn != turn) {
        log_line("LOCKSTEP", "   (no state dump: our table is from turn %u and the "
                             "mismatch is at turn %u -- sending it would diff two "
                             "different instants)", g.hashed_turn, turn);
        g.dump_sent = true;
        return;
    }
    if (g.hashed_count == 0 || g.hashed_count > kMaxDumpCats) return;

    StateDumpMsg m{};
    m.battle_id = g.battles.current;
    m.turn      = turn;
    m.count     = g.hashed_count;
    m.stride    = (uint32_t)sizeof(CatState);
    m.size      = m.count * m.stride;
    m.data      = (uint8_t*)g.hashed;
    g.dump_sent = net_send_statedump(m);
}

// Prints one line per cat whose row differs, and nothing at all for the rows
// that agree -- which on a real desync is almost all of them.
void compare_state_dump(const StateDumpMsg& m) {
    if (g.dump_seen) return;

    if (m.stride != sizeof(CatState)) {
        log_line_lvl(LogLevel::Error, "LOCKSTEP",
                     "!! the peer's state dump packs %u bytes per cat and this "
                     "build packs %u -- NOT diffing it. The two peers are running "
                     "different revisions of the record, which is a bigger "
                     "problem than the desync.",
                     m.stride, (unsigned)sizeof(CatState));
        g.dump_seen = true;
        return;
    }
    if (!g.hashed_valid || g.hashed_turn != m.turn) {
        log_line("LOCKSTEP", "   (the peer's turn-%u state dump arrived but our own "
                             "table is from turn %u -- not diffing two different "
                             "instants)", m.turn, g.hashed_turn);
        g.dump_seen = true;
        return;
    }
    g.dump_seen = true;

    if (m.count != g.hashed_count) {
        // Not a reason to stop: the overlap still diffs, and a roster length
        // difference is itself the most useful line in the dump.
        log_line_lvl(LogLevel::Error, "LOCKSTEP",
                     "!! ROSTER SIZE: this peer snapshotted %u cats and the peer "
                     "snapshotted %u. The two are not playing the same battle.",
                     g.hashed_count, m.count);
    }

    const CatState* theirs = (const CatState*)m.data;
    const uint32_t  n = (m.count < g.hashed_count) ? m.count : g.hashed_count;

    log_line("LOCKSTEP", "STATE DIFF at turn %u (this peer | the peer), %u cats "
                         "compared:", m.turn, n);

    uint32_t differ = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const CatState& a = g.hashed[i];
        const CatState& b = theirs[i];

        // Field by field, and facing is INCLUDED here even though the hash
        // excludes it. That exclusion is about what may halt a run; it is not a
        // reason to hide the field once the run has already halted -- and
        // facing is precisely what explained the 49-turn backstab desync.
        char line[320];
        int  k = 0;
        // _snprintf_s returns -1 on truncation, so the length is re-measured
        // rather than accumulated: a negative return added to `k` would index
        // backwards out of the buffer.
        #define MGMP_DIFF(fmt, fa, fb)                                          \
            if ((fa) != (fb) && k < (int)sizeof(line) - 48) {                    \
                _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, fmt, fa, fb); \
                k = (int)strlen(line);                                           \
            }

        line[0] = 0;
        MGMP_DIFF(" hp %d|%d",        a.hp, b.hp);
        MGMP_DIFF(" shield %d|%d",    a.shield, b.shield);
        MGMP_DIFF(" maxhp %d|%d",     a.maxhp, b.maxhp);
        MGMP_DIFF(" dead %u|%u",      (unsigned)a.dead, (unsigned)b.dead);
        MGMP_DIFF(" elem0 %08X|%08X", a.e0, b.e0);
        MGMP_DIFF(" elem1 %08X|%08X", a.e1, b.e1);
        MGMP_DIFF(" in_battle %u|%u", (unsigned)a.in_battle, (unsigned)b.in_battle);
        MGMP_DIFF(" readable %u|%u",  (unsigned)a.readable,  (unsigned)b.readable);
        MGMP_DIFF(" linked %u|%u",    (unsigned)a.linked,    (unsigned)b.linked);
        MGMP_DIFF(" elems %u|%u",     (unsigned)a.elems,     (unsigned)b.elems);
        #undef MGMP_DIFF

        if ((a.tx != b.tx || a.ty != b.ty) && k < (int)sizeof(line) - 48) {
            _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE,
                        " tile (%d,%d)|(%d,%d)", a.tx, a.ty, b.tx, b.ty);
            k = (int)strlen(line);
        }
        if ((a.fx != b.fx || a.fy != b.fy) && k < (int)sizeof(line) - 48) {
            _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE,
                        " FACE (%d,%d)|(%d,%d) [not hashed]",
                        a.fx, a.fy, b.fx, b.fy);
            k = (int)strlen(line);
        }

        if (!line[0]) continue;
        ++differ;
        log_line_lvl(LogLevel::Error, "LOCKSTEP", "  cat %2u %s", i, line);
    }

    if (differ == 0) {
        // Worth its own loud line. Every hashed field agreeing while the hash
        // did not means the divergence is somewhere the state hash does not
        // look: the rng stream, the queue, or state no cat row carries.
        log_line_lvl(LogLevel::Warn, "LOCKSTEP",
                     "  every cat row AGREES. Whatever diverged is not in the "
                     "cat table -- look at the rng and queue halves of the hash, "
                     "and at the summon tail below, which the snapshot does not "
                     "cover.");
    } else {
        log_line("LOCKSTEP", "  %u of %u cat(s) differ. Diff the two peers' "
                             "APPLY/DOACTION/TRIGGER lines for the first one, "
                             "back from this turn.", differ, n);
    }
}

void compare_hash(const HashMsg& mine, const HashMsg& theirs) {
    if (mine.turn != theirs.turn) return;
    if (mine.rng_hash == theirs.rng_hash &&
        mine.queue_depth == theirs.queue_depth &&
        mine.state_hash == theirs.state_hash)
        return;

    // Say which half disagreed, because the two mean different things. RNG or
    // queue means the simulations took different draws -- a real divergence. A
    // state-only miss with the stream still in step is the suspicious one: it
    // is what a wrong field offset looks like, so dump the per-cat table on
    // both peers and let a diff of the two logs name the cat and the field.
    bool rng_ok   = (mine.rng_hash == theirs.rng_hash);
    bool queue_ok = (mine.queue_depth == theirs.queue_depth);

    char why[192];
    _snprintf_s(why, sizeof(why), _TRUNCATE,
                "turn %u hash mismatch (%s%s%s): rng %016llx/%016llx queue %u/%u state %016llx/%016llx",
                mine.turn,
                rng_ok ? "" : "rng ", queue_ok ? "" : "queue ",
                (rng_ok && queue_ok) ? "state only" : "",
                (unsigned long long)mine.rng_hash, (unsigned long long)theirs.rng_hash,
                mine.queue_depth, theirs.queue_depth,
                (unsigned long long)mine.state_hash, (unsigned long long)theirs.state_hash);
    ++g.mismatches;
    if (!g.diverged) { g.diverged = true; g.first_mismatch = mine.turn; }

    // Both peers publish the table their own hash was taken over, whichever
    // half disagreed. It used to go out only for a state-only mismatch, on the
    // reasoning that rng or queue means "a real divergence" and the cats would
    // not add anything -- but "the streams differ" is a symptom, and the cat
    // that took different damage two turns ago is what caused it. The dump is
    // free here: it is sent once, after the run is already lost.
    send_state_dump(mine.turn);

    // --- halting (the default) ------------------------------------------
    if (config().net_desync_halt) {
        if (rng_ok && queue_ok) dump_cat_states("state-only mismatch");
        halt(why);
        return;
    }

    // --- reporting only (net_desync_halt = 0) ----------------------------
    //
    // Still counted as a desync -- the run is not clean and the summary must
    // not pretend otherwise -- but the simulation continues so that the next
    // few boundaries can say whether this was a divergence or a skew.
    ++g.stats.desyncs;

    // The first two get the whole table; after that it would be the same 45
    // lines with one number moving, which is what the delta trace is for.
    const bool worth_dumping = (rng_ok && queue_ok && g.mismatches <= 2);
    if (worth_dumping)
        dump_cat_states("state-only mismatch (NOT halting -- net_desync_halt = 0)");

    log_line("LOCKSTEP", "!! MISMATCH #%u at turn %u (not halting): %s",
             g.mismatches, mine.turn, why);
    if (!worth_dumping && rng_ok && queue_ok)
        log_line("LOCKSTEP", "   (cat table suppressed after the first two --"
                             " read the `delta:` lines instead)");
}

// Reports one matched pair, either way. The AGREES line is not decoration: a
// log that speaks only on mismatch cannot be told apart from a log whose check
// never ran, and telling those two apart is the entire point of this fix.
void report_pair(const HashMsg& mine, const HashMsg& theirs, uint8_t peer) {
    if (mine.rng_hash == theirs.rng_hash &&
        mine.queue_depth == theirs.queue_depth &&
        mine.state_hash == theirs.state_hash) {
        ++g.agreements;
        // Trace, not Info: this is one line per turn per peer, and in a healthy
        // session it is the single loudest thing in the panel -- 59 of them in
        // one measured run, which is what buries the lines a player needs to
        // see. It stays in the FILE, where it is the evidence the comment above
        // describes; the panel shows the count instead, in the summary and on
        // the session window's `turn` row.
        log_line_lvl(LogLevel::Trace, "LOCKSTEP",
                 "turn %u AGREES with peer %u%s", mine.turn,
                 (unsigned)peer,
                 g.state_hash_on ? " (rng+queue+state)" : " (rng+queue)");

        // The line net_desync_halt exists to print. Two peers that agree again
        // after a mismatch did not have a divergence: they had an effect land
        // on opposite sides of a turn boundary, which is a timing artefact and
        // is fixed somewhere completely different from a simulation bug. Said
        // once, at the moment it re-converges, because that is the moment the
        // evidence exists -- and a halting run never reaches it.
        if (g.diverged) {
            g.diverged = false;
            log_line("LOCKSTEP", "   ^^ TRANSIENT: the turn-%u mismatch has"
                                 " RE-CONVERGED by turn %u without either peer"
                                 " being corrected. State that diverges and comes"
                                 " back is timing, not simulation -- something"
                                 " landed on opposite sides of the boundary."
                                 " Diff the two peers' `delta:` lines around"
                                 " turn %u to see what.",
                     g.first_mismatch, mine.turn, g.first_mismatch);
        }
        return;
    }
    log_line("LOCKSTEP", "!! the disagreement is with peer %u", (unsigned)peer);
    compare_hash(mine, theirs);
}

} // namespace

// ---------------------------------------------------------------------------

void lockstep_set_base(uintptr_t base) {
    g.affecting_elements = nullptr;

    const uintptr_t addr = addr_of_call(C_AffectingElements);
    if (!addr) {
        // Not fatal. Losing this costs detection, not correctness: the hash
        // falls back to exactly what it covered before, which is the state the
        // last several sessions ran on.
        log_line("LOCKSTEP", "!! %s did not resolve by signature -- "
                             "element state will NOT be hashed",
                 kCalls[C_AffectingElements].name);
        return;
    }
    g.affecting_elements = (fn_affecting_elements)addr;
}

void lockstep_init() {
    if (g.active) return;
    if (!g.cs_ready) { InitializeCriticalSection(&g.cs); g.cs_ready = true; }

    // IS THIS A FRESH SESSION, OR A RECONNECT INTO A BATTLE WE NEVER LEFT?
    //
    // A peer that only lost its socket is still standing in the same battle, in
    // the same process, holding the same live Character objects -- pointers are
    // stable within one battle, and this snapshot describes the fight that is
    // still on screen. Wiping it is not "starting clean", it is throwing away
    // the only description of where we are.
    //
    // And wiping it DEADLOCKS, which is how this was found. The chain, measured
    // on 2026-08-26 (host log 021012 / client 021026):
    //
    //   1. reconnect -> go_ready -> lockstep_init -> snapped = false
    //   2. lockstep_fill_choice opens with `if (!g.snapped) return false`, so
    //      every cat -- ours and the peer's -- falls through to the local brain
    //   3. the peer's cats then wait for a local click that is never coming,
    //      and our own cats never consult the 4 decisions the host replayed
    //   4. no decision -> the turn never ends -> TurnControl::NextTurn never
    //      fires -> snapshot_cats, which ONLY runs from the turn boundary,
    //      never runs. Nothing can ever set snapped again.
    //
    // The client's log named it exactly: "0 sent, 0 applied, 4 still pending".
    // The catch-up worked; there was no roster to spend it against.
    //
    // So: keep what describes the BATTLE, reset what describes the SESSION.
    //
    // `snapped` stays set between battles -- it is only cleared when a turn
    // boundary sees a DIFFERENT character list -- so this also fires for a peer
    // that reconnects while standing on the map, preserving a roster that
    // describes the fight that just ended. That is deliberate and harmless:
    // nothing takes a human decision outside a battle, and the first turn
    // boundary of the next battle resets the roster, the turn, the control
    // check and the barrier together. It is the same self-correction that
    // already covers a stale roster today, so it needs no second mechanism.
    const bool rejoin_in_battle = g.snapped && g.snapped_list != nullptr;

    g.active  = true;
    g.halted  = false;
    if (!rejoin_in_battle) {
        g.snapped = false;
        // Turn numbering is per battle and the HASH messages are keyed on it.
        // Resetting to 0 while standing on turn 2 would compare our turn 2
        // against the peer's turn 0 for the rest of the fight.
        g.turn = 0;
    }
    for (uint32_t p = 0; p < kMaxPeers; ++p) {
        g.peer_hash[p].clear();
        g.peer_hash_full_warned[p] = false;
    }
    g.my_hash.clear();
    g.epoch             = 0;
    g.stale_drops       = 0;
    g.said_stale        = false;
    g.local_actor       = false;
    g.state_hash_on     = false;

    // The control split and the barrier go with the battle, not the session,
    // for the same reason the roster does -- and for one more.
    //
    // The barrier opens when both peers have exchanged CONTROL for this battle.
    // Clearing that on a reconnect makes the returning peer wait for a CONTROL
    // the host will never send: the host stayed Ready throughout, so its own
    // control_checked is still set and verify_control returns early forever.
    // The rejoining peer would sit at a barrier nobody can open -- a stall with
    // nothing in either log to explain it, which is precisely the failure the
    // barrier's "no disconnect timeout" note accepts as the better one and
    // therefore must not be manufactured here.
    //
    // The split is a property of (this battle, this membership) and neither
    // changed: the peer that came back is the same peer, at the same index.
    // If membership DID change while we were away the split is stale until the
    // next battle re-derives it -- acceptable, and no worse than the general
    // rule that an explicit net_control list is stale the moment you fight
    // anything else.
    if (!rejoin_in_battle) {
        g.humans          = 0;
        g.control_checked = false;
        for (uint32_t p = 0; p < kMaxPeers; ++p) g.have_peer_control[p] = false;
        g.barrier_said_open = false;
    }
    g.barrier_waits     = 0;
    g.have_prev         = false;
    g.mismatches        = 0;
    g.first_mismatch    = 0;
    g.diverged          = false;
    // Per SESSION, not per battle: lockstep_init has exactly one caller,
    // go_ready. Reset here so a reconnect that compares nothing before the run
    // ends reports that honestly instead of inheriting the previous session's
    // agreements -- which is CLAUDE.md's open item 2, the case where `0
    // desync(s)` was never backed by a single comparison.
    g.agreements        = 0;
    // Trace unless the barrier is off, in which case the warning is the point.
    log_line_lvl(config().net_join_barrier ? LogLevel::Trace : LogLevel::Warn,
             "LOCKSTEP", "armed (%s)%s",
             net_role() == NetRole::Host ? "host" : "client",
             config().net_join_barrier ? "" : " -- join barrier DISABLED by mgmp.json:"
             " a peer that is not yet in a battle will silently miss the turns"
             " taken without it");
    if (rejoin_in_battle)
        log_line("LOCKSTEP", "re-armed INSIDE battle %016llx at turn %u -- kept the "
                             "roster (%u cat(s)), the control split and the barrier; "
                             "this peer never left the fight",
                 (unsigned long long)g.battles.current, g.turn, g.cat_count);
}

void lockstep_shutdown() {
    if (!g.active) return;
    LockstepStats s = lockstep_stats();

    // `0 desync(s)` is a pass ONLY if something was compared. A session that
    // never reached a shared turn boundary produces the identical number and
    // means nothing by it -- so the agreement count is printed beside it and
    // decides the severity. See CLAUDE.md rule 3.
    const LogLevel lvl = s.desyncs      ? LogLevel::Error
                       : !g.agreements  ? LogLevel::Warn
                                        : LogLevel::Trace;
    log_line_lvl(lvl, "LOCKSTEP",
             "done: %u sent, %u applied, %u still pending, %u desync(s)%s",
             s.sent, s.applied, s.pending, s.desyncs,
             g.agreements ? "" : " and NO TURN HASH WAS EVER COMPARED -- this run "
                                 "says nothing about whether the battles stayed "
                                 "in sync");
    if (g.agreements)
        log_line_lvl(s.desyncs ? LogLevel::Error : LogLevel::Trace, "LOCKSTEP",
                 "   %u turn hash(es) compared and agreed", g.agreements);
    g.active = false;
    if (g.cs_ready) { DeleteCriticalSection(&g.cs); g.cs_ready = false; }
}

bool lockstep_active() { return g.active && net_active(); }
bool lockstep_halted() { return g.halted; }
bool lockstep_in_battle() { return g.active && g.snapped && !g.halted; }

// One question with three answers, because mgmp_aim needs all three and asking
// them separately would mean three lookups of the same character and three
// chances to disagree about the answer.
//
// The three-way shape is the control split's, deliberately: AI cats are decided
// locally on BOTH peers, so they are neither published nor drawn. Treating one
// as remote would paint a preview over a cat that is about to move on its own.
bool lockstep_aim_subject(const void* character, uint8_t& cat, bool& peer_owns) {
    if (!g.active || !g.snapped || !character) return false;
    const uint8_t i = cat_index_of(character);
    if (i == kNoCat)     return false;   // a summon: in nobody's roster
    if (!g.human_cat[i]) return false;   // an AI cat: both peers drive it
    cat       = i;
    peer_owns = !g.local_cat[i];
    return true;
}

// --- the aim preview's write to facing --------------------------------------
//
// Brain::UpdateDecision turns the acting cat toward where it is being aimed
// while a decision is held, and it does it by calling Character::Face -- the
// same durable write the committed action makes. See the PREVIEWFACE target in
// mgmp_addresses.h for the guard it sits under; the short version is that
// whether the turn happens at all depends on a WALL-CLOCK dt timer, so two
// peers at different frame rates end up with different facings.
//
// That was known and was thought to be cosmetic. It is not: sub_14011C6F0, the
// backstab test, is called from Character::receive_damage_passives and reads
// Character+0x388 against the direction the hit came from. A cat facing its
// attacker takes a normal hit; one facing away takes a backstab. So the preview
// reaches the simulation through DAMAGE, and it reaches it on the DEFENDER --
// which is why the old argument ("the preview write only survives in the gap
// between actions, and the next action re-establishes it") was wrong. The gap
// between a cat's own actions is exactly when it gets attacked.
//
// The fix is to let the preview run and then put the field back. Nothing else
// inside UpdateDecision writes facing -- Character::Face is called from it
// exactly once -- and the committed direction is written by Ability::trigger
// and Character::DoAction, which are outside this function. So the cat no
// longer visibly turns while you aim it, and every facing either peer ever
// reads is one an action put there.
struct PreviewFacing {
    uint8_t* ch = nullptr;
    int32_t  fx = 0, fy = 0;
    bool     have = false;
    uint32_t restored = 0;
    bool     said = false;
};
PreviewFacing g_pf;

void lockstep_preview_facing_begin(void* brain) {
    g_pf.have = false;
    // Only inside a net session: a solo game has nobody to diverge from, and
    // the preview is a real piece of feedback to the player.
    if (!brain || !lockstep_active()) return;

    uint8_t* ch = nullptr;
    if (!mem_read((const uint8_t*)brain + kBrain_Character, &ch, sizeof(ch)) || !ch)
        return;
    if (!mem_read(ch + kChar_Facing,     &g_pf.fx, sizeof(g_pf.fx))) return;
    if (!mem_read(ch + kChar_Facing + 4, &g_pf.fy, sizeof(g_pf.fy))) return;
    g_pf.ch   = ch;
    g_pf.have = true;
}

void lockstep_preview_facing_end() {
    if (!g_pf.have) return;
    g_pf.have = false;

    int32_t fx = 0, fy = 0;
    if (!mem_read(g_pf.ch + kChar_Facing,     &fx, sizeof(fx))) return;
    if (!mem_read(g_pf.ch + kChar_Facing + 4, &fy, sizeof(fy))) return;
    if (fx == g_pf.fx && fy == g_pf.fy) return;

    if (!mem_write(g_pf.ch + kChar_Facing,     &g_pf.fx, sizeof(g_pf.fx))) return;
    if (!mem_write(g_pf.ch + kChar_Facing + 4, &g_pf.fy, sizeof(g_pf.fy))) return;
    ++g_pf.restored;

    // Once per session. The count is what matters, not each event -- this fires
    // on most frames of a held decision.
    if (!g_pf.said) {
        g_pf.said = true;
        log_line("LOCKSTEP", "the aim preview turned a cat (%p: (%d,%d) -> (%d,%d)) "
                             "and was put back -- facing is read by the backstab "
                             "test, so it must come from actions only",
                 (void*)g_pf.ch, fx, fy, g_pf.fx, g_pf.fy);
    }
}

uint32_t lockstep_preview_facing_count() { return g_pf.restored; }

// --- the state fence --------------------------------------------------------
//
// See mgmp_lockstep.h. The reason this exists rather than a facing-only guard:
// the call it wraps was justified once by "it only draws", and that claim was
// wrong in a way no fence then in place could see. A fence that watches only
// the field you already know about would repeat the mistake, so this one takes
// the whole cat state -- the same struct the per-turn hash uses.

struct StateFence {
    CatState snap[kMaxCats]{};
    bool     got[kMaxCats]{};
    uint32_t count = 0;
    bool     armed = false;
    uint32_t hits  = 0;     // cats that moved, ever, this session
    bool     said  = false;
};

StateFence g_sf;

void lockstep_state_fence_begin() {
    g_sf.armed = false;
    if (!g.active || !g.snapped || g.halted) return;
    g_sf.count = g.cat_count > kMaxCats ? kMaxCats : g.cat_count;
    for (uint32_t i = 0; i < g_sf.count; ++i)
        g_sf.got[i] = read_cat_state(g.cats[i], g_sf.snap[i]);
    g_sf.armed = true;
}

uint32_t lockstep_state_fence_end(const char* what) {
    if (!g_sf.armed) return 0;
    g_sf.armed = false;

    uint32_t moved = 0;
    for (uint32_t i = 0; i < g_sf.count; ++i) {
        if (!g_sf.got[i]) continue;
        CatState now{};
        if (!read_cat_state(g.cats[i], now)) continue;

        const CatState& was = g_sf.snap[i];
        const bool face_moved = (now.fx != was.fx || now.fy != was.fy);
        // memcmp would fold facing in with the rest; compare the fields that
        // cannot be put back separately, so the log can say which kind it was.
        const bool rest_moved =
            now.hp   != was.hp   || now.shield != was.shield ||
            now.maxhp!= was.maxhp|| now.tx     != was.tx     ||
            now.ty   != was.ty   || now.dead   != was.dead   ||
            now.e0   != was.e0   || now.e1     != was.e1;

        if (!face_moved && !rest_moved) continue;
        ++moved;

        if (face_moved) {
            // Putting it back is correct here for the same reason it is correct
            // in the aim-preview freeze: the only facing either peer may read
            // is the one an action wrote.
            mem_write((uint8_t*)g.cats[i] + kChar_Facing,     &was.fx, sizeof(was.fx));
            mem_write((uint8_t*)g.cats[i] + kChar_Facing + 4, &was.fy, sizeof(was.fy));
        }

        if (!g_sf.said) {
            g_sf.said = true;
            log_line_lvl(rest_moved ? LogLevel::Error : LogLevel::Warn, "LOCKSTEP",
                         "!! %s moved cat %u's state: face (%d,%d)->(%d,%d)%s "
                         "hp %d->%d tile (%d,%d)->(%d,%d) dead %u->%u. Facing is "
                         "put back; the rest is NOT, because a value invented "
                         "here would hide a divergence rather than fix one.",
                     what, i, was.fx, was.fy, now.fx, now.fy,
                     face_moved ? " (restored)" : "",
                     was.hp, now.hp, was.tx, was.ty, now.tx, now.ty,
                     (unsigned)was.dead, (unsigned)now.dead);
        }
    }
    g_sf.hits += moved;
    return moved;
}

uint32_t lockstep_state_fence_hits() { return g_sf.hits; }

void lockstep_pump() {
    if (!g.active) return;

    NetMsg m{};
    while (net_poll(m)) {
        Guard guard;
        switch (m.type) {
            case MSG_ACTION:
                if (stale_battle(m.action.battle_id, "an action")) break;
                pend_push(m.action);
                break;

            case MSG_HASH: {
                if (g.halted) break;
                if (stale_battle(m.hash.battle_id, "a hash")) break;

                // Match against a turn we have ALREADY passed. Without this the
                // check is one-sided: comparison used to happen only at our own
                // turn boundary against hashes that had already arrived, so the
                // peer running AHEAD found an empty ring every time and compared
                // nothing. Measured -- a clean 4-turn run had the client print
                // four AGREES and the host none. A desync would have halted the
                // trailing peer while the leading one played on.
                if (m.from >= kMaxPeers) break;
                if (const HashMsg* mine = g.my_hash.find(m.hash.battle_id, m.hash.turn)) {
                    report_pair(*mine, m.hash, m.from);
                    break;
                }

                // Otherwise that peer is ahead of us; hold it until we get there.
                if (!g.peer_hash[m.from].push_refusing(m.hash) &&
                    !g.peer_hash_full_warned[m.from]) {
                    // Dropping the NEWEST keeps the held turns contiguous from
                    // where we stand, which is what the boundary consumes. Said
                    // once, because the alternative -- what this code did before
                    // -- was to discard peer hashes forever in silence.
                    g.peer_hash_full_warned[m.from] = true;
                    log_line("LOCKSTEP", "!! peer %u is more than %u turns ahead; "
                                         "dropping its hash for turn %u and any "
                                         "further ones -- those turns go unchecked",
                             (unsigned)m.from, kHashRing, m.hash.turn);
                }
                break;
            }

            // The map layer's message rides the same queue: net_poll drains
            // one queue, so exactly one place may pump it.
            case MSG_ENTERNODE:
                follow_on_message(m.enter_node);
                break;

            // Also not epoch-gated: a decision-screen choice belongs to a map
            // node, not to a battle, and the epoch counter does not advance on
            // an event node at all.
            case MSG_CHOICE:
                choice_on_message(m.choice);
                break;

            case MSG_SAVEFILE:
                savefile_on_message(m.savefile);
                break;

            // Not battle-gated either: leaving the run is the one thing the
            // host does that no other message reports, and it is true
            // whatever this peer happens to be in the middle of.
            case MSG_HOSTLEFT:
                leave_on_message(m.hostleft);
                break;

            // Not epoch-gated, and deliberately so: a cat is RUN state, not
            // battle state. It stays true across a battle boundary, so a late
            // one is still correct -- unlike an ACTION, which means nothing
            // outside the battle it was taken in.
            case MSG_CATDATA:
                catsync_on_message(m.catdata);
                break;

            // Run state as well, and not epoch-gated for the same reason.
            case MSG_INVENTORY:
                invsync_on_message(m.inventory);
                break;

            // Run state too: the used-event list is true across a battle
            // boundary, so a late one is still correct.
            case MSG_RUNHIST:
                runhist_on_message(m.runhist);
                break;

            // Pure detection, and about a map node rather than a battle -- so
            // it is neither battle-gated nor stale-counted here. It carries its
            // own node seed and mgmp_nodehash matches on that.
            case MSG_NODEHASH:
                nodehash_on_message(m.from, m.nodehash);
                break;

            // Cosmetic, so it is neither epoch-gated nor stale-counted here.
            // The overlay compares epochs itself, at the moment it would draw,
            // and hides a cursor that belongs to another board instead of
            // discarding the message -- a peer one battle ahead comes back into
            // view the instant we catch up, with no gap to re-fill.
            case MSG_CURSOR:
                cursor_on_message(m.from, m.cursor);
                overlay_on_message(m.from, m.cursor);
                break;

            // Same class as CURSOR exactly: cosmetic, not epoch-gated here.
            // mgmp_aim checks the battle id itself, at the moment it would
            // draw, for the same reason the overlay does.
            case MSG_AIM:
                aim_on_message(m.aim);
                break;

            case MSG_CONTROL:
                // A CONTROL for the NEXT battle is kept, not acted on: it is
                // held until our own epoch catches up, and verify_control is
                // what gates that. Dropping it would leave the split unchecked
                // for a whole battle, which is precisely the failure CONTROL
                // was added to make visible.
                if (stale_battle(m.control.battle_id, "a control split")) break;
                if (m.from < kMaxPeers) {
                    g.peer_control[m.from]      = m.control;
                    g.have_peer_control[m.from] = true;
                } else {
                    log_line("LOCKSTEP", "!! control split from peer %u, which is "
                                         "outside the peer table -- ignored",
                             (unsigned)m.from);
                }
                verify_control();
                break;

            // Deliberately NOT gated on g.halted. This is the one message that
            // has to be processed after a halt, because it only ever arrives
            // after one -- and the peer's HALT usually beats it here.
            case MSG_STATEDUMP:
                if (stale_battle(m.statedump.battle_id, "a state dump")) break;
                compare_state_dump(m.statedump);
                // Answer in kind, so the peer that halted first also gets a
                // diff in its own log. send_state_dump is idempotent.
                send_state_dump(m.statedump.turn);
                break;

            case MSG_HALT:
                if (!g.halted) {
                    g.halted = true;
                    g.stats.halted = true;
                    ++g.stats.desyncs;
                    log_line("LOCKSTEP", "!! peer halted at turn %u: %s",
                             m.halt.turn, m.halt.reason);
                }
                break;

            case MSG_REFUSE:
                log_line("LOCKSTEP", "!! peer refused: %s", m.refuse);
                break;

            // A peer joining while we are already playing. This has to be
            // handled HERE as well as in session_update, because session_update
            // returns early once this peer is Ready and lockstep_pump becomes
            // the only thing draining the queue. Falling into `default` below
            // is what silently dropped the second client's handshake: it
            // connected, received PEERS from the net layer, and was then never
            // accepted, welcomed or refused by anyone.
            case MSG_HELLO:
                session_on_hello(m.from, m.hello);
                break;

            default:
                break;
        }
        // MSG_SAVEFILE is the one frame that owns memory. Releasing every frame
        // unconditionally -- including the ones this switch ignored -- keeps
        // that fact in one place instead of one place per case.
        net_msg_release(m);
    }
}

// Defined just below, after its first caller.
static void apply_remote(const ActionMsg& msg, const void* actor, void* out);

bool lockstep_fill_choice(void* brain, void* out) {
    if (!g.active || !brain || !out) return false;
    if (g.halted) {
        // Freeze both peers rather than let one run on. Forcing type=1 parks
        // the game in its normal "waiting for a decision" state, which is a
        // far kinder failure than a crash or a silently diverged battle.
        uint32_t none = TA_None;
        mem_write(out, &none, sizeof(none));
        return true;
    }

    if (!g.snapped) return false;

    const void* actor = nullptr;
    if (!mem_read((const uint8_t*)brain + kBrain_Owner, &actor, sizeof(actor)) || !actor)
        return false;

    uint8_t cat = cat_index_of(actor);
    // Recorded before the AI and barrier branches below, so it tracks every
    // turn rather than only the ones this function goes on to act upon. A
    // summon (kNoCat) is nobody's, same as an AI cat.
    g.local_actor = (cat != kNoCat) && g.local_cat[cat];
    if (cat == kNoCat) return false;      // a summon: not in the snapshot, so
                                          // neither peer treats it as owned and
                                          // both let their own AI drive it.

    // An AI cat is decided locally on BOTH peers -- never sent, never injected,
    // never suppressed. Getting this wrong is a deadlock, not a desync: a cat
    // that neither side sends for is a cat both sides sit and wait for.
    //
    // "Never suppressed" includes the join barrier below, and that is NOT an
    // oversight -- it is the whole reason the barrier works. Measured
    // 2026-08-24, and it cost a run to learn:
    //
    //   host:    RangedAttackAbility target=(3,8) dir=(-1,0)
    //   client:  RangedAttackAbility target=(7,3) dir=(0,-1)   <- the right one
    //
    // An earlier version of the barrier held AI cats too, on the theory that
    // letting one peer take AI turns the other had not reached was divergence
    // by a quieter door. The opposite is true, twice over:
    //
    //   - Holding an AI brain is NOT free. Brain::UpdateDecision calls GetChoice
    //     only when nothing is cached, so overwriting the decision it just
    //     released makes the AI DERIVE A NEW ONE next frame -- and deriving
    //     draws from the simulation stream. The host sat at the barrier for 529
    //     polls, re-derived that many times, and by the time the barrier opened
    //     its stream had run far past the client's. turn 0 hashes matched
    //     exactly; turn 1 disagreed on rng with state_hash still identical,
    //     which is precisely what "extra draws, same outcome so far" looks like.
    //   - Letting the AI run ahead IS safe, for the reason the control split
    //     already depends on: AI decisions are deterministic and re-derived
    //     identically on both peers (phase 2B left 12 of 29 decisions to
    //     PatternBrain and all 12 matched). A peer ahead on AI turns walks the
    //     same turns with the same draws, and the other peer catches up.
    //
    // Holding a HUMAN brain, by contrast, really is free: GetChoice returns
    // type=1 by itself while waiting on a person and did so for 1695 of 1711
    // calls in one tutorial battle, so suppressing it changes nothing it was
    // going to do anyway. That asymmetry is the whole design.
    if (!g.human_cat[cat]) return false;

    // The join barrier -- human cats only, per the note above. This is what the
    // late-join gap actually needed: the host must not take HUMAN decisions and
    // send them while the client is not in the battle to receive them.
    {
        Guard guard;
        if (const char* why = barrier_blocking()) {
            barrier_wait_tick(why);
            uint32_t none = TA_None;
            mem_write(out, &none, sizeof(none));
            return true;
        }
    }

    Guard guard;

    // --- our cat: let the brain decide, then publish the decision -----------
    if (g.local_cat[cat]) {
        // ...UNLESS a decision for this cat is already sitting in the queue.
        //
        // In a normal battle that never happens: nobody sends us decisions for
        // cats we own, so this branch is inert. It fires only when we have
        // JOINED A BATTLE ALREADY UNDER WAY and the host replayed its history
        // to us -- and then it is the whole reason the catch-up works. Our own
        // human cats already acted in the turns we are fast-forwarding through,
        // and a human decision cannot be re-derived the way an AI one can, so
        // without this the replay would stall on the first turn one of our cats
        // was due to act and wait for a click that already happened.
        //
        // Not gated on a "catching up" flag on purpose: "there is a decision
        // for this cat that we did not make" IS the condition, and a flag would
        // just be a less direct way of asking the same question.
        ActionMsg replayed{};
        if (pend_take(cat, replayed)) {
            log_line("LOCKSTEP", "<= catch-up: replaying our own cat %u's turn-%u "
                                 "decision (we joined this battle late)",
                     (unsigned)cat, replayed.turn);
            apply_remote(replayed, actor, out);
            return true;
        }

        if (g.outstanding) return false;

        TurnAction a{};
        if (!mem_read(out, &a, sizeof(a))) return false;
        if (a.type != TA_Ability && a.type != TA_EndTurn) return false;

        ActionMsg msg{};
        msg.battle_id = g.battles.current;
        msg.turn  = g.turn;
        msg.actor = cat;
        msg.type  = (uint8_t)a.type;
        msg.tx = a.target_x; msg.ty = a.target_y;
        msg.dx = a.dir_x;    msg.dy = a.dir_y;
        msg.b30 = a.tail[0x30 - 0x28];
        msg.b31 = a.tail[0x31 - 0x28];

        if (a.type == TA_Ability) {
            AbilitySlot slot = ability_slot_of(actor, a.ability);
            msg.slot_kind  = slot.kind;
            msg.slot_index = slot.index;
            ability_gon_name(a.ability, msg.gon, sizeof(msg.gon));
            if (slot.kind == SLOT_UNKNOWN) {
                // Unreproducible on the peer: it has no way to name this
                // ability. Say so loudly rather than send an action the other
                // side will resolve to nullptr and silently skip.
                log_line("LOCKSTEP", "!! cat %u used an ability in no known slot "
                                     "(gon='%s') -- peer cannot resolve it",
                         (unsigned)cat, msg.gon[0] ? msg.gon : "?");
            }
        }

        // Logged BEFORE the send, and logged whether or not the send succeeds.
        // The log is what a peer joining mid-battle replays to catch up, so it
        // has to describe what this peer DID, not what it managed to transmit
        // -- a decision that failed to send is exactly the one a joining peer
        // most needs, and a peer that was not connected yet cannot have been
        // sent anything at all.
        sent_log_push(msg);

        if (net_send_action(msg)) {
            g.outstanding = true;
            ++g.stats.sent;
            log_line("LOCKSTEP", "-> cat %u %s slot=%u:%u target=(%d,%d) gon=%s",
                     (unsigned)cat, a.type == TA_EndTurn ? "endturn" : "ability",
                     msg.slot_kind, msg.slot_index, msg.tx, msg.ty,
                     msg.gon[0] ? msg.gon : "-");
        }
        return false;    // the local brain's own decision proceeds unchanged
    }

    // --- the peer's cat: overwrite unconditionally --------------------------
    //
    // Unconditionally, because this is also what suppresses local input on a
    // cat we do not own: a stray click produces a decision that we replace
    // here, so it can never reach QueueDecision.
    ActionMsg msg{};
    if (!pend_take(cat, msg)) {
        uint32_t none = TA_None;
        mem_write(out, &none, sizeof(none));
        return true;                       // nothing yet -- keep waiting, free
    }
    apply_remote(msg, actor, out);
    return true;
}

// Turn a decision that came off the wire into the TurnAction the brain was
// about to return. Factored out because there are now two callers and they
// differ only in WHOSE cat it is: the peer's, in the ordinary case, and our
// own when we joined a battle late and the host replayed what our cats already
// did. Everything below -- the slot resolution, the GON cross-check, the halt
// on a mismatch -- has to be identical for both, because a replayed decision is
// exactly as authoritative as a live one.
static void apply_remote(const ActionMsg& msg, const void* actor, void* out) {
    const uint8_t cat = msg.actor;

    TurnAction a{};
    memset(&a, 0, sizeof(a));
    a.type     = msg.type;
    a.target_x = msg.tx; a.target_y = msg.ty;
    a.dir_x    = msg.dx; a.dir_y    = msg.dy;
    a.actor    = nullptr;                  // arrives null from a real brain too;
                                           // Character::DoAction fills it in
    a.tail[0x30 - 0x28] = msg.b30;
    a.tail[0x31 - 0x28] = msg.b31;

    if (msg.type == TA_Ability) {
        AbilitySlot slot{ msg.slot_kind, msg.slot_index };
        a.ability = ability_from_slot(actor, slot);
        if (!a.ability) {
            char why[192];
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "cat %u slot %u:%u (gon '%s') is empty on this peer",
                        (unsigned)cat, msg.slot_kind, msg.slot_index, msg.gon);
            halt(why);
            uint32_t none = TA_None;
            mem_write(out, &none, sizeof(none));
            return;
        }
        // Resolve by slot, validate by name. A disagreement here means the two
        // peers' cats no longer hold the same abilities, which is a desync that
        // has already happened -- catching it now beats playing the wrong spell.
        char gon[64];
        if (msg.gon[0] && ability_gon_name(a.ability, gon, sizeof(gon)) &&
            strcmp(gon, msg.gon) != 0) {
            char why[192];
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "cat %u slot %u:%u holds '%s', peer sent '%s'",
                        (unsigned)cat, msg.slot_kind, msg.slot_index, gon, msg.gon);
            halt(why);
            uint32_t none = TA_None;
            mem_write(out, &none, sizeof(none));
            return;
        }
    }

    mem_write(out, &a, sizeof(a));
    ++g.stats.applied;
    log_line("LOCKSTEP", "<- cat %u %s slot=%u:%u target=(%d,%d) gon=%s",
             (unsigned)cat, msg.type == TA_EndTurn ? "endturn" : "ability",
             msg.slot_kind, msg.slot_index, msg.tx, msg.ty,
             msg.gon[0] ? msg.gon : "-");
}

void lockstep_on_applied(const void* action, const void* actor) {
    if (!g.active || !action) return;

    TurnAction a{};
    if (!mem_read(action, &a, sizeof(a))) return;

    // Types 6 and 7 are generated locally on both peers and are never sent.
    // They must not clear the outstanding guard or advance anything.
    if (a.type != TA_Ability && a.type != TA_EndTurn) return;

    // Match the recorder's precedence exactly (mgmp_hooks.cpp, h_ApplyAction):
    // prefer the action's own actor, fall back to TurnControl's current one.
    // Getting this backwards is what made the replayer cry wolf on a Toss --
    // an action that carries its own actor and is not the turn's actor.
    const void* who = a.actor ? a.actor : actor;

    Guard guard;
    g.outstanding = false;

    // A cat outside the snapshot is a summon, which both peers generate
    // locally; nothing to account for. Inside it, this is where a per-action
    // cross-check would go once the state hash covers Character fields.
    uint8_t cat = cat_index_of(who);
    if (cat != kNoCat && g.local_cat[cat]) ++g.stats.applied;
}

void lockstep_turn_boundary(void* turn_control) {
    if (!g.active) return;

    // A new battle means a new character list, and the old roster describes
    // cats that no longer exist. Detecting it matters more now than it did in
    // phase 4: with the split derived from the roster, a stale roster means a
    // stale split, and the second battle of a run would be played with the
    // first battle's ownership.
    //
    // The vector OBJECT is the right thing to compare, not its data pointer:
    // summons append and can reallocate the data mid-battle, which is not a new
    // battle. Both peers notice at the same logical point -- the first turn
    // boundary of the new battle -- so the turn numbering the HASH messages are
    // keyed on stays aligned across the reset.
    if (g.snapped && !g.halted) {
        const void* list = resolve_char_list(turn_control);
        if (list && list != g.snapped_list) {
            ++g.epoch;
            log_line("LOCKSTEP", "new battle -- re-snapshotting the roster"
                                 " (was %u cats over %u turns; epoch %u now)",
                     g.cat_count, g.turn, g.epoch);

            g.snapped           = false;
            g.snapped_list      = nullptr;
            g.turn              = 0;
            g.outstanding       = false;
            g.control_checked   = false;
            g.state_hash_on     = false;
            for (uint32_t p = 0; p < kMaxPeers; ++p) g.peer_hash_full_warned[p] = false;
            g.stale_drops       = 0;
            g.said_stale        = false;

            // The catch-up log belongs to the battle that just ended.
            g.sent_log_n        = 0;
            g.sent_log_full     = false;

            // Both are per battle for the same reason the barrier is: a new
            // battle's cat 3 is a different cat, so last battle's readings are
            // not a baseline, and a mismatch count carried across would make
            // the second battle look like a continuation of the first.
            g.have_prev         = false;
            g.mismatches        = 0;
            g.first_mismatch    = 0;
            g.diverged          = false;

            // The barrier is per battle, not per session: the peer has to be
            // shown to be in THIS one. Re-arming it here is what makes it cover
            // every battle of a run rather than only the first -- a peer can
            // fall behind at any node, and the second battle is no safer than
            // the first.
            g.barrier_waits     = 0;
            g.barrier_said_open = false;

            // Purge rather than clear. Anything the peer already sent for THIS
            // new battle -- it may have got here first -- is still wanted, and
            // wiping it is what lost a late peer the actions taken without it.
            //
            // g.battles is already up to date here: the map layer called
            // lockstep_enter_battle at EnterNode, which retired the battle we
            // just left and made the new node's seed current. This detection
            // fires later, at the first turn boundary, so "retired" already
            // means what these purges need it to mean.
            uint32_t da = pend_purge_retired();
            uint32_t dh = 0;
            for (uint32_t p = 0; p < kMaxPeers; ++p)
                dh += g.peer_hash[p].purge_retired(g.battles);
            g.my_hash.clear();                 // all ours, all from the old battle
            if (da || dh)
                log_line("LOCKSTEP", "  dropped %u stale action(s) and %u stale hash(es)"
                                     " from the battle we just left", da, dh);
            if (g.pend_count)
                log_line("LOCKSTEP", "  kept %u action(s) the peer had already sent"
                                     " for this battle", g.pend_count);

            // The peer's split only survives if it is for this battle or a
            // later one; anything older describes a roster that no longer
            // exists.
            for (uint32_t p = 0; p < kMaxPeers; ++p)
                if (g.have_peer_control[p] &&
                    g.battles.is_retired(g.peer_control[p].battle_id))
                    g.have_peer_control[p] = false;
        }
    }

    snapshot_cats(turn_control);
    if (!g.snapped || g.halted) return;

    Guard guard;
    HashMsg mine = build_hash(turn_control);
    net_send_hash(mine);

    // Log every turn's hash, not only mismatches. A log that goes quiet when
    // things are fine cannot be distinguished from a log whose check never
    // ran -- and the first run of this hook produced exactly that ambiguity.
    // `chars` is the LIVE character-list count, not our snapshot's.
    //
    // The snapshot is deliberately fixed at battle start, so summons that append
    // to the list and deaths that remove from it are invisible to the state hash
    // -- a summon is not hashed and is driven locally by each peer's own AI.
    // That is correct as long as both peers summon the SAME things at the same
    // turns, and nothing checked it. A boss that used SpawnAbility twice and
    // then diverged on state with an identical rng hash is exactly the shape
    // that would produce, so print the live count every turn: if the two logs
    // disagree here, the divergence is in the unhashed part of the roster and no
    // amount of staring at the 45 hashed cats will show it.
    //
    // Logged rather than hashed on purpose. Hashing it would halt on a
    // difference that a summon's death animation could produce a frame apart,
    // and a false halt is worse than a late one -- the same reasoning that keeps
    // passive counts out of the hash.
    // Counted by MEMBERSHIP rather than by length, because an append and a
    // removal in the same turn cancel out and a bare count then reports "45/45,
    // nothing happened" through a roster change big enough to desync. The
    // appeared/left pair is what actually has to match between the two logs.
    uint32_t live_chars = 0, appeared = 0, departed = 0;
    bool still_in[kMaxCats];
    const void* list = resolve_char_list(turn_control);
    if (!list) list = g.snapped_list;
    if (snapshot_membership(list, live_chars, still_in, appeared))
        departed = snapshot_departed(still_in);
    else if (list)
        mem_read((const uint8_t*)list + kList_Count, &live_chars, sizeof(live_chars));

    char roster[96];
    roster[0] = 0;
    if (appeared || departed)
        _snprintf_s(roster, sizeof(roster), _TRUNCATE,
                    "  ROSTER +%u/-%u", appeared, departed);

    // After the hash is built (so it reads the same instant the hash covers)
    // and before it is logged, so a turn's deltas sit above the hash line they
    // explain rather than under the next turn's.
    trace_state_deltas();

    log_line("LOCKSTEP", "turn %u hash rng=%016llx state=%016llx queue=%u chars=%u/%u%s",
             mine.turn,
             (unsigned long long)mine.rng_hash,
             (unsigned long long)mine.state_hash, mine.queue_depth,
             live_chars, g.cat_count, roster);

    // Remember it before comparing: if the peer's counterpart arrives later,
    // match-on-arrival in lockstep_pump needs to find this turn here.
    g.my_hash.push_evicting(mine);

    // Compare against anything the peer has already sent for this turn. A peer
    // running ahead is normal and not itself a desync -- run E measured the
    // same battle at 23,211 frames and at 12,230 -- so an unmatched hash is
    // simply kept until its counterpart arrives.
    // Every player's, not just one. A four-player battle has three counterparts
    // for this turn and any of them can be the one that diverged; stopping at
    // the first match would leave the other two unchecked.
    for (uint32_t p = 0; p < kMaxPeers; ++p) {
        if (p == net_self()) continue;
        HashMsg theirs{};
        if (g.peer_hash[p].take(mine.battle_id, mine.turn, theirs))
            report_pair(mine, theirs, (uint8_t)p);
    }

    ++g.turn;
}

uint64_t lockstep_battle_id()  { return g.active ? g.battles.current : kNoBattle; }
bool     lockstep_local_actor(){ return g.active && g.snapped && g.local_actor; }

bool lockstep_peer_owns_character(const void* character) {
    if (!g.active || !g.snapped || !character) return false;
    const uint8_t cat = cat_index_of(character);
    if (cat == kNoCat)      return false;   // a summon: nobody's, both AIs drive it
    if (!g.human_cat[cat])  return false;   // an AI cat: decided locally on both
    return !g.local_cat[cat];
}

// Called by the map layer from BOTH peers as they enter a node -- the host from
// its EnterNode hook, the client from the follow tick that drives it into the
// same node. `seed0` is MapNode+0x118's first word, so the two peers arrive at
// the same value without exchanging it, which is the whole point.
//
// Every node calls this, not just the combat ones. A shop or an event is a
// battle-less "battle" as far as identity goes, and giving it an id is what
// makes a CURSOR or a stray ACTION from the previous fight droppable while
// standing in a shop.
void lockstep_enter_battle(uint64_t seed0) {
    Guard guard;
    if (seed0 == kNoBattle) return;
    if (seed0 == g.battles.current) return;      // same node; not a transition

    const uint64_t left = g.battles.current;
    g.battles.enter(seed0);

    // The log describes the battle we just left, and nothing in it can be
    // replayed into the new one.
    g.sent_log_n    = 0;
    g.sent_log_full = false;

    log_line("LOCKSTEP", "battle id now %016llx%s", (unsigned long long)seed0,
             left == kNoBattle ? " (first this session)" : "");
}

// A peer connected or reconnected. Hand it everything this peer has already
// decided in the battle it is joining, so it can fast-forward instead of
// playing a different battle from turn 0.
//
// Sent to that peer only: a broadcast would re-inject every decision into peers
// that already applied them, and pend_take has no way to tell a replay from a
// live decision -- correctly, because there is no difference except who needs
// it.
void lockstep_catchup(uint8_t peer) {
    Guard guard;
    if (!g.active || g.battles.current == kNoBattle) return;
    if (g.sent_log_n == 0) {
        log_line("LOCKSTEP", "peer %u joined battle %016llx -- nothing to replay,"
                             " it can derive this battle from the node seed alone",
                 (unsigned)peer, (unsigned long long)g.battles.current);
        return;
    }
    if (g.sent_log_full) {
        // Loud, and deliberately not a refusal: the peer is better off in the
        // right node with an incomplete history -- where the per-turn hash will
        // catch the divergence and halt -- than left on the map with no way to
        // report anything at all.
        log_line("LOCKSTEP", "!! peer %u is joining a battle whose decision log"
                             " overflowed -- replaying the %u we still have; expect"
                             " a hash mismatch rather than a silent desync",
                 (unsigned)peer, g.sent_log_n);
    }

    uint32_t sent = 0;
    for (uint32_t i = 0; i < g.sent_log_n; ++i)
        if (net_send_action_to(peer, g.sent_log[i])) ++sent;

    log_line("LOCKSTEP", "-> replayed %u/%u decision(s) of battle %016llx to peer %u"
                         " so it can catch up to turn %u",
             sent, g.sent_log_n, (unsigned long long)g.battles.current,
             (unsigned)peer, g.turn);
}

LockstepStats lockstep_stats() {
    LockstepStats s = g.stats;
    s.pending = g.pend_count;
    return s;
}

} // namespace mgmp
