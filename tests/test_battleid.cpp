// test_battleid.cpp -- covers battle identity: which battle a message is for.
//
// The thing it replaced was a per-process counter, and the bug it replaced was
// invisible in every unit test because it only appeared when ONE PEER
// RESTARTED and the two counters stopped meaning the same thing. So the cases
// below are written from the peers' point of view, not the struct's: the last
// group is the reconnect that broke the counter, and it passes here for the
// reason the counter could not -- neither side counts anything.
//
//     cl /nologo /EHsc /std:c++17 /I..\src\session test_battleid.cpp && test_battleid.exe
#include "mgmp_battleid.h"
#include <cstdio>

using namespace mgmp;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL  %s\n", what); ++g_fail; }
    else       printf("  ok    %s\n", what);
}

int main() {
    printf("-- a fresh tracker is in no battle --\n");
    {
        BattleTracker<> t;
        check(t.current == kNoBattle, "current is kNoBattle");
        check(t.rel(1234) == BattleRel::Unknown, "an unseen id is Unknown, so it is HELD");
        check(t.rel(kNoBattle) == BattleRel::Unknown,
              "id 0 is not 'ours' even when we are in no battle -- otherwise every "
              "peer sitting on the map would look like it shared one battle");
    }

    printf("-- the three relations --\n");
    {
        BattleTracker<> t;
        t.enter(0xAAAA);
        check(t.rel(0xAAAA) == BattleRel::Ours,     "the battle we are in is Ours");
        check(t.rel(0xBBBB) == BattleRel::Unknown,  "a battle we have not reached is Unknown");
        t.enter(0xBBBB);
        check(t.rel(0xAAAA) == BattleRel::Retired,  "the one we left is Retired -- DROP");
        check(t.rel(0xBBBB) == BattleRel::Ours,     "the new one is Ours");
        check(t.rel(0xCCCC) == BattleRel::Unknown,  "still-unseen stays Unknown -- HOLD");
    }

    printf("-- re-entering the same id is not a transition --\n");
    {
        // The roster snapshot can be re-taken inside one battle (a summon
        // changes the character list). Retiring the battle we are still in
        // would make us drop our own peer's messages for it.
        BattleTracker<> t;
        t.enter(0xAAAA);
        t.enter(0xAAAA);
        check(t.rel(0xAAAA) == BattleRel::Ours, "still Ours after re-entering it");
        check(t.count == 0, "nothing was retired");
    }

    printf("-- leaving a battle for the map --\n");
    {
        BattleTracker<> t;
        t.enter(0xAAAA);
        t.leave();
        check(t.current == kNoBattle, "no current battle");
        check(t.rel(0xAAAA) == BattleRel::Retired, "the battle just left is Retired");
        t.leave();
        check(t.count == 1, "leaving twice does not retire kNoBattle");
    }

    printf("-- the ring forgets the OLDEST, and forgetting is safe --\n");
    {
        BattleTracker<4> t;
        for (uint64_t i = 1; i <= 6; ++i) t.enter(i);   // current = 6, retired = 2,3,4,5
        check(t.rel(6) == BattleRel::Ours, "current is the last entered");
        check(t.rel(5) == BattleRel::Retired, "the most recent retirement is remembered");
        check(t.rel(2) == BattleRel::Retired, "and so is the oldest still in the ring");
        // 1 fell off. It reads Unknown, so it is HELD rather than dropped --
        // the benign direction: a stale message waits instead of a live one
        // being thrown away.
        check(t.rel(1) == BattleRel::Unknown,
              "an id that fell off the ring degrades to HOLD, never to a wrong DROP");
    }

    printf("-- no duplicates in the retired ring --\n");
    {
        BattleTracker<4> t;
        t.enter(0xAAAA); t.leave();
        t.enter(0xAAAA); t.leave();      // same battle twice
        check(t.count == 1, "the same id is retired once, so the ring is not wasted");
    }

    printf("-- THE RECONNECT: the case the counter could not survive --\n");
    {
        // The host has played six battles. The client's process restarts and
        // has played none. With a counter the host sat at 6 and the client at
        // 0, every consume path gated on equality, and the battle stalled in
        // silence. Here both peers enter the same node and therefore hold the
        // same id, and neither has counted anything.
        const uint64_t kNode = 0x211f6ea51e365ae0ULL;   // a real seed0 from a log

        BattleTracker<> host;
        for (uint64_t i = 1; i <= 6; ++i) host.enter(i);
        host.enter(kNode);

        BattleTracker<> client;          // freshly restarted: nothing retired
        client.enter(kNode);

        check(host.rel(kNode) == BattleRel::Ours,   "host: the shared node is Ours");
        check(client.rel(kNode) == BattleRel::Ours, "client: the same node is Ours");
        check(host.rel(kNode) == client.rel(kNode),
              "both peers agree WITHOUT having agreed on a number");

        // And the guarantee that made the counter attractive still holds: a
        // message from a battle the host has genuinely left is still dropped.
        check(host.rel(3) == BattleRel::Retired, "host still drops its own old battles");
        // ...while the client, which never played battle 3, HOLDS instead of
        // dropping. Different answer, and the right one: it has no evidence
        // that battle is over.
        check(client.rel(3) == BattleRel::Unknown,
              "a peer that never played a battle holds rather than drops -- it has "
              "no evidence the battle is finished");
    }

    printf(g_fail ? "\nFAILED (%d)\n" : "\nall passed\n", g_fail);
    return g_fail ? 1 : 0;
}
