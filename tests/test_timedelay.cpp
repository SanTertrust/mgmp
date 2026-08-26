// test_timedelay.cpp -- covers the tick conversion's state machine.
//
// The hook it belongs to can only be exercised in-game by fighting an
// AstroZombie, so the branches are tested here instead. Build and run:
//     cl /nologo /EHsc /std:c++17 /I..\src\determinism test_timedelay.cpp && test_timedelay.exe
#include "mgmp_timedelay.h"
#include <cstdio>
#include <cmath>

using namespace mgmp;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL  %s\n", what); ++g_fail; }
    else       printf("  ok    %s\n", what);
}

int main() {
    printf("-- a fresh 3s delay is converted, not fired --\n");
    {
        TdDecision d = td_decide(3.0, 10, 1);
        check(d.action == TD_TAKE_OVER, "TAKE_OVER on first sight");
        check(d.due == 11, "due = turn + wait");
        check(d.value == kTimeDelayManaged + 11.0, "field encodes the due turn");
    }

    printf("\n-- once owned, it waits until the due turn --\n");
    {
        double field = kTimeDelayManaged + 11.0;
        check(td_decide(field, 10, 1).action == TD_WAIT, "turn 10: WAIT");
        check(td_decide(field, 11, 1).action == TD_FIRE, "turn 11: FIRE");
        check(td_decide(field, 99, 1).action == TD_FIRE, "turn 99: FIRE (late)");
        check(td_decide(field, 11, 1).value == -1.0,     "FIRE writes -1.0");
        check(td_decide(field, 11, 1).due   == 11,       "due survives the round trip");
    }

    printf("\n-- the encoding round-trips over a long session --\n");
    {
        bool all = true;
        for (uint32_t t = 0; t < 100000; t += 7) {
            TdDecision d = td_decide(2.5, t, 3);
            if (d.value != kTimeDelayManaged + (double)(t + 3)) { all = false; break; }
            if (td_decide(d.value, t, 3).due != t + 3)          { all = false; break; }
        }
        check(all, "due turn recovers exactly for turns 0..100000");
    }

    printf("\n-- things that must stay vanilla --\n");
    {
        check(td_decide(0.0,  5, 1).action == TD_PASSTHROUGH, "delay 0 (GON key absent)");
        check(td_decide(-1.0, 5, 1).action == TD_PASSTHROUGH, "already-expired countdown");
        double nan = std::nan("");
        check(td_decide(nan, 5, 1).action == TD_PASSTHROUGH,  "NaN does not take over");
    }

    printf("\n-- timedelay_turns = 0 fires immediately, deterministically --\n");
    {
        TdDecision d = td_decide(3.0, 10, 0);
        check(d.action == TD_FIRE, "wait=0 -> FIRE on first sight");
        check(d.value  == -1.0,    "and writes the expiry sentinel");
    }

    printf("\n-- every shipped delay converts (none collide with the sentinel) --\n");
    {
        const double shipped[] = { 0.1, 0.25, 1.13333, 3.0 };
        bool all = true;
        for (double s : shipped)
            if (td_decide(s, 4, 1).action != TD_TAKE_OVER) all = false;
        check(all, ".1 / .25 / 1.13333 / 3 all take over");
    }

    printf("\n%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail != 0;
}
