// mgmp_hooks.cpp -- phase 1: observe, never interfere.
//
// Every hook logs and tail-calls the original. Nothing here changes game state,
// consumes RNG, or takes a decision. The point is a trace we can diff between
// two instances launched on the same seed (phase 2), and a first empirical look
// at the TurnAction blob that the lockstep protocol has to carry.
//
// Argument roles below follow the MSVC x64 ABI applied to the mangled
// signatures recovered from the binary. TurnAction is passed by value and is
// larger than 8 bytes, so it arrives as a hidden pointer:
//
//   Ability::trigger(TurnAction)          rcx=this   rdx=&TurnAction
//   Character::DoAction(TurnAction, bool) rcx=this   rdx=&TurnAction   r8b=bool
//   Character::BeginTurn(int)             rcx=this   edx=int
//   Brain::GetChoice() -> TurnAction      rcx=this   rdx=&sret, returned in rax
//
// GetChoice's roles were open until the first battle trace settled them: rcx
// resolves through RTTI to a real glaiel::PlayerBrain, rdx is a stack address,
// and the returned pointer equals rdx. So `this` is in rcx and the hidden
// return buffer is in rdx -- the opposite of the "sret always lands in rcx"
// rule of thumb, which only holds for free functions.
//
// GetChoice is a *poll*, not a decision point. PlayerBrain::GetChoice is called
// over and over while the game waits for input, returning type=1 ("no decision
// yet") every time; 1695 of 1711 observed calls were that. Logging all of them
// buries the battle. So by default only decided results are logged, and
// Character::DoAction -- which fired exactly 12 times for 12 decisions -- is the
// real command boundary for lockstep.
#include "mgmp_ability.h"
#include "mgmp_addresses.h"
#include "mgmp_catsync.h"
#include "mgmp_combatlock.h"
#include "mgmp_cursor.h"
#include "mgmp_choice.h"
#include "mgmp_overlay.h"
#include "mgmp_invsync.h"
#include "mgmp_aim.h"
#include "mgmp_nodehash.h"
#include "mgmp_runhist.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_timedelay.h"
#include "mgmp_hooks.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_record.h"
#include "mgmp_replay.h"
#include "mgmp_resolve.h"
#include "mgmp_session.h"
#include "mgmp_lockstep.h"
#include "mgmp_follow.h"
#include "mgmp_savefile.h"
#include "mgmp_leave.h"
#include "mgmp_rng.h"
#include "mgmp_rtti.h"
#include "mgmp_turnaction.h"

#include <windows.h>
#include "MinHook.h"

#include <cstdio>
#include <cstring>
#include <intrin.h>   // _ReturnAddress

