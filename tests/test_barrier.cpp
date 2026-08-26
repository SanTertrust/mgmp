// test_barrier.cpp -- covers the join barrier's decision table.
//
// The hook it belongs to can only be exercised in-game by getting two peers to
// drift apart -- one inside a battle, one still loading -- which is how the
// late-join gap was found in the first place (by accident) and is no way to
// check the edges. Build and run:
//     cl /nologo /EHsc /std:c++17 /I..\src\session test_barrier.cpp && test_barrier.exe
#include "mgmp_barrier.h"
#include <cstdio>
#include <cstring>

using namespace mgmp;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL  %s\n", what); ++g_fail; }
    else       printf("  ok    %s\n", what);
}

// The state a peer is in once both sides are in the same battle and agree.
static BarrierFacts ready() {
    BarrierFacts f;
    f.enabled = true; f.control_checked = true; f.snapped = true;
    f.have_peer_ctrl = true; f.peer_battle = 3; f.battle = 3;
    return f;
}

int main() {
    printf("-- open only when both peers are in this battle --\n");
    {
        check(barrier_decide(ready()) == nullptr, "agreed split opens the barrier");

        BarrierFacts f = ready();
        f.control_checked = false;
        check(barrier_decide(f) != nullptr, "unchecked split holds even when everything else is ready");
    }

    printf("\n-- each way of not being ready names itself --\n");
    {
        BarrierFacts f = ready();
        f.control_checked = false; f.snapped = false;
        check(strstr(barrier_decide(f), "roster") != nullptr, "no roster is reported as such");

        f = ready();
        f.control_checked = false; f.have_peer_ctrl = false;
        check(strstr(barrier_decide(f), "control split") != nullptr, "missing CONTROL is reported as such");

        f = ready();
        // Ahead: a battle we have not reached, so nothing retired it.
        f.control_checked = false; f.peer_battle = 4; f.peer_battle_retired = false;
        check(strstr(barrier_decide(f), "have not reached") != nullptr,
              "peer ahead is reported as ahead");

        // Behind: a battle we finished. Battle ids do not order, so the CALLER
        // supplies this from its retired set -- that is the whole difference
        // between the two branches now.
        f = ready();
        f.control_checked = false; f.peer_battle = 2; f.peer_battle_retired = true;
        check(strstr(barrier_decide(f), "have left") != nullptr,
              "peer behind is reported as behind");
    }

    printf("\n-- disabled is a no-op, not a different code path --\n");
    {
        // Every blocked case above must open once the ini turns it off. If any
        // one of them still held, `net_join_barrier = 0` would not actually
        // answer "is the barrier what is stuck?", which is the only reason the
        // toggle exists.
        bool all = true;
        for (int ctl = 0; ctl < 2; ++ctl)
        for (int snap = 0; snap < 2; ++snap)
        for (int have = 0; have < 2; ++have)
        for (uint32_t pe = 0; pe < 3; ++pe) {
            BarrierFacts f;
            f.enabled = false;
            f.control_checked = (ctl != 0); f.snapped = (snap != 0);
            f.have_peer_ctrl = (have != 0); f.peer_battle = pe; f.battle = 1;
            if (barrier_decide(f) != nullptr) all = false;
        }
        check(all, "disabled opens in all 24 state combinations");
    }

    printf("\n-- the barrier re-arms per battle, not per session --\n");
    {
        // A peer that agreed on battle 3 and then moved to battle 4 must hold
        // again until the peer's CONTROL for 4 arrives. This is the case that
        // makes the barrier cover a whole run rather than only its first fight:
        // a peer can fall behind at any map node, and the second battle is no
        // safer than the first.
        BarrierFacts f = ready();
        f.battle = 4; f.control_checked = false;  // re-snapshotted; peer_battle still 3
        f.peer_battle_retired = true;             // ...and 3 is one we finished
        check(barrier_decide(f) != nullptr, "new battle holds again on the old split");
        f.peer_battle = 4; f.peer_battle_retired = false; f.control_checked = true;
        check(barrier_decide(f) == nullptr, "and opens when the peer catches up");
    }

    printf("\n-- same battle but unchecked still holds --\n");
    {
        // verify_control may simply not have run yet this frame. Holding is the
        // safe reading: the cost is a few more polls, and the alternative is
        // deciding a turn on the strength of a check that has not happened.
        BarrierFacts f = ready();
        f.control_checked = false;
        check(barrier_decide(f) != nullptr, "same battle is not by itself agreement");
    }

    printf("\n%s -- %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
