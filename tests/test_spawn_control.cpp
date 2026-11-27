#include "mgmp_spawn_control.h"

#include <cstdio>

using namespace mgmp;

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* description) {
        if (condition) return;
        std::printf("FAIL: %s\n", description);
        ++failures;
    };

    SpawnControl live;
    SpawnControl replay;
    int egg = 0, larva = 0, other = 0;
    int replay_egg = 0, replay_larva = 0, replay_other = 0;
    live.record(&egg, 15, 3);
    live.record(&other, 9, 1);
    replay.record(&replay_other, 22, 1);
    replay.record(&replay_egg, 10, 3);

    auto& child = live.record(&larva, 20, live.owner_slot(&egg, 15));
    auto& replay_child = replay.record(&replay_larva, 19, replay.owner_slot(&replay_egg, 10));
    check(child.owner_slot == 3 && replay_child.owner_slot == 3, "hatching inherits the original owner slot");
    check(live.find(&egg, 15)->id == kNoActor, "AI-only ancestors use no input ID");
    check(live.assign_id(child) == kFirstSpawnActor, "first player input gets the first dynamic ID");
    check(replay.assign_id(replay_child) == child.id, "replay reproduces the ID with different spawn order and addresses");
    check(live.assign_id(*live.find(&other, 9)) == kFirstSpawnActor + 1, "second controllable actor gets a distinct ID");
    check(replay.assign_id(*replay.find(&replay_other, 22)) == kFirstSpawnActor + 1, "second ID survives replay");
    check(live.assign_id(live.record(&larva, 20, 1)) == kFirstSpawnActor && child.owner_slot == 3,
          "a repeated spawn notification preserves ownership and ID");

    live.record(&egg, 16, 0);
    check(live.owner_slot(&egg, 15) == kNoControlSlot, "reused memory cannot impersonate an old source");
    check(child.owner_slot == 3, "a child keeps its owner after its ancestor allocation is reused");
    auto& replacement = live.record(&larva, 21, 0);
    check(live.find(&larva, 20) == nullptr && live.assign_id(replacement) == kFirstSpawnActor + 2,
          "a new allocation gets a new ID even at the same address");

    live.clear();
    check(live.find(&larva, 21) == nullptr && live.assign_id(live.record(&larva, 21, 0)) == kFirstSpawnActor,
          "a new battle clears the registry and restarts its ID sequence");
    return failures != 0;
}
