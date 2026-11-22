// test_draft.cpp -- unit tests for DraftModel & draft wire protocol.

#include "mgmp_draft.h"
#include "mgmp_proto.h"

#include <cstdio>
#include <cstdlib>

using namespace mgmp;

static int g_fails = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_fails; \
    } \
} while (0)

static void test_quotas_2_3_4_players() {
    const uint64_t cats4[4] = { 101, 102, 103, 104 };

    // 2 players, 4 cats -> 2 / 2
    {
        DraftModel d;
        const uint8_t peers[2] = { 0, 1 };
        CHECK(d.start(1, cats4, 4, peers, 2));
        CHECK(d.quota_of(0) == 2);
        CHECK(d.quota_of(1) == 2);
        CHECK(d.total_quota() == 4);
    }

    // 3 players, 4 cats -> host 2, others 1 each (2 / 1 / 1)
    {
        DraftModel d;
        const uint8_t peers[3] = { 0, 1, 2 };
        CHECK(d.start(2, cats4, 4, peers, 3));
        CHECK(d.quota_of(0) == 2);
        CHECK(d.quota_of(1) == 1);
        CHECK(d.quota_of(2) == 1);
        CHECK(d.total_quota() == 4);
    }

    // 4 players, 4 cats -> 1 / 1 / 1 / 1
    {
        DraftModel d;
        const uint8_t peers[4] = { 0, 1, 2, 3 };
        CHECK(d.start(3, cats4, 4, peers, 4));
        CHECK(d.quota_of(0) == 1);
        CHECK(d.quota_of(1) == 1);
        CHECK(d.quota_of(2) == 1);
        CHECK(d.quota_of(3) == 1);
        CHECK(d.total_quota() == 4);
    }

    // 3 cats, 2 players -> 2 / 1
    {
        DraftModel d;
        const uint8_t peers[2] = { 0, 1 };
        CHECK(d.start(4, cats4, 3, peers, 2));
        CHECK(d.quota_of(0) == 2);
        CHECK(d.quota_of(1) == 1);
        CHECK(d.total_quota() == 3);
    }
}

static void test_first_come_unpick_and_autocomplete() {
    DraftModel d;
    const uint64_t cats[4] = { 10, 20, 30, 40 };
    const uint8_t peers[3] = { 0, 1, 2 }; // quotas: 2 / 1 / 1
    CHECK(d.start(7, cats, 4, peers, 3));

    // Peer 1 claims cat 20 first.
    CHECK(d.toggle_pick(1, 20));
    CHECK(d.active);
    CHECK(!d.decided);

    // Host tries to steal cat 20 -> rejected!
    CHECK(!d.toggle_pick(0, 20));

    // Peer 1 tries to claim a second cat (quota is 1) -> rejected!
    CHECK(!d.toggle_pick(1, 30));

    // Peer 1 unpicks cat 20 while draft is still active -> succeeds!
    CHECK(d.toggle_pick(1, 20));
    CHECK(d.picked_of(1) == 0);

    // Now host claims 20 and 10 (quota 2).
    CHECK(d.toggle_pick(0, 20));
    CHECK(d.toggle_pick(0, 10));
    CHECK(!d.toggle_pick(0, 30)); // host quota full
    CHECK(d.active);

    // Peer 1 claims 30.
    CHECK(d.toggle_pick(1, 30));
    CHECK(d.active);

    // Peer 2 claims 40 -> draft auto-completes!
    CHECK(d.toggle_pick(2, 40));
    CHECK(!d.active);
    CHECK(d.decided);
    CHECK(d.is_complete());

    // Cannot unpick after completion.
    CHECK(!d.toggle_pick(2, 40));

    uint8_t own = kNoPeer;
    CHECK(d.lookup_owner(10, &own) && own == 0);
    CHECK(d.lookup_owner(20, &own) && own == 0);
    CHECK(d.lookup_owner(30, &own) && own == 1);
    CHECK(d.lookup_owner(40, &own) && own == 2);
    CHECK(!d.lookup_owner(999, &own));
}

static void test_peer_disconnect_mid_draft() {
    DraftModel d;
    const uint64_t cats[4] = { 1, 2, 3, 4 };
    const uint8_t peers[3] = { 0, 1, 2 }; // quotas: 2 / 1 / 1
    CHECK(d.start(1, cats, 4, peers, 3));

    CHECK(d.toggle_pick(2, 4));
    // Peer 2 disconnects -> cat 4 freed, quotas become 2 / 2 for peers {0, 1}.
    CHECK(d.on_peer_left(2));
    CHECK(d.active);
    CHECK(d.peer_count == 2);
    CHECK(d.quota_of(0) == 2);
    CHECK(d.quota_of(1) == 2);
    CHECK(d.owner[3] == kNoPeer);

    // Peer 1 also disconnects -> <2 peers remain, draft cancels cleanly.
    CHECK(d.on_peer_left(1));
    CHECK(!d.active);
    CHECK(!d.decided);
}

static void test_wire_roundtrip() {
    DraftPickMsg p_in{};
    p_in.generation = 42;
    p_in.cat_uid    = 0x1122334455667788ULL;

    uint8_t buf[256] = {};
    uint32_t n = enc_draft_pick(buf, sizeof(buf), p_in);
    CHECK(n > 0);
    CHECK(buf[0] == MSG_DRAFT_PICK);

    Reader r(buf + 1, n - 1);
    DraftPickMsg p_out{};
    CHECK(dec_draft_pick(r, p_out));
    CHECK(p_out.generation == p_in.generation);
    CHECK(p_out.cat_uid == p_in.cat_uid);

    DraftModel d;
    const uint64_t cats[4] = { 10, 20, 30, 40 };
    const uint8_t peers[3] = { 0, 1, 2 };
    d.start(9, cats, 4, peers, 3);
    d.toggle_pick(0, 10);
    d.toggle_pick(2, 40);

    DraftStateMsg s_in = d.to_msg();
    uint32_t sn = enc_draft_state(buf, sizeof(buf), s_in);
    CHECK(sn > 0);
    CHECK(buf[0] == MSG_DRAFT_STATE);

    Reader sr(buf + 1, sn - 1);
    DraftStateMsg s_out{};
    CHECK(dec_draft_state(sr, s_out));
    CHECK(s_out.generation == 9);
    CHECK(s_out.active == 1);
    CHECK(s_out.count == 4);
    CHECK(s_out.peer_count == 3);
    CHECK(s_out.cat_uid[0] == 10 && s_out.owner[0] == 0);
    CHECK(s_out.cat_uid[1] == 20 && s_out.owner[1] == kNoPeer);
    CHECK(s_out.cat_uid[3] == 40 && s_out.owner[3] == 2);
    CHECK(s_out.peer_quota[0] == 2 && s_out.peer_quota[1] == 1 && s_out.peer_quota[2] == 1);

    DraftModel d2;
    d2.apply_msg(s_out);
    CHECK(d2.active);
    CHECK(d2.picked_of(0) == 1);
    CHECK(d2.picked_of(2) == 1);
}

int main() {
    test_quotas_2_3_4_players();
    test_first_come_unpick_and_autocomplete();
    test_peer_disconnect_mid_draft();
    test_wire_roundtrip();
    if (g_fails == 0) printf("test_draft: ALL PASSED\n");
    return g_fails ? 1 : 0;
}