namespace mgmp {
namespace {

uintptr_t g_base = 0;

// Which detours are actually live. Written only by hooks_install, read by
// hooks_is_live -- see the header for why "the config asked for it" is a
// different and weaker claim.
bool g_live[T_COUNT] = {};

typedef void  (__fastcall* fn_this)(void* self);
typedef void  (__fastcall* fn_this_i32)(void* self, int arg);
typedef void  (__fastcall* fn_ptr_i32)(void* a, int b);
typedef void  (__fastcall* fn_this_ptr)(void* self, void* ta);
typedef void  (__fastcall* fn_this_ptr_b)(void* self, void* ta, unsigned char flag);
typedef void* (__fastcall* fn_two_ptr)(void* a, void* b);
typedef void  (__fastcall* fn_ptr_i32_b)(void* a, int b, unsigned char c);
typedef char  (__fastcall* fn_this_c)(void* self);

fn_this       o_InitSystems = nullptr;
fn_this       o_NextTurn    = nullptr;
fn_two_ptr    o_GetChoice   = nullptr;
fn_this_ptr_b o_DoAction    = nullptr;
fn_this_ptr   o_Trigger     = nullptr;
fn_this_i32   o_BeginTurn   = nullptr;
fn_this       o_EndTurn     = nullptr;
fn_this       o_FrameBegin  = nullptr;
fn_this_ptr   o_ApplyAction = nullptr;
fn_this_ptr   o_QueueDecision = nullptr;
fn_ptr_i32    o_SaveScum      = nullptr;
fn_this       o_TimeDelay     = nullptr;
fn_this       o_MapUpdate     = nullptr;
fn_this_ptr   o_EnterNode     = nullptr;
fn_this       o_StatusMenu    = nullptr;
fn_this       o_SaveSelUpdate = nullptr;
fn_ptr_i32_b  o_SaveSlotClick = nullptr;
fn_two_ptr    o_MewDirInit    = nullptr;
fn_this       o_EventChoice   = nullptr;   // sub_140937F30(capture*)
fn_this       o_EventUpdate   = nullptr;
fn_this_ptr   o_LevelSelect   = nullptr;   // select_option(this, LevelUpOption*)
fn_this       o_LevelUpdate   = nullptr;
fn_this       o_CombatMenu    = nullptr;
fn_this       o_ButtonUpdate  = nullptr;
// Two pointers, not one: UpdateDecision RETURNS a TurnAction by value, so the
// caller passes the sret buffer in rdx (`lea rdx, [sret]` @ 0x1408DF2EF, one
// instruction before the call). Typed as a one-argument function, the detour
// clobbers rdx before the trampoline runs and the game writes 0x88 bytes --
// a whole TurnAction, std::function and all -- over whatever rdx happened to
// hold. Same ABI as GetChoice, and for the same reason.
fn_two_ptr    o_UpdateDecision = nullptr;
fn_this_c     o_HighlightRefresh = nullptr;

// MewSaveFile::Store / ::Load. Both are (this, std::string key BY VALUE,
// ByteStream&); the return value is the key destructor's and no caller reads
// it, which is what makes suppressing them possible at all.
typedef void* (__fastcall* fn_three_ptr)(void* a, void* b, void* c);
fn_three_ptr  o_SFStoreBlob   = nullptr;
fn_three_ptr  o_SFLoadBlob    = nullptr;

// TurnControl+0x60 is the live count of queued decisions, read off the drain
// loop in TurnControl::update (ring at +0x48, capacity at +0x50, head at +0x58,
// count at +0x60). Recorded rather than derived because a queue that grows in
// one run and not another is the cheapest desync signal we have.
uint32_t queue_depth(const void* turn_control) {
    if (!turn_control) return 0;
    uint64_t n = 0;
    if (!mem_read((const uint8_t*)turn_control + 0x60, &n, sizeof(n))) return 0;
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
}

// ---- shared formatting helpers -------------------------------------------

// "cls=glaiel::MoveAbility" plus, when pointers=1, " this=00007FF6..."
struct Who {
    char text[320];
    Who(const void* obj, const char* field) {
        char cls[192];
        rtti_class_name(obj, cls, sizeof(cls));
        if (tune::kPointers)
            _snprintf_s(text, sizeof(text), _TRUNCATE, "cls=%s %s=%p", cls, field, obj);
        else
            _snprintf_s(text, sizeof(text), _TRUNCATE, "cls=%s", cls);
    }
};

struct TaDump {
    char text[1200];
    explicit TaDump(const void* ta) {
        format_turn_action(text, sizeof(text), ta, tune::kTaRaw);
    }
};

// ---- hooks ---------------------------------------------------------------

void __fastcall h_InitSystems(void* self) {
    log_line("INIT", "ApplicationBase::initSystems enter");
    o_InitSystems(self);
    log_line("INIT", "ApplicationBase::initSystems leave");
}

void __fastcall h_NextTurn(void* self) {
    uint32_t t = log_bump_turn();
    // Before the original: snapshot the roster on the first boundary of a
    // battle, then exchange this turn's hash while the queue is still the one
    // the turn ended with.
    if (lockstep_active()) lockstep_turn_boundary(self);
    Who      w(self, "tc");
    log_line("NEXTTURN", ">>> turn %u begins  %s", t, w.text);

    // Also the flush point for the binary stream: a turn boundary is the one
    // place in a turn-based game where a few hundred microseconds of file I/O
    // cannot be mistaken for a frame-pacing effect.
    uint64_t total = 0, global = 0;
    rng_counters(&total, &global);
    record_turn(t, self, total, global);
    if (record_active()) {
        uint32_t distinct = 0; bool overflow = false;
        uint64_t st = 0, hp = 0, im = 0, tl = 0;
        record_stream_stats(&distinct, &overflow, &st, &hp, &im, &tl);
        uint32_t unknown = record_unknown_slots();
        log_line("RNG", "turn %u: %llu draws | tls+0x178=%llu other_tls=%llu"
                        " stack=%llu heap=%llu image=%llu | %u distinct streams%s%s",
                 t, (unsigned long long)total, (unsigned long long)global,
                 (unsigned long long)tl, (unsigned long long)st,
                 (unsigned long long)hp, (unsigned long long)im, distinct,
                 overflow ? " (TABLE OVERFLOWED)" : "",
                 unknown ? " (UNRESOLVED ABILITY SLOTS)" : "");
        if (unknown)
            log_line("SLOT", "%u action(s) so far named an ability the actor does not"
                             " own -- replay cannot reproduce those", unknown);
    }

    o_NextTurn(self);
    log_line("NEXTTURN", "<<< turn %u dispatched", t);
}

void* __fastcall h_GetChoice(void* self, void* out) {
    void* ret = o_GetChoice(self, out);

    // --- run B: inject the recorded decision -----------------------------
    //
    // The original still runs first. It is a poll with no RNG of its own
    // (Brain::GetChoice has zero TLS loads), but it does its own bookkeeping,
    // and overwriting its result is a smaller intervention than skipping it.
    //
    // Two cases, and the second matters as much as the first:
    //   - we have a decision to inject -> overwrite the buffer with it;
    //   - we have one outstanding but not yet applied -> force type=1, so a
    //     stray click during replay cannot queue a second decision on top of
    //     the one already in flight.
    // Lockstep and replay are alternatives, never both: one injects from a
    // socket, the other from a file, and they would fight over the same buffer.
    if (lockstep_active()) {
        if (lockstep_fill_choice(self, ret)) return ret;
    } else if (replay_active()) {
        if (replay_fill_choice(self, ret)) {
            TaDump d(ret);
            log_line("REPLAY", "injected %s", d.text);
            return ret;
        }
        if (replay_outstanding()) {
            uint32_t none = TA_None;
            mem_write(ret, &none, sizeof(none));
        }
    }

    // Suppress the poll. Without this a single tutorial battle produced 3422
    // CHOICE lines against 12 real actions.
    static LONG volatile polls = 0;
    TurnAction a{};
    bool       decided = mem_read(ret, &a, sizeof(a)) && a.type != TA_None;

    if (!decided && !tune::kChoiceAll) {
        InterlockedIncrement(&polls);
        return ret;
    }

    LONG suppressed = InterlockedExchange(&polls, 0);
    Who    w(self, "brain");
    TaDump d(ret);
    if (suppressed)
        log_line("CHOICE", "%s %s (after %ld polls)", w.text, d.text, suppressed);
    else
        log_line("CHOICE", "%s %s", w.text, d.text);
    return ret;
}

void __fastcall h_DoAction(void* self, void* ta, unsigned char flag) {
    Who    w(self, "char");
    TaDump d(ta);
    log_line("DOACTION", "%s flag=%u %s", w.text, (unsigned)flag, d.text);
    o_DoAction(self, ta, flag);
}

void __fastcall h_Trigger(void* self, void* ta) {
    Who    w(self, "abil");
    TaDump d(ta);
    log_line("TRIGGER", "%s %s", w.text, d.text);
    o_Trigger(self, ta);
}

void __fastcall h_BeginTurn(void* self, int arg) {
    Who w(self, "char");
    log_line("BEGINTURN", "%s arg=%d", w.text, arg);
    o_BeginTurn(self, arg);
}

void __fastcall h_EndTurn(void* self) {
    Who w(self, "char");
    log_line("ENDTURN", "%s", w.text);
    o_EndTurn(self);
}

void __fastcall h_ApplyAction(void* self, void* ta) {
    TaDump d(ta);
    log_line("APPLY", "%s", d.text);

    if (record_active()) {
        TurnAction a{};
        if (mem_read(ta, &a, sizeof(a))) {
            EvAction e{};
            e.turn        = log_turn();
            e.type        = a.type;
            e.target_x    = a.target_x;
            e.target_y    = a.target_y;
            e.dir_x       = a.dir_x;
            e.dir_y       = a.dir_y;
            e.ability_ptr = (uint64_t)a.ability;
            e.actor_ptr   = (uint64_t)a.actor;
            e.ability_cls = record_intern_class(a.ability);
            e.actor_cls   = record_intern_class(a.actor);
            e.queue_depth = queue_depth(self);
            e.b30         = a.tail[0x30 - 0x28];
            e.b31         = a.tail[0x31 - 0x28];

            // The identity the replayer will actually use. TurnAction+0x20
            // arrives NULL from the brain -- Character::DoAction fills it in,
            // and that runs *after* ApplyTurnAction -- so the actor has to come
            // from TurnControl itself here. Prefer the action's own actor when
            // it happens to be set (type 6 reaction broadcasts carry one).
            const void* actor = a.actor ? a.actor : current_actor(self);
            AbilitySlot slot  = ability_slot_of(actor, a.ability);
            e.slot_kind  = slot.kind;
            e.slot_index = slot.index;
            if (slot.kind == SLOT_UNKNOWN) record_note_unknown_slot();

            char gon[64];
            if (ability_gon_name(a.ability, gon, sizeof(gon)))
                e.ability_name = record_intern_name(gon);

            // Character+0x68 is the Brain (TurnControl::update reads exactly
            // this to call UpdateDecision). Its class separates the human's
            // decisions from the AI's, and only the human's are replayable.
            const void* brain = nullptr;
            if (actor && mem_read((const uint8_t*)actor + 0x68, &brain, sizeof(brain)))
                e.brain_cls = record_intern_class(brain);

            // +0x04 is deliberately absent: it is uninitialized padding and
            // would differ between runs for no reason. See mgmp_turnaction.h.
            record_action(e);
        }
    }

    // Advance and validate the replay FIFO. Done before the original runs, so
    // the comparison sees the action exactly as it was recorded -- ApplyTurnAction
    // mutates the slot at TurnControl+0x90 on its way through.
    if (replay_active())   replay_on_applied(ta, current_actor(self));
    if (lockstep_active()) lockstep_on_applied(ta, current_actor(self));

    o_ApplyAction(self, ta);
}

// The push side of the ring. Fires for every deferred reaction a passive or
// status queues, which is where types 6 and 7 come from -- neither ever
// originates in a brain, so neither goes on the wire, but both are derived from
// proc rolls and so both diverge the instant an RNG stream does.
void __fastcall h_QueueDecision(void* self, void* ta) {
    if (record_active()) {
        uint32_t type = 0;
        if (mem_read(ta, &type, sizeof(type)) && type > TA_None) {
            // type == 1 is the brain's "nothing decided yet" poll. QueueDecision
            // rejects it internally, so it never reaches the ring -- but the call
            // still happens, once per frame, for as long as the player is
            // thinking. Run A recorded 3787 of them against 30 real pushes.
            //
            // They are not just volume: the count is a function of how long a
            // human took to move and of the frame rate, so it differs between
            // any two runs by construction. Recording them would make every
            // diff fail on the first turn for a reason that means nothing.
            EvQueue q{};
            q.turn = log_turn();
            q.type = type;
            // Depth is sampled before the push, so +1 is what the ring will
            // hold once the original returns.
            q.depth_after = queue_depth(self) + 1;
            uintptr_t ret = (uintptr_t)_ReturnAddress();
            q.site = (ret >= g_base && (ret - g_base) <= 0xFFFFFFFFull)
                         ? (uint32_t)(ret - g_base) : 0;
            record_queue(q);
        }
    }
    o_QueueDecision(self, ta);
}

// The one hook that changes the game instead of watching it.
//
// sub_1408DD9C0 is the save-scum penalty: it increments the scum counter at
// RunState+0xE0, walks the cat roster applying per-cat penalties, and queues
// the Steven NPC scripts. Swallowing the call removes all three.
//
// This exists for the capture methodology, not for convenience. Repeating a
// battle means reloading the same save repeatedly, and the penalty MUTATES CAT
// STATE AS A FUNCTION OF HOW MANY TIMES YOU RELOADED -- so with it live, run N
// and run N+1 start from different states by construction and no A/B capture
// can be trusted. It is a determinism hazard for the instrument, in exactly the
// way an unfenced RNG stream is.
//
// It is off by default. A capture taken with `hook_savescum = 1` is not a
// capture of the shipped game, so the banner says so out loud.
// ---------------------------------------------------------------------------
// hook_timedelay -- convert TimeDelayStatusApplication from a wall-clock
// countdown to a turn countdown.
//
// This is the one place in the battle sim measured to advance on real time.
// Slot 11 does, per update:
//
//     [this+0xF8] -= dt * this->rate * scene->timescale;
//     if ([this+0xF8] < 0) apply_the_statuses();
//
// and does nothing whatsoever when the countdown is still positive (the
// not-expired branch jumps straight to the epilogue -- verified, it is
// `add rsp,0B0h / pop x5 / retn`). So the whole function is that one statement,
// which is what makes it safe to intercept: when the effect is not due we can
// simply not call the original, and when it is due we hand control back so the
// REAL status-application code runs, untouched.
//
// Why it has to change at all: two peers in turn-lockstep run at different
// frame rates -- run E measured the same battle at 23,211 frames and at 12,230,
// with turns lasting 2.6x to 5.4x longer on one side. Three seconds of dt
// therefore spans a different number of TURNS on each peer, so the AstroZombie's
// delayed Cleanse+FullHeal lands before one peer's next action and after the
// other's. Everything downstream of that desyncs.
//
// The conversion: wait `timedelay_turns` turn boundaries instead of N seconds.
// A turn boundary is the coarsest thing both peers already agree on by
// construction, and the shipped delays (.1, .25, 1.13333, 3 seconds) are all
// shorter than a turn, so "the next turn boundary" is the nearest deterministic
// reading of what the data was asking for.
//
// The branch logic lives in mgmp_timedelay.h as a pure function so it can be
// tested without a running game -- the only ordinarily-reachable content that
// exercises this path is one miniboss. Everything below is plumbing.

void __fastcall h_TimeDelay(void* self) {
    if (!self) { if (o_TimeDelay) o_TimeDelay(self); return; }

    double v = 0.0;
    if (!mem_read((const uint8_t*)self + 0xF8, &v, sizeof(v))) {
        if (o_TimeDelay) o_TimeDelay(self);          // unreadable: do no harm
        return;
    }

    const uint32_t turn = log_turn();
    const TdDecision d  = td_decide(v, turn, tune::kTimeDelayTurns);

    switch (d.action) {
    case TD_WAIT:
        return;                                       // the original would only
                                                      // have decremented
    case TD_PASSTHROUGH:
        if (o_TimeDelay) o_TimeDelay(self);
        return;

    case TD_TAKE_OVER:
        if (!mem_write((uint8_t*)self + 0xF8, &d.value, sizeof(d.value))) {
            if (o_TimeDelay) o_TimeDelay(self);       // could not take over
            return;
        }
        log_line("TDELAY", "converted %.5fs -> %u turn(s): turn %u, due %u, status=%p",
                 v, tune::kTimeDelayTurns, turn, d.due, self);
        return;

    case TD_FIRE:
        mem_write((uint8_t*)self + 0xF8, &d.value, sizeof(d.value));
        log_line("TDELAY", "firing at turn %u (due %u) status=%p", turn, d.due, self);
        if (o_TimeDelay) o_TimeDelay(self);
        return;
    }
}

void __fastcall h_SaveScum(void* run, int mode) {
    static LONG volatile swallowed = 0;
    LONG n = InterlockedIncrement(&swallowed);
    uint32_t counter = 0;
    if (run) mem_read((const uint8_t*)run + 0xE0, &counter, sizeof(counter));
    log_line("SCUM", "swallowed save-scum penalty #%ld (mode=%d, counter stays at %u)",
             n, mode, counter);
    // Deliberately does NOT call o_SaveScum.
}

// --- phase 5: following the host through the map ---------------------------

void __fastcall h_EnterNode(void* self, void* node) {
    // The client's own clicks are swallowed here; the host's are published.
    // Note there is no "am I replaying this" flag: the injection below calls
    // o_EnterNode, the MinHook trampoline, which bypasses this detour outright.
    bool sent = false;
    if (!follow_on_enter_node(self, node, &sent)) return;
    o_EnterNode(self, node);
}

void __fastcall h_MapUpdate(void* self) {
    o_MapUpdate(self);

    // After the original, not before: the node the host chose should be entered
    // from a map screen that has already finished this frame's update, which is
    // as close as we can get to the native call site (a UI callback fired from
    // inside this same function).
    if (!o_EnterNode) return;
    if (void* node = follow_map_update(self))
        o_EnterNode(self, node);
}

// --- the decision screens --------------------------------------------------
//
// Four hooks, two jobs. The two commit functions both CAPTURE the host's pick
// and SWALLOW the client's -- one splice does both because they are the same
// choke point. The two update ticks exist only so that a choice which arrives
// before this peer's screen is up has somewhere to land.
//
// The commit hooks are the only place in the mod that can DECLINE to run the
// original, and that is the whole point on the client: a click the local player
// made on a screen the host owns must not happen at all.

void __fastcall h_EventChoice(void* cap) {
    if (choice_on_event_commit(cap)) o_EventChoice(cap);
}

void __fastcall h_EventUpdate(void* self) {
    o_EventUpdate(self);
    // After the original, for the same reason h_MapUpdate follows o_MapUpdate:
    // the native click arrives from a UI callback fired inside this update, so
    // injecting here is at the same point in the frame rather than ahead of the
    // screen's own bookkeeping.
    choice_on_event_update(self);
    // ...and the node hash's second sample point. After the choice tick so that
    // an injected choice has already happened: the name at WorldEvent+0x1A10 is
    // written by init and does not change, but sampling last keeps this hook's
    // one ordering rule -- observation follows action -- rather than making the
    // reader work out that it does not matter here.
    nodehash_on_event_screen(self);
}

void __fastcall h_LevelSelect(void* self, void* option) {
    if (choice_on_level_select(self, option)) o_LevelSelect(self, option);
}

void __fastcall h_LevelUpdate(void* self) {
    o_LevelUpdate(self);
    choice_on_level_update(self);
}

// --- peer cursors ----------------------------------------------------------

void __fastcall h_StatusMenu(void* self) {
    o_StatusMenu(self);

    // After the original, for two reasons that happen to point the same way.
    // The hovered tile at StatusMenu+124 is written BY this function, so before
    // it we would be reading last frame's; and the immediate-mode pieces it
    // submits are this frame's, so ours belong in the same batch rather than
    // trailing the previous one.
    cursor_on_status_menu(self);
}

// --- QoL: the combat menu greys out on a cat this peer does not own --------

void __fastcall h_CombatMenu(void* self) {
    // AROUND the original, not after it, and that is the whole point of the
    // pair. combatlock_enter opens a scope that h_ButtonUpdate acts inside; the
    // original is what walks the bar and calls each button's update, which is
    // the only moment between "the game decided this button's state" and "the
    // game drew it". See mgmp_combatlock.h.
    combatlock_enter(self);
    o_CombatMenu(self);
    combatlock_leave();
}

// The status half of the ability highlight. Swallowed while mgmp_aim is drawing
// another player's aim on a cat this peer does not own -- see the block under
// kCalls in mgmp_addresses.h for what it does and what it cost.
//
// Same scoped shape as h_ButtonUpdate below: outside the window this is one
// load and one branch, and the guard is raised for the duration of a single
// call into the game.
char __fastcall h_HighlightRefresh(void* self) {
    if (aim_highlight_suppressed()) return 0;
    return o_HighlightRefresh(self);
}

void __fastcall h_ButtonUpdate(void* self) {
    // Fires for every button in the game. Outside a combat-menu tick this is a
    // load and a branch: combatlock_on_button's first test is the scope flag.
    combatlock_on_button(self);
    o_ButtonUpdate(self);

    // AFTER the original, and this one has to be. Button::update recomputes the
    // button's state and pushes it into the SWF before it returns, and
    // Button::Click's own guards read that state -- clicking first would test
    // the previous frame's. It is also where the game's own click would have
    // landed: update dispatches at the bottom of its hover branch.
    //
    // Same trampoline reasoning as everywhere else: Button::Click is a call
    // target, not a hook, so nothing here re-enters this detour.
    savefile_on_button_update(self);

    // The other half of the same idea: Play on the main menu gets a client
    // INTO the host's run, Quit To Menu in the pause sidebar gets it back
    // OUT when the host has left. Same slot, same trampoline reasoning.
    leave_on_button_update(self);
}

// The aim preview turns the acting cat while a decision is held, writing the
// same Character+0x388 the backstab test reads -- under a wall-clock gate, so
// two peers at different frame rates disagree about damage. Snapshot the field,
// let the preview do whatever it likes, put it back. Inert outside a session.
//
// `out` is the caller's TurnAction sret buffer. It is not read or written here
// -- it exists in the signature so that rdx and the returned rax survive the
// detour untouched.
void* __fastcall h_UpdateDecision(void* self, void* out) {
    lockstep_preview_facing_begin(self);
    void* ret = o_UpdateDecision(self, out);
    lockstep_preview_facing_end();

    // AFTER the original, deliberately. The game's own aim preview draws from
    // inside it, so drawing the peer's from here puts the two on the same
    // frame, in the same order, at the same layer -- and on the peer that does
    // NOT own this cat the original drew nothing anyway, because GetChoice was
    // overwritten with "nothing decided" and the type-2 guard never passed.
    aim_on_update_decision(self);
    return ret;
}


// --- phase 5: the client never picks a save file ---------------------------

void __fastcall h_SaveSlotClick(void* ss, int slot, unsigned char play_sound) {
    // Same shape as h_EnterNode, and for the same reason: the client's own pick
    // is swallowed here, and the auto-select below calls o_SaveSlotClick -- the
    // MinHook trampoline -- which bypasses this detour outright, so no
    // "am I injecting" flag is needed.
    if (!savefile_on_slot_click(ss, slot)) return;
    o_SaveSlotClick(ss, slot, play_sound);
}

void __fastcall h_SaveSelUpdate(void* self) {
    o_SaveSelUpdate(self);

    // After the original: the native click arrives from a Button callback fired
    // inside this same update, so entering the slot once it has returned is the
    // same point in the frame rather than a reentrant call into a half-updated
    // screen.
    if (!o_SaveSlotClick) return;
    int slot = savefile_autoselect(self);
    if (slot >= 0) o_SaveSlotClick(self, slot, 0);
}

// The client loads the host's run from its own file rather than from the slot
// the host was playing, so the player's real saves survive a co-op session.
// MewDirector::init takes its filename BY VALUE -- a hidden pointer the callee
// reads and does not own -- so handing it a std::string of ours is exactly as
// valid as the vector element the game would have passed.
void* __fastcall h_MewDirInit(void* self, void* name) {
    if (const void* sub = savefile_redirect_load())
        return o_MewDirInit(self, (void*)sub);
    return o_MewDirInit(self, name);
}

// The two inventory blob accessors. Both bodies are one predicted branch on a
// flag that only mgmp_invsync's own calls ever set -- every OTHER blob in the
// save file passes through here too, and rewriting those would be rewriting the
// save. When the intercept returns true it has already destroyed the key
// string, because the function we are standing in for would have.
//
// Returning null is safe: sub_14022C4D0 and sub_14022C620 both tail into
// std::string::_Tidy_deallocate, whose return value is junk, and no call site
// of either reads it.
void* __fastcall h_SFStoreBlob(void* self, void* key, void* bs) {
    if (invsync_intercept_store(key, bs)) return nullptr;
    return o_SFStoreBlob(self, key, bs);
}

void* __fastcall h_SFLoadBlob(void* self, void* key, void* bs) {
    if (invsync_intercept_load(key, bs)) return nullptr;
    return o_SFLoadBlob(self, key, bs);
}

void __fastcall h_FrameBegin(void* self) {
    static LONG volatile frames = 0;
    LONG n = InterlockedIncrement(&frames);
    uint32_t every = tune::kFrameLogEvery;
    if (every && (n == 1 || (n % (LONG)every) == 0))
        log_line("FRAME", "frame %ld", n);

    // Frame markers are what let Run D ask its question: with identical actions
    // and a deliberately stalled frame rate, does the draw sequence between two
    // actions change? Off by default because on a clean recording they are just
    // volume.
    if (tune::kRecordFrames) record_frame((uint32_t)n);

    // The network is pumped here rather than at a turn boundary so messages
    // keep arriving while a brain is polled -- a peer's decision has to be able
    // to land in the middle of our wait for it, not only between turns.
    session_update();

    o_FrameBegin(self);
}

// ---- installation --------------------------------------------------------

struct Binding {
    Target target;
    void*  detour;
    void** original;
};

const Binding kBindings[] = {
    { T_InitSystems, (void*)&h_InitSystems, (void**)&o_InitSystems },
    { T_NextTurn,    (void*)&h_NextTurn,    (void**)&o_NextTurn    },
    { T_GetChoice,   (void*)&h_GetChoice,   (void**)&o_GetChoice   },
    { T_DoAction,    (void*)&h_DoAction,    (void**)&o_DoAction    },
    { T_Trigger,     (void*)&h_Trigger,     (void**)&o_Trigger     },
    { T_BeginTurn,   (void*)&h_BeginTurn,   (void**)&o_BeginTurn   },
    { T_EndTurn,     (void*)&h_EndTurn,     (void**)&o_EndTurn     },
    { T_FrameBegin,  (void*)&h_FrameBegin,  (void**)&o_FrameBegin  },
    { T_ApplyAction, (void*)&h_ApplyAction, (void**)&o_ApplyAction },

    { T_QueueDecision, (void*)&h_QueueDecision, (void**)&o_QueueDecision },

    { T_MapUpdate,   (void*)&h_MapUpdate,   (void**)&o_MapUpdate   },
    { T_EnterNode,   (void*)&h_EnterNode,   (void**)&o_EnterNode   },

    { T_StatusMenuUpdate, (void*)&h_StatusMenu, (void**)&o_StatusMenu },

    { T_EventChoice,  (void*)&h_EventChoice, (void**)&o_EventChoice },
    { T_EventUpdate,  (void*)&h_EventUpdate, (void**)&o_EventUpdate },
    { T_LevelSelect,  (void*)&h_LevelSelect, (void**)&o_LevelSelect },
    { T_LevelUpdate,  (void*)&h_LevelUpdate, (void**)&o_LevelUpdate },

    { T_SaveSelUpdate,  (void*)&h_SaveSelUpdate,  (void**)&o_SaveSelUpdate  },
    { T_SaveSlotClick,  (void*)&h_SaveSlotClick,  (void**)&o_SaveSlotClick  },
    { T_MewDirectorInit,(void*)&h_MewDirInit,     (void**)&o_MewDirInit     },

    { T_SFStoreBlob,    (void*)&h_SFStoreBlob, (void**)&o_SFStoreBlob },
    { T_SFLoadBlob,     (void*)&h_SFLoadBlob,  (void**)&o_SFLoadBlob  },

    { T_CombatMenuUpdate, (void*)&h_CombatMenu,   (void**)&o_CombatMenu   },
    { T_ButtonUpdate,     (void*)&h_ButtonUpdate, (void**)&o_ButtonUpdate },
    { T_HighlightRefresh, (void*)&h_HighlightRefresh, (void**)&o_HighlightRefresh },
    { T_UpdateDecision,   (void*)&h_UpdateDecision, (void**)&o_UpdateDecision },

    { T_SaveScumPenalty, (void*)&h_SaveScum, (void**)&o_SaveScum },
    { T_TimeDelayTick,   (void*)&h_TimeDelay, (void**)&o_TimeDelay },

    // Detours and originals live in mgmp_rng.cpp so the hot path is compiled
    // without the tracing machinery this file pulls in.
    { T_RandInt,   nullptr, nullptr },
    { T_RandFloat, nullptr, nullptr },
    { T_Rand2,     nullptr, nullptr },
    { T_RollChance,nullptr, nullptr },
};

// Resolves the three RNG bindings, which are not static initialisers because
// their detours are defined in another translation unit.
bool rng_binding(Target t, void** detour, void*** original) {
    switch (t) {
    case T_RandInt:   *detour = rng_detour_randint();   *original = rng_original_randint();   return true;
    case T_RandFloat: *detour = rng_detour_randfloat(); *original = rng_original_randfloat(); return true;
    case T_Rand2:     *detour = rng_detour_rand2();     *original = rng_original_rand2();     return true;
    case T_RollChance:*detour = rng_detour_rollchance();*original = rng_original_rollchance();return true;
    default: return false;
    }
}

// Install one binding. Returns true only if it went live on THIS call, so a
// caller can count what it actually added.
//
// `announce_off` separates the two passes. The startup pass lists every hook it
// did not install, because that banner is meant to be a complete inventory of
// what this process is running. The late pass (hooks_install_late) is only
// interested in what it managed to add -- repeating the "off by default" lines
// there would print the same inventory twice.
bool install_one(const Binding& b, bool announce_off) {
    const TargetDesc& t = kTargets[b.target];

    // Already live. The late pass walks the whole table, so this is the normal
    // case there rather than an error.
    if (b.target >= 0 && b.target < T_COUNT && g_live[b.target]) return false;

    if (!config().hook[b.target]) {
        if (announce_off) {
            // The RNG hooks default off and are implied by debug.record, so
            // a flat "disabled" was misleading for them -- it read as if
            // something had been turned off when it had not.
            bool is_rng = (b.target == T_RandInt || b.target == T_RandFloat ||
                           b.target == T_Rand2);
            log_raw("  [-] %-9s %s (%s)", t.name, t.symbol,
                    is_rng && !config().record ? "off: needs debug.record"
                                               : "off by default");
        }
        return false;
    }

    void*  detour   = b.detour;
    void** original = b.original;
    rng_binding(b.target, &detour, &original);
    if (!detour || !original) {
        log_raw("  [!] %-9s no detour bound", t.name);
        return false;
    }

    // Resolved by signature, not by RVA. Zero means it did not resolve at
    // all, and resolve_init has already said so and decided whether that
    // was survivable -- here it is simply a hook we do not install.
    void* addr = (void*)addr_of(b.target);
    if (!addr) {
        log_raw("  [!] %-9s unresolved -- not hooking", t.name);
        return false;
    }
    MH_STATUS s = MH_CreateHook(addr, detour, original);
    if (s != MH_OK) {
        log_raw("  [!] %-9s MH_CreateHook failed (%d) at %p", t.name, (int)s, addr);
        return false;
    }
    s = MH_EnableHook(addr);
    if (s != MH_OK) {
        log_raw("  [!] %-9s MH_EnableHook failed (%d) at %p", t.name, (int)s, addr);
        return false;
    }
    log_raw("  [+] %-9s %p  %s", t.name, addr, t.symbol);
    if (b.target >= 0 && b.target < T_COUNT) g_live[b.target] = true;
    return true;
}

} // namespace

bool hooks_verify_module(uintptr_t base, char* err, size_t err_size) {
    g_base = base;

    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    uint32_t size_of_image = nt->OptionalHeader.SizeOfImage;

    // A DIFFERENT BUILD IS NO LONGER A REASON TO REFUSE.
    //
    // This used to return false here, which meant that on the day the game
    // patched, the mod would not even attempt to load -- the signature
    // machinery below would never get to run, and the one mechanism built to
    // survive an update would be skipped by the check guarding it. The size is
    // still worth stating, because it tells the reader of a log which build
    // produced it, but it decides nothing.
    if (size_of_image != kExpectedSizeOfImage) {
        log_raw("[~] SizeOfImage 0x%X != the pinned 0x%X -- this is NOT the build "
                "the signatures were generated from. Resolving by signature.",
                size_of_image, kExpectedSizeOfImage);
    }

    // Everything now hangs off this: it resolves all 49 addresses, reports any
    // that moved, and returns false only when something lockstep cannot be
    // correct without has gone missing entirely.
    if (!resolve_init(base)) {
        _snprintf_s(err, err_size, _TRUNCATE,
                    "a critical hook target could not be resolved by signature");
        return false;
    }
    return true;
}

int hooks_install() {
    if (MH_Initialize() != MH_OK) {
        log_raw("  [!] MH_Initialize failed");
        return -1;
    }

    rng_set_base(g_base);
    catsync_set_base(g_base);
    invsync_set_base(g_base);
    runhist_set_base(g_base);
    nodehash_set_base(g_base);
    aim_set_base(g_base);
    lockstep_set_base(g_base);
    cursor_set_base(g_base);
    choice_set_base(g_base);
    savefile_set_base(g_base);
    leave_set_base(g_base);
    overlay_set_base(g_base);

    int installed = 0;
    for (const Binding& b : kBindings)
        if (install_one(b, /*announce_off=*/true)) ++installed;

    // After every hook is known, not before: a feature that is only safe while
    // another hook runs has to be told, and aim_set_base ran above -- before
    // anything was installed -- so it could not have asked then.
    aim_on_hooks_installed();
    return installed;
}

int hooks_install_late() {
    int installed = 0;
    for (const Binding& b : kBindings)
        if (install_one(b, /*announce_off=*/false)) ++installed;

    // Re-asked for the same reason it is asked at startup, and it MATTERS here:
    // T_HighlightRefresh is one of the hooks this pass exists to add, and
    // mgmp_aim refuses to call the ability highlight without it. Skipping this
    // would leave the aim preview permanently disarmed in exactly the session
    // that just fixed its hooks.
    if (installed) aim_on_hooks_installed();
    return installed;
}

bool hooks_is_live(int target) {
    return target >= 0 && target < T_COUNT && g_live[target];
}

void hooks_uninstall() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

} // namespace mgmp
