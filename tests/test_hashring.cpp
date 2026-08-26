// test_hashring -- the per-turn hash exchange's bookkeeping.
//
// The bug this exists to pin: comparison used to happen only at our own turn
// boundary, against hashes the peer had already sent. The peer running AHEAD
// therefore found an empty ring every time and compared nothing. A clean 4-turn
// loopback run measured it -- client four AGREES, host none -- which means a
// desync would have halted the trailing peer while the leading one played on.
//
// Build: via CMake (tests/CMakeLists.txt), or standalone with
//   cl /nologo /EHsc /std:c++17 /W4 /I..\src\net tests\test_hashring.cpp

#include "mgmp_hashring.h"

#include <cstdio>
#include <cstdint>

using namespace mgmp;

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_fail;                                                        \
            std::printf("FAIL %s:%d  ", __FILE__, __LINE__);                 \
            std::printf(__VA_ARGS__);                                        \
            std::printf("\n");                                               \
        }                                                                    \
    } while (0)

// Stand-in for HashMsg: the ring only requires `battle_id` and `turn` members,
// and depending on the real one here would drag the whole protocol header in
// for nothing.
struct Stamp {
    uint64_t battle_id = 0;
    uint32_t turn = 0;
    uint64_t payload = 0;
};

// Stand-in for BattleTracker: purge_retired only asks "have we left that
// battle?", so the test supplies the answer directly rather than driving a
// tracker. That keeps this file testing the RING and not battle identity,
// which has its own test.
struct Retired {
    uint64_t ids[8] = {};
    uint32_t n = 0;
    void add(uint64_t id) { if (n < 8) ids[n++] = id; }
    bool is_retired(uint64_t id) const {
        for (uint32_t i = 0; i < n; ++i) if (ids[i] == id) return true;
        return false;
    }
};

