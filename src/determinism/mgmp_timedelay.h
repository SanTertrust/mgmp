// mgmp_timedelay.h -- the tick conversion's decision logic, isolated so it can
// be tested without a running game.
//
// The hook that uses this (h_TimeDelay in mgmp_hooks.cpp) can only be exercised
// end-to-end by fighting an AstroZombie, which is the sole ordinarily-reachable
// piece of content that uses TimeDelayStatusApplication. That is a poor test
// loop, so the part with the branches in it lives here as a pure function over
// (countdown, turn, wait) and is covered by tests/test_timedelay.cpp instead.
// The hook is then only plumbing: read the field, ask this, act.
#pragma once

#include <cstdint>

namespace mgmp {

// A value the game can never write to the countdown field. It only ever stores
// a GON-parsed `delay` there (every shipped one is < 4) and then subtracts from
// it, so anything at or above this marks a status we have taken ownership of,
// with its due turn encoded in the remainder. Keeping the state in the object
// this way means there is no side table to size, nothing to leak when a status
// is destroyed, and no way for a recycled allocation to inherit stale state.
static const double kTimeDelayManaged = 1.0e6;

enum TdAction : int {
    TD_PASSTHROUGH,   // not ours -- call the original unchanged
    TD_TAKE_OVER,     // first sight: write `value`, do NOT call the original
    TD_WAIT,          // ours and not due -- do nothing at all
    TD_FIRE,          // due: write `value` (a negative), then call the original
};

struct TdDecision {
    TdAction action;
    double   value;    // what to store at +0xF8; ignored for WAIT/PASSTHROUGH
    uint32_t due;      // for logging
};

// `v` is the raw countdown at status+0xF8, `turn` the current logical turn,
// `wait` how many turn boundaries a converted delay should wait.
//
// -1.0 is what FIRE stores: the original recomputes v - dt*rate*timescale and
// applies its statuses when that is < 0. dt is never negative, so -1 expires on
// that very call regardless of frame timing -- which is the entire point.
inline TdDecision td_decide(double v, uint32_t turn, uint32_t wait) {
    TdDecision d{ TD_PASSTHROUGH, 0.0, 0 };

    if (v >= kTimeDelayManaged) {          // already ours
        d.due = (uint32_t)(v - kTimeDelayManaged);
        if (turn < d.due) { d.action = TD_WAIT; return d; }
        d.action = TD_FIRE;
        d.value  = -1.0;
        return d;
    }

    // A non-positive countdown was never a delay: the GON key was absent (the
    // parser stores 0) or it has already expired. Vanilla behaviour is correct
    // and converting it would invent a delay the data never asked for.
    if (!(v > 0.0)) { d.action = TD_PASSTHROUGH; return d; }

    d.due   = turn + wait;
    d.value = kTimeDelayManaged + (double)d.due;
    // wait == 0 means "fire on the update it was first seen" -- still
    // deterministic, but it drops the delay rather than converting it.
    d.action = (wait == 0) ? TD_FIRE : TD_TAKE_OVER;
    if (wait == 0) d.value = -1.0;
    return d;
}

} // namespace mgmp
