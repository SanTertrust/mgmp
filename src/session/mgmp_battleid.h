// mgmp_battleid.h -- which battle a message belongs to.
//
// Battle identity is the 64-bit node seed the battle was entered with, read
// out of MapNode+0x118 by both peers as they enter the same node. See the long
// note above ActionMsg in mgmp_proto.h for why it is that and not a counter;
// the short version is that a counter cannot survive one peer restarting, and
// this project has already had to undo per-process identity three times
// (ability pointers, item ids, and now the epoch).
//
// What a counter gave for free and an id does not is ORDER. `epoch < ours` was
// how a message from a battle we had left was told apart from one for a battle
// we had not reached, and those two need opposite handling: the first is
// dropped, the second is HELD. So the receiver remembers the ids it has
// retired, and that set is the only thing separating the two cases.
//
// Pure logic, no game state, so it is unit-tested directly -- tests/test_battleid.cpp.
// The convention this file follows is the one td_decide, HashRing and
// barrier_decide already follow, and it has the same limit: these tests pin
// what the functions RETURN, never where they are called from.

#pragma once

#include <cstdint>

namespace mgmp {

// Zero is "no battle" -- a peer on the map, in a shop, at a menu. It is never
// a real node seed in practice, and treating it as a battle would make every
// out-of-battle peer look like it shared one.
constexpr uint64_t kNoBattle = 0;

enum class BattleRel {
    Ours,      // the battle this peer is in right now
    Retired,   // one this peer has finished and left -- DROP
    Unknown,   // not ours and not retired: the sender is somewhere we are not.
               // HOLD. Covers the peer that is a battle ahead, which is the
               // case the late-join gap taught us never to drop.
};

// `Slots` is how many finished battles are remembered. An id that falls off
// the end reads as Unknown rather than Retired, so the failure mode of a small
// ring is holding a very stale message instead of dropping it -- bounded, and
// strictly safer than the reverse. Sixteen is far more than the one or two
// battles a message can plausibly be in flight across.
template <uint32_t Slots = 16>
struct BattleTracker {
    uint64_t current = kNoBattle;
    uint64_t retired[Slots] = {};
    uint32_t head = 0;         // next slot to overwrite
    uint32_t count = 0;        // how many slots are populated

    // Enter a battle. The one we were in becomes retired.
    //
    // Re-entering the SAME id is not a transition: the roster snapshot can be
    // re-taken within one battle (a summon changes the character list), and
    // retiring the battle we are still in would make us drop our own peer's
    // messages for it.
    void enter(uint64_t id) {
        if (id == current) return;
        if (current != kNoBattle) push_retired(current);
        // Entering a battle we had retired is not something the game does --
        // a run never returns to a node -- but if it ever happened, treating
        // the id as current has to win over treating it as retired, or every
        // message about it would be dropped. rel() checks `current` first.
        current = id;
    }

    // Leave the current battle without entering another (a battle ends and the
    // peer goes back to the map).
    void leave() {
        if (current == kNoBattle) return;
        push_retired(current);
        current = kNoBattle;
    }

    BattleRel rel(uint64_t id) const {
        if (id != kNoBattle && id == current) return BattleRel::Ours;
        for (uint32_t i = 0; i < count; ++i)
            if (retired[i] == id) return BattleRel::Retired;
        return BattleRel::Unknown;
    }

    bool is_retired(uint64_t id) const { return rel(id) == BattleRel::Retired; }

private:
    void push_retired(uint64_t id) {
        for (uint32_t i = 0; i < count; ++i)
            if (retired[i] == id) return;      // already known; do not duplicate
        retired[head] = id;
        head = (head + 1) % Slots;
        if (count < Slots) ++count;
    }
};

} // namespace mgmp