int main() {
    // --- find/take on an empty ring ------------------------------------
    {
        HashRing<Stamp> r;
        Stamp out{};
        CHECK(r.count == 0, "fresh ring is not empty");
        CHECK(!r.full(), "fresh ring reports full");
        CHECK(r.find(1, 0) == nullptr, "found a turn in an empty ring");
        CHECK(!r.take(1, 0, out), "took a turn from an empty ring");
    }

    // --- THE REGRESSION: a hash for a turn we already passed ------------
    //
    // The leading peer's path. Our boundary records turns 0..3; the trailing
    // peer's hash for turn 1 arrives afterwards and must still find a partner.
    {
        HashRing<Stamp> mine;
        for (uint32_t t = 0; t < 4; ++t) mine.push_evicting(Stamp{1, t, 0xA000 + t});

        const Stamp* hit = mine.find(1, 1);
        CHECK(hit != nullptr, "a hash for a turn we already passed found no partner");
        CHECK(hit && hit->payload == 0xA001, "matched the wrong turn");

        // find() does not consume: both peers may report on the same turn.
        CHECK(mine.count == 4, "find() consumed an entry");
        CHECK(mine.find(1, 9) == nullptr, "matched a turn we never reached");
    }

    // --- the trailing peer's path: hold until our boundary arrives -------
    {
        HashRing<Stamp> peer;
        CHECK(peer.push_refusing(Stamp{1, 5, 0xB005}), "refused into an empty ring");
        CHECK(peer.push_refusing(Stamp{1, 6, 0xB006}), "refused with room left");

        Stamp out{};
        CHECK(!peer.take(1, 4, out), "took a turn that was never held");
        CHECK(peer.take(1, 5, out), "could not take a held turn");
        CHECK(out.payload == 0xB005, "took the wrong turn");
        CHECK(peer.count == 1, "take() did not consume");
        CHECK(!peer.take(1, 5, out), "took the same turn twice");
    }

    // --- take() preserves order, since the boundary consumes in turn order
    {
        HashRing<Stamp> r;
        for (uint32_t t = 0; t < 5; ++t) r.push_refusing(Stamp{1, t, 0xC000 + t});
        Stamp out{};
        CHECK(r.take(1, 2, out), "could not take from the middle");
        CHECK(r.count == 4, "wrong count after a middle take");
        // 0,1,3,4 in that order
        const uint32_t want[4] = {0, 1, 3, 4};
        for (uint32_t i = 0; i < 4; ++i)
            CHECK(r.slot[i].turn == want[i], "order broken at %u: %u != %u",
                  i, r.slot[i].turn, want[i]);
    }

    // --- overflow, my ring: drop the OLDEST, never fail ------------------
    //
    // Only recent turns can still have a counterpart in flight, so the far end
    // is the safe end to lose.
    {
        HashRing<Stamp> mine;
        for (uint32_t t = 0; t < kHashRing + 4; ++t) mine.push_evicting(Stamp{1, t, t});
        CHECK(mine.count == kHashRing, "evicting ring grew past its bound");
        CHECK(mine.find(1, 0) == nullptr, "oldest survived eviction");
        CHECK(mine.find(1, 3) == nullptr, "an evicted turn was still found");
        CHECK(mine.find(1, 4) != nullptr, "evicted more than it had to");
        CHECK(mine.find(1, kHashRing + 3) != nullptr, "newest was not kept");
        // Contiguous and in order after the shifting.
        for (uint32_t i = 0; i < kHashRing; ++i)
            CHECK(mine.slot[i].turn == i + 4, "eviction broke order at %u", i);
    }

    // --- overflow, peer ring: refuse the NEWEST, and SAY so --------------
    //
    // The boundary consumes these in turn order, so the held run must stay
    // contiguous from where we stand. Evicting the oldest here would silently
    // remove the very turn we are about to ask for -- which is the shape of the
    // original bug, and is why the two rings overflow differently.
    {
        HashRing<Stamp> peer;
        for (uint32_t t = 0; t < kHashRing; ++t)
            CHECK(peer.push_refusing(Stamp{1, t, t}), "refused early at %u", t);
        CHECK(peer.full(), "ring did not report full");
        CHECK(!peer.push_refusing(Stamp{1, kHashRing, 0}), "accepted past the bound");
        CHECK(peer.count == kHashRing, "refusal changed the count");
        CHECK(peer.find(1, 0) != nullptr, "refusal dropped the oldest instead");

        // Still drains from the front, which is the whole point of refusing.
        Stamp out{};
        CHECK(peer.take(1, 0, out) && out.turn == 0, "cannot drain after a refusal");
        CHECK(peer.push_refusing(Stamp{1, kHashRing, 99}), "no room after draining one");
    }

    // --- clear() is what a new battle does -------------------------------
    {
        HashRing<Stamp> r;
        for (uint32_t t = 0; t < 5; ++t) r.push_evicting(Stamp{1, t, t});
        r.clear();
        CHECK(r.count == 0, "clear() left entries");
        CHECK(r.find(1, 0) == nullptr, "a stale entry survived clear()");
        // Turn numbering restarts per battle, so a stale turn 0 matching a new
        // turn 0 would be a manufactured desync. clear() is what prevents it.
        r.push_evicting(Stamp{1, 0, 0xD00D});
        const Stamp* hit = r.find(1, 0);
        CHECK(hit && hit->payload == 0xD00D, "post-clear entry did not take");
    }

    // --- THE BATTLE-EPOCH RACE ------------------------------------------
    //
    // Turn numbering restarts at 0 in every battle. A hash still in flight when
    // both peers cross a battle boundary arrives looking exactly like one about
    // the battle they just entered -- and comparing it would manufacture a
    // desync out of a stale message. The key is the PAIR, never the turn alone.
    {
        HashRing<Stamp> r;
        r.push_evicting(Stamp{1, 0, 0xE001});   // battle 1, turn 0
        r.push_evicting(Stamp{2, 0, 0xE002});   // battle 2, turn 0

        const Stamp* a = r.find(1, 0);
        const Stamp* b = r.find(2, 0);
        CHECK(a && a->payload == 0xE001, "turn 0 of battle 1 did not match itself");
        CHECK(b && b->payload == 0xE002, "turn 0 of battle 2 did not match itself");
        CHECK(a != b, "two battles' turn 0 collapsed into one entry");
        CHECK(r.find(3, 0) == nullptr, "matched a battle that never happened");

        Stamp out{};
        CHECK(r.take(2, 0, out) && out.payload == 0xE002, "took the wrong battle");
        CHECK(r.find(1, 0) != nullptr, "taking battle 2 removed battle 1");
    }

    // --- purge_before: drop the past, KEEP the future --------------------
    //
    // The asymmetry is the point. Entries from a battle we have left are stale;
    // entries for a battle we have not reached yet belong to a peer that got
    // there first, and discarding those is what made a peer joining late lose
    // the actions taken without it.
    {
        HashRing<Stamp> r;
        r.push_refusing(Stamp{1, 7, 0xF001});   // past
        r.push_refusing(Stamp{2, 0, 0xF002});   // current
        r.push_refusing(Stamp{2, 1, 0xF003});   // current
        r.push_refusing(Stamp{3, 0, 0xF004});   // future -- peer is ahead

        Retired left;
        left.add(1);                            // we have finished battle 1
        uint32_t dropped = r.purge_retired(left);
        CHECK(dropped == 1, "purge dropped %u, expected 1", dropped);
        CHECK(r.count == 3, "purge left %u entries, expected 3", r.count);
        CHECK(r.find(1, 7) == nullptr, "a stale entry survived the purge");
        CHECK(r.find(2, 0) != nullptr, "purge dropped the current battle");
        CHECK(r.find(2, 1) != nullptr, "purge dropped the current battle");
        CHECK(r.find(3, 0) != nullptr, "purge dropped a FUTURE entry -- this is "
                                       "the late-join drop, reintroduced");

        // Order survives, since the boundary consumes in turn order.
        CHECK(r.slot[0].turn == 0 && r.slot[0].battle_id == 2, "purge broke order at 0");
        CHECK(r.slot[1].turn == 1 && r.slot[1].battle_id == 2, "purge broke order at 1");
        CHECK(r.slot[2].battle_id == 3, "purge broke order at 2");

        // Idempotent, and retiring everything empties it.
        CHECK(r.purge_retired(left) == 0, "second purge dropped something");
        left.add(2); left.add(3);
        CHECK(r.purge_retired(left) == 3, "retiring every battle did not empty it");
        CHECK(r.count == 0, "ring not empty after purging everything");
    }

    // --- an UNKNOWN battle is kept, never dropped -------------------------
    {
        // The half that matters for a reconnect: a peer that restarted has
        // retired nothing, so every battle_id it has not played reads Unknown.
        // If purge treated Unknown as stale it would throw away exactly the
        // messages a joining peer needs.
        HashRing<Stamp> r;
        r.push_refusing(Stamp{0xAAAA, 0, 1});
        r.push_refusing(Stamp{0xBBBB, 0, 2});
        Retired nothing;                        // a freshly restarted peer
        CHECK(r.purge_retired(nothing) == 0, "a peer with no history dropped entries");
        CHECK(r.count == 2, "a peer with no history lost entries");
    }

    // --- purge on an empty ring is a no-op --------------------------------
    {
        HashRing<Stamp> r;
        Retired left; left.add(5);
        CHECK(r.purge_retired(left) == 0, "purge of an empty ring dropped something");
        CHECK(r.count == 0, "purge of an empty ring changed the count");
    }

    std::printf("%s -- %d checks, %d failure(s)\n",
                g_fail ? "FAILED" : "PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
