// mgmp_barrier.h -- the join barrier's decision logic, isolated so it can be
// tested without two running games.
//
// Same reasoning as mgmp_timedelay.h and mgmp_hashring.h: the hook that uses
// this can only be exercised end-to-end by getting two peers to drift apart --
// one in a battle, one still on the map -- which is a poor test loop and was
// originally found only by accident. So the part with the branches in it lives
// here as a pure function over the five facts that decide it, and the caller in
// mgmp_lockstep.cpp is left as plumbing: read the state, ask this, park or not.
//
// WHAT IT IS FOR. Neither peer may decide anything until BOTH have snapshotted
// the same battle's roster. Without that, a peer not yet in the battle is
// simply absent while the other plays on -- the late-join gap, reproduced
// 2026-08-24: the host took three actions, the client applied one, and the
// client's turn-2 rng_hash came back byte-identical to its own turn 1 because
// its stream had never run those turns. It then halted on the mismatch.
//
// The battle epoch already fixed the half where pending actions were thrown
// away at the roster reset. This is the other half, and it is the one that
// matters: keeping the actions is no use if the peer that missed them has
// already computed a hash without them.
//
// WHY WAITING IS FREE -- AND ONLY FOR HUMAN BRAINS. Brain::GetChoice is a POLL:
// it returns type=1 ("nothing decided yet") every frame while waiting on a
// person, and did so for 1695 of 1711 calls in one tutorial battle. Parking a
// HUMAN brain is therefore the exact state the game already sits in between
// clicks -- no timeout, no blocking recv, no frame budget.
//
// It is NOT free for an AI brain, and the first version of this got that
// backwards at the cost of a run. Brain::UpdateDecision calls GetChoice only
// when no decision is cached, so overwriting the one it just released makes the
// AI derive a fresh decision next frame -- and deriving draws from the
// simulation stream. A host parked for 529 polls re-derived 529 times, ran its
// stream far past the client's, and the two peers' AI then chose different
// targets from the same board:
//
//   host:    RangedAttackAbility target=(3,8) dir=(-1,0)
//   client:  RangedAttackAbility target=(7,3) dir=(0,-1)   <- the correct one
//
// turn 0 hashes matched byte for byte and turn 1 disagreed on rng alone, with
// state_hash still identical -- the signature of extra draws with no divergent
// outcome yet.
//
// So the barrier is applied to human-driven cats ONLY (see the call site in
// mgmp_lockstep.cpp). Letting the AI run ahead is safe for the reason the whole
// control split already depends on: AI decisions are deterministic and
// re-derived identically on both peers, so a peer ahead on AI turns walks the
// same turns with the same draws and the other catches up. That is also exactly
// what the late-join gap needed -- what must not happen is the host taking
// HUMAN decisions and sending them while the client is not there to receive
// them.
#pragma once

#include <cstdint>

namespace mgmp {

// Everything the decision depends on, gathered so the branches can be reasoned
// about (and tested) in one place rather than read out of a mutable global.
struct BarrierFacts {
    bool     enabled         = true;   // mgmp.json debug.join_barrier
    bool     control_checked = false;  // our roster + the peer's CONTROL agreed
    bool     snapped         = false;  // we have a roster for this battle
    bool     have_peer_ctrl  = false;  // the peer's CONTROL has arrived
    uint64_t peer_battle     = 0;      // ...and which battle it describes
    uint64_t battle          = 0;      // the battle we are in
    // Whether peer_battle is one WE have already finished. With a counter this
    // was `peer_epoch < epoch`; battle ids do not order, so the caller supplies
    // the answer from its BattleTracker. It only changes the wording of the
    // reason -- both cases hold -- but the wording is the whole diagnostic when
    // a barrier does not open.
    bool     peer_battle_retired = false;
};

// nullptr means "open -- both peers are in this battle, let brains decide".
// Otherwise a short reason, which the caller logs.
//
// The reason is not decoration. A barrier that never lifts is a stall, and a
// stall nobody can explain is precisely the failure this layer exists to
// prevent -- the first version of the control split produced exactly that, a
// battle that hung with nothing in either log to say which side was waiting.
inline const char* barrier_decide(const BarrierFacts& f) {
    // Disabled: never hold anything. Kept as the first test so that turning the
    // barrier off is unambiguously a no-op rather than a different code path.
    if (!f.enabled) return nullptr;

    // The one condition that opens it. verify_control() sets this only when we
    // have a roster AND the peer's CONTROL for the SAME epoch arrived and
    // agreed, which is exactly "both of us are here, and we concur about who
    // drives what". Checked before the diagnostics below so that an open
    // barrier costs one branch.
    if (f.control_checked) return nullptr;

    if (!f.snapped)        return "this peer has no roster yet";
    if (!f.have_peer_ctrl) return "the peer has not sent its control split";

    // Both of these are transient and neither is an error. A peer legitimately
    // ahead or behind is the normal state around a battle boundary -- run E
    // measured the same battle at 23,211 frames on one side and 12,230 on the
    // other -- so they are reported, not halted on. That distinction is the
    // same one battle identity was introduced to make.
    if (f.peer_battle != f.battle) {
        return f.peer_battle_retired
             ? "the peer's control split is for a battle we have left"
             : "the peer is in a battle we have not reached";
    }

    // Same battle but control_checked is still false: verify_control has not
    // run yet this frame, or it halted. Holding is the safe reading of both.
    return "the control split for this battle has not been checked yet";
}

} // namespace mgmp
