#pragma once
// mgmp_hashring -- the bookkeeping behind the per-turn hash exchange, extracted
// as pure code so it can be tested without two live game instances.
//
// Same reason td_decide lives in mgmp_timedelay.h: the surrounding hook can only
// be exercised by a situation that is expensive to stage (there, fighting an
// AstroZombie; here, two peers drifting far enough apart to matter), so the part
// that can be reasoned about on its own is moved somewhere a test can reach it.
// This header owns no global state and does no logging.
//
// WHAT IT IS FOR. Two peers exchange a hash at every turn boundary, but they do
// not arrive at a given turn at the same moment -- run E measured the same
// battle at 23,211 frames and at 12,230, so one peer running several turns ahead
// is ordinary and is not itself a desync. A hash therefore has to be held until
// its counterpart shows up, and it can show up from either direction:
//
//   - the peer is AHEAD of us: its hash arrives for a turn we have not reached.
//     Hold it (peer ring) until our own boundary gets there.
//   - the peer is BEHIND us: its hash arrives for a turn we already passed.
//     Hold our own (my ring) so there is still something to compare against.
//
// The second case is the one that was missing. Comparison used to happen only at
// our own boundary against hashes that had already arrived, so the peer running
// ahead found an empty ring every time and checked nothing. Measured on a clean
// 4-turn loopback run: the client printed four AGREES and the host printed none.
// Correctness survived that -- the trailing peer still catches a mismatch -- but
// only the trailing peer would halt, while the leading one played on.
//
// THE TWO RINGS OVERFLOW DIFFERENTLY, AND ON PURPOSE. They are asymmetric
// because losing the oldest entry and losing the newest entry cost different
// things depending on which ring it is:
//
//   - my own hashes (push_evicting): drop the OLDEST. Only recent turns can
//     still have a counterpart in flight, so the far end is the safe end.
//   - the peer's hashes (push_refusing): drop the NEWEST, and report it. The
//     boundary consumes these in turn order, so the held run has to stay
//     contiguous from where we stand; evicting the oldest would silently
//     remove the very turn we are about to ask for.

#include <cstdint>

namespace mgmp {

// Sized against the failure it guards, not against expected lag. With matching
// in both directions a ring only fills when the peers are genuinely this far
// apart, which for a turn-based lockstep is already pathological -- so filling
// one is a condition to report, not a load to absorb.
constexpr uint32_t kHashRing = 16;

// T must have `uint64_t battle_id` and `uint32_t turn` members, and be
// copy-assignable.
//
// The key is the PAIR, never the turn alone. Turn numbering restarts at 0 in
// every battle, so a hash held across a battle boundary would otherwise match
// the new battle's turn N and manufacture a desync out of a stale message.
template <typename T>
struct HashRing {
    T        slot[kHashRing] = {};
    uint32_t count           = 0;

    void clear() { count = 0; }
    bool full() const { return count == kHashRing; }

    // Append, evicting the oldest when full. Never fails.
    void push_evicting(const T& v) {
        if (count == kHashRing) {
            for (uint32_t i = 0; i + 1 < kHashRing; ++i) slot[i] = slot[i + 1];
            --count;
        }
        slot[count++] = v;
    }

    // Append, refusing when full. Returns false if the value was dropped, so
    // the caller can say so -- the bug this replaces dropped them in silence.
    bool push_refusing(const T& v) {
        if (count == kHashRing) return false;
        slot[count++] = v;
        return true;
    }

    const T* find(uint64_t battle_id, uint32_t turn) const {
        for (uint32_t i = 0; i < count; ++i)
            if (slot[i].battle_id == battle_id && slot[i].turn == turn) return &slot[i];
        return nullptr;
    }

    // find + remove, preserving the order of everything else.
    bool take(uint64_t battle_id, uint32_t turn, T& out) {
        for (uint32_t i = 0; i < count; ++i) {
            if (slot[i].battle_id != battle_id || slot[i].turn != turn) continue;
            out = slot[i];
            for (uint32_t j = i; j + 1 < count; ++j) slot[j] = slot[j + 1];
            --count;
            return true;
        }
        return false;
    }

    // Drops everything from a battle we have already left, keeping entries for
    // the battle we are in AND for battles we have not reached. Keeping the
    // latter is the point: a peer that reached the next battle first has
    // already sent for it, and discarding that is what made a late-arriving
    // peer lose the actions taken without it. Returns how many were dropped, so
    // the caller can report a number instead of losing them silently.
    //
    // `retired` answers "have we left that battle?" -- a BattleTracker, or
    // anything with the same `is_retired(uint64_t)`. It replaces an `epoch <
    // ours` comparison, which was cheaper but only worked while both peers
    // counted from the same zero; see mgmp_battleid.h.
    template <typename Retired>
    uint32_t purge_retired(const Retired& retired) {
        uint32_t kept = 0, dropped = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (retired.is_retired(slot[i].battle_id)) { ++dropped; continue; }
            slot[kept++] = slot[i];
        }
        count = kept;
        return dropped;
    }
};

} // namespace mgmp
