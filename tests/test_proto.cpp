// test_proto.cpp -- covers the wire codec.
//
// The transport it belongs to can only be exercised with two game instances, so
// the encoding is tested here instead. Same reasoning as test_timedelay.cpp:
// the parts that are pure logic get tested where they are cheap to test, and
// the in-game run is then only asked to prove the parts that need a game.
//
//     cl /nologo /EHsc /std:c++17 /I..\src\net test_proto.cpp && test_proto.exe
#include "mgmp_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using namespace mgmp;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL  %s\n", what); ++g_fail; }
    else       printf("  ok    %s\n", what);
}

int main() {
    uint8_t buf[512];

    printf("-- ActionMsg round-trips every field --\n");
    {
        ActionMsg a{};
        a.turn = 7; a.actor = 3; a.type = 2;
        a.battle_id = 0x211f6ea51e365ae0ULL;   // a real node seed0, from a log
        a.slot_kind = 4; a.slot_index = 1;
        a.b30 = 1; a.b31 = 0;
        a.tx = 5; a.ty = -3; a.dx = -1; a.dy = 0;
        strcpy_s(a.gon, "wp_BearTraps");

        uint32_t n = enc_action(buf, sizeof(buf), a);
        check(n > 0, "encodes");

        Reader r(buf, n);
        check(r.u8v() == MSG_ACTION, "message type is ACTION");
        ActionMsg b{};
        check(dec_action(r, b), "decodes");
        check(b.battle_id == 0x211f6ea51e365ae0ULL,
              "battle_id, which keys every per-battle message -- all 64 bits of it, "
              "since it is a node seed and not a small counter");
        check(b.turn == 7 && b.actor == 3 && b.type == 2, "turn/actor/type");
        check(b.slot_kind == 4 && b.slot_index == 1, "slot identity");
        check(b.b30 == 1 && b.b31 == 0, "the two tail bytes");
        check(b.tx == 5 && b.ty == -3, "target survives a negative");
        check(b.dx == -1 && b.dy == 0, "direction survives a negative");
        check(strcmp(b.gon, "wp_BearTraps") == 0, "GON name, the second identity");
    }

    printf("\n-- types 6 and 7 are refused at the edge --\n");
    {
        // They are generated locally on both peers. One arriving over the wire
        // means the sender is broken, and applying it would double-fire a
        // reaction on this peer -- so the decoder rejects rather than trusts.
        for (uint8_t bad : { (uint8_t)1, (uint8_t)6, (uint8_t)7, (uint8_t)99 }) {
            ActionMsg a{}; a.type = bad; a.turn = 1;
            uint32_t n = enc_action(buf, sizeof(buf), a);
            Reader r(buf, n); r.u8v();
            ActionMsg b{};
            char what[64];
            sprintf_s(what, "type %u rejected", (unsigned)bad);
            check(!dec_action(r, b), what);
        }
        for (uint8_t good : { (uint8_t)2, (uint8_t)3 }) {
            ActionMsg a{}; a.type = good; a.turn = 1;
            uint32_t n = enc_action(buf, sizeof(buf), a);
            Reader r(buf, n); r.u8v();
            ActionMsg b{};
            char what[64];
            sprintf_s(what, "type %u accepted", (unsigned)good);
            check(dec_action(r, b), what);
        }
    }

    printf("\n-- Hello and the data-identity fields --\n");
    {
        Hello h{};
        h.gpak_hash = 0xDEADBEEFCAFEF00DULL;
        h.build_hash = 0x0123456789ABCDEFULL;
        strcpy_s(h.name, "host");
        uint32_t n = enc_hello(buf, sizeof(buf), h);
        Reader r(buf, n); r.u8v();
        Hello o{};
        check(dec_hello(r, o), "decodes");
        check(o.proto == kProtoVersion, "protocol version");
        check(o.gpak_hash == h.gpak_hash, "gpak hash survives the high bit");
        check(o.build_hash == h.build_hash, "build hash");
        check(strcmp(o.name, "host") == 0, "name");
    }

    printf("\n-- Welcome carries all four xoshiro words --\n");
    {
        Welcome w{};
        w.rng_state[0] = 0x967e2d6d328620b1ULL;   // the state four launches
        w.rng_state[1] = 0x1111111111111111ULL;   // all entered a battle at
        w.rng_state[2] = 0x2222222222222222ULL;
        w.rng_state[3] = 0xFFFFFFFFFFFFFFFFULL;
        w.cat_count = 2; w.cats[0] = 0; w.cats[1] = 2;
        uint32_t n = enc_welcome(buf, sizeof(buf), w);
        Reader r(buf, n); r.u8v();
        Welcome o{};
        check(dec_welcome(r, o), "decodes");
        check(o.rng_state[0] == w.rng_state[0] && o.rng_state[3] == w.rng_state[3],
              "the simulation stream round-trips");
        check(o.cat_count == 2 && o.cats[0] == 0 && o.cats[1] == 2, "control list");
    }

    printf("\n-- HashMsg --\n");
    {
        HashMsg h{}; h.battle_id = 6; h.turn = 12; h.rng_hash = 0xABCDEF; h.state_hash = 0;
        h.queue_depth = 3; h.queue_sig = 0x99;
        uint32_t n = enc_hash(buf, sizeof(buf), h);
        Reader r(buf, n); r.u8v();
        HashMsg o{};
        check(dec_hash(r, o), "decodes");
        check(o.battle_id == 6, "battle id");
        check(o.turn == 12 && o.rng_hash == 0xABCDEF, "turn and rng hash");
        check(o.queue_depth == 3 && o.queue_sig == 0x99, "queue depth and signature");
    }

    printf("\n-- HashMsg carries the state hash --\n");
    {
        // state_hash was 0 on the wire for the whole of phase 4, so it is the
        // field most likely to be dropped by a codec change and least likely
        // to be noticed: two peers that both send 0 agree forever.
        HashMsg h{}; h.turn = 4; h.rng_hash = 1; h.state_hash = 0x8877665544332211ULL;
        uint32_t n = enc_hash(buf, sizeof(buf), h);
        Reader r(buf, n); r.u8v();
        HashMsg o{};
        check(dec_hash(r, o), "decodes");
        check(o.state_hash == 0x8877665544332211ULL, "state hash survives the high bit");
    }

    printf("\n-- ControlMsg round-trips the split --\n");
    {
        ControlMsg c{};
        c.battle_id = 3;
        c.humans = 4; c.count = 2; c.cats[0] = 19; c.cats[1] = 20;
        uint32_t n = enc_control(buf, sizeof(buf), c);
        check(n > 0, "encodes");
        Reader r(buf, n);
        check(r.u8v() == MSG_CONTROL, "message type is CONTROL");
        ControlMsg o{};
        check(dec_control(r, o), "decodes");
        check(o.battle_id == 3, "battle id");
        check(o.humans == 4, "human count");
        check(o.count == 2 && o.cats[0] == 19 && o.cats[1] == 20, "claimed indices");
        check(r.pos == r.len, "consumed exactly the frame");
    }
    {
        // An empty claim is legal: a pure observer claims nothing, and the
        // peer must be able to tell that apart from a malformed frame.
        ControlMsg c{}; c.humans = 2; c.count = 0;
        uint32_t n = enc_control(buf, sizeof(buf), c);
        Reader r(buf, n); r.u8v();
        ControlMsg o{}; o.count = 7;
        check(dec_control(r, o) && o.count == 0, "an empty claim decodes as empty");
    }
    {
        ControlMsg c{}; c.humans = 1; c.count = 1; c.cats[0] = 5;
        uint32_t n = enc_control(buf, sizeof(buf), c);
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader r(buf, cut); r.u8v();
            ControlMsg o{};
            dec_control(r, o);          // must not crash
        }
        check(true, "truncated CONTROL frames parse without faulting");
        Reader r(buf, n - 1); r.u8v();
        ControlMsg o{};
        check(!dec_control(r, o), "one byte short is rejected");
    }

    printf("\n-- AimMsg carries a SLOT, never an ability pointer --\n");
    {
        AimMsg a{};
        a.battle_id = 0xABCDEF0123456789ull;
        a.cat = 13; a.active = 1; a.slot_kind = 4; a.slot_index = 2;
        a.tx = 6; a.ty = -3; a.dx = -1; a.dy = 0;
        strcpy(a.gon, "FireballSpell");
        uint32_t n = enc_aim(buf, sizeof(buf), a);
        check(n > 0, "encodes");
        Reader r(buf, n);
        check(r.u8v() == MSG_AIM, "message type is AIM");
        AimMsg o{};
        check(dec_aim(r, o), "decodes");
        check(o.battle_id == a.battle_id, "battle id survives all 64 bits");
        check(o.cat == 13 && o.active == 1, "cat and active");
        check(o.slot_kind == 4 && o.slot_index == 2, "the slot, which is the identity");
        check(o.tx == 6 && o.ty == -3, "target, including a negative coordinate");
        check(o.dx == -1 && o.dy == 0, "direction");
        check(strcmp(o.gon, "FireballSpell") == 0, "the GON name, the second identity");
        check(r.pos == r.len, "consumed exactly the frame");
    }
    {
        // The tile reaches a game function that indexes the tactics grid, the
        // same hazard CURSOR's does, so it is refused at the decoder.
        AimMsg a{}; a.active = 1; a.tx = 999999;
        uint32_t n = enc_aim(buf, sizeof(buf), a);
        Reader r(buf, n); r.u8v();
        AimMsg o{};
        check(!dec_aim(r, o), "an out-of-range target is rejected");
    }
    {
        // A direction is one step, never a vector across the board. Anything
        // else means the fields moved.
        AimMsg a{}; a.active = 1; a.dx = 50;
        uint32_t n = enc_aim(buf, sizeof(buf), a);
        Reader r(buf, n); r.u8v();
        AimMsg o{};
        check(!dec_aim(r, o), "an implausible direction is rejected");
    }
    {
        AimMsg a{}; a.cat = 1; a.active = 1; a.tx = 2; a.ty = 3;
        strcpy(a.gon, "DefaultMove");
        uint32_t n = enc_aim(buf, sizeof(buf), a);
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader r(buf, cut); r.u8v();
            AimMsg o{};
            dec_aim(r, o);              // must not fault at any truncation
        }
        check(true, "truncated AIM frames parse without faulting");
    }

    printf("\n-- CursorMsg is a tile, not a pixel --\n");
    {
        CursorMsg c{};
        c.battle_id = 7; c.x = 11; c.y = 4; c.on_board = 1; c.owns_turn = 1;
        uint32_t n = enc_cursor(buf, sizeof(buf), c);
        check(n > 0, "encodes");
        Reader r(buf, n);
        check(r.u8v() == MSG_CURSOR, "message type is CURSOR");
        CursorMsg o{};
        check(dec_cursor(r, o), "decodes");
        check(o.battle_id == 7, "battle id");
        check(o.x == 11 && o.y == 4, "tile coordinates");
        check(o.on_board == 1 && o.owns_turn == 1, "both flags");
        check(r.pos == r.len, "consumed exactly the frame");
    }
    {
        // Off the board is the common case -- reading a tooltip, moving to the
        // end-turn button -- and must survive whatever stale tile is still in
        // the coordinate fields, including a negative one.
        CursorMsg c{}; c.x = -1; c.y = -1; c.on_board = 0;
        uint32_t n = enc_cursor(buf, sizeof(buf), c);
        Reader r(buf, n); r.u8v();
        CursorMsg o{}; o.on_board = 1;
        check(dec_cursor(r, o), "an off-board cursor decodes");
        check(o.on_board == 0, "and stays off-board");
    }
    {
        // The tile reaches an array subscript inside the game, so a nonsense
        // one is refused at the decoder rather than at the draw call.
        CursorMsg c{}; c.x = 999999; c.y = 0; c.on_board = 1;
        uint32_t n = enc_cursor(buf, sizeof(buf), c);
        Reader r(buf, n); r.u8v();
        CursorMsg o{};
        check(!dec_cursor(r, o), "an out-of-range tile is rejected");
    }
    {
        CursorMsg c{}; c.battle_id = 1; c.x = 2; c.y = 3; c.on_board = 1;
        uint32_t n = enc_cursor(buf, sizeof(buf), c);
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader r(buf, cut); r.u8v();
            CursorMsg o{};
            dec_cursor(r, o);           // must not crash
        }
        check(true, "truncated CURSOR frames parse without faulting");
        Reader r(buf, n - 1); r.u8v();
        CursorMsg o{};
        check(!dec_cursor(r, o), "one byte short is rejected");
    }

    printf("\n-- EnterNodeMsg carries the node seed --\n");
    {
        // seed0 is the field that matters: EnterNode copies MapNode+0x118
        // into TLS+0x178, so a mismatch here IS the battle starting from a
        // different RNG state. It must survive the high bit intact.
        EnterNodeMsg m{};
        m.index = 12; m.node_count = 40; m.type = 5;
        m.seed0 = 0xF7E6D5C4B3A29180ULL;
        uint32_t n = enc_enter_node(buf, sizeof(buf), m);
        check(n > 0, "encodes");
        Reader r(buf, n);
        check(r.u8v() == MSG_ENTERNODE, "message type is ENTERNODE");
        EnterNodeMsg o{};
        check(dec_enter_node(r, o), "decodes");
        check(o.index == 12 && o.node_count == 40, "index and map size");
        check(o.type == 5, "node type (5 = battle)");
        check(o.seed0 == 0xF7E6D5C4B3A29180ULL, "seed survives the high bit");
        check(r.pos == r.len, "consumed exactly the frame");
    }
    {
        EnterNodeMsg m{}; m.index = 1; m.seed0 = 1;
        uint32_t n = enc_enter_node(buf, sizeof(buf), m);
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader r(buf, cut); r.u8v();
            EnterNodeMsg o{};
            dec_enter_node(r, o);
        }
        check(true, "truncated ENTERNODE frames parse without faulting");
        Reader r(buf, n - 1); r.u8v();
        EnterNodeMsg o{};
        check(!dec_enter_node(r, o), "one byte short is rejected");
    }

    printf("\n-- ChoiceMsg: the message that replaces RUNSTATE --\n");
    {
        // The whole meta layer rests on this being right: both peers compute
        // the same event effects from the same node seed, so the ONLY thing
        // that crosses the wire is which option a person picked. Get the index
        // wrong and the client applies a different outcome silently.
        ChoiceMsg m{};
        m.kind = kChoiceLevelUp; m.index = 2; m.count = 4; m.aux = 7;
        m.node_seed = 0x99AF76329BA60F33ull;
        strcpy_s(m.name, "FurySwipes");
        uint32_t n = enc_choice(buf, sizeof(buf), m);
        check(n > 0, "encodes");
        Reader r(buf, n);
        check(r.u8v() == MSG_CHOICE, "message type is CHOICE");
        ChoiceMsg o{};
        check(dec_choice(r, o), "decodes");
        check(o.kind == kChoiceLevelUp, "kind");
        check(o.index == 2 && o.count == 4, "index and option count");
        check(o.aux == 7, "the level-up option type rides in aux");
        check(strcmp(o.name, "FurySwipes") == 0, "the name cross-check survives");
        // Proto 19. Without this the choice has no idea which node it is about,
        // and a held one surfaces on a later node's screen -- measured live on
        // 2026-08-25, where node 6's pick landed on node 3 and corrupted a run.
        check(o.node_seed == 0x99AF76329BA60F33ull, "the node seed survives");
        check(r.pos == r.len, "consumed exactly the frame");
    }
    {
        // An event choice carries the stat key rather than a type, and kind 0
        // must round-trip as cleanly as kind 1.
        ChoiceMsg m{};
        m.kind = kChoiceEvent; m.index = 0; m.count = 2;
        strcpy_s(m.name, "con");
        uint32_t n = enc_choice(buf, sizeof(buf), m);
        Reader r(buf, n); r.u8v();
        ChoiceMsg o{};
        check(dec_choice(r, o), "an event choice decodes");
        check(o.kind == kChoiceEvent && o.aux == 0, "kind 0, no aux");
        check(strcmp(o.name, "con") == 0, "the stat key survives");
        // Zero is a legitimate value meaning "the sender did not know which
        // node it was in", and the receiver must read it as unknown rather
        // than as a mismatch -- see apply_pending.
        check(o.node_seed == 0, "an unstated node seed round-trips as zero");
    }
    {
        ChoiceMsg m{}; m.index = 1; m.count = 3; strcpy_s(m.name, "leave");
        uint32_t n = enc_choice(buf, sizeof(buf), m);
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader r(buf, cut); r.u8v();
            ChoiceMsg o{};
            dec_choice(r, o);
        }
        check(true, "truncated CHOICE frames parse without faulting");
        Reader r(buf, n - 1); r.u8v();
        ChoiceMsg o{};
        check(!dec_choice(r, o), "one byte short is rejected");
    }

    printf("\n-- NodeHashMsg: the meta layer's per-node check --\n");
    {
        NodeHashMsg m{};
        m.node_seed = 0xABCAAC1E04A8C2C0ull; m.node_index = 6;
        m.point = kNodePointEvent;
        m.rng[0] = 1; m.rng[1] = 2; m.rng[2] = 3; m.rng[3] = 4;
        m.hist_hash = 0x1111222233334444ull;
        m.cats_hash = 0x5555666677778888ull;
        m.cat_count = 29;
        m.inv_hash  = 0x9999AAAABBBBCCCCull;
        strcpy_s(m.event, "data/events/alley_cat.gon");

        uint32_t n = enc_nodehash(buf, sizeof(buf), m);
        check(n > 0, "encodes");
        Reader r(buf, n);
        check(r.u8v() == MSG_NODEHASH, "message type is NODEHASH");
        NodeHashMsg o{};
        check(dec_nodehash(r, o), "decodes");
        check(o.node_seed == m.node_seed, "the node seed is the identity");
        check(o.point == kNodePointEvent, "the sample point");
        // All four words, not just the first: the log prints rng[0] but the
        // COMPARISON is over the whole 32-byte state, so a codec that carried
        // one word would agree on streams that had genuinely diverged.
        check(o.rng[0] == 1 && o.rng[1] == 2 && o.rng[2] == 3 && o.rng[3] == 4,
              "all four words of the simulation stream survive");
        check(o.hist_hash == m.hist_hash, "the run-history hash");
        check(o.cats_hash == m.cats_hash && o.cat_count == 29, "the cat roster");
        check(o.inv_hash == m.inv_hash, "the inventory hash");
        check(strcmp(o.event, m.event) == 0, "the chosen event's name");
        check(r.pos == r.len, "consumed exactly the frame");
    }
    {
        NodeHashMsg m{}; m.node_seed = 7; m.point = kNodePointEnter;
        uint32_t n = enc_nodehash(buf, sizeof(buf), m);
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader r(buf, cut); r.u8v();
            NodeHashMsg o{};
            dec_nodehash(r, o);
        }
        check(true, "truncated NODEHASH frames parse without faulting");
    }

    printf("\n-- RunHistMsg: the used-event list --\n");
    {
        uint8_t body[64];
        for (uint32_t i = 0; i < sizeof(body); ++i) body[i] = (uint8_t)(i * 7 + 1);
        RunHistMsg m{};
        m.size = sizeof(body);
        m.data = body;
        m.hash = savefile_hash(body, sizeof(body));

        uint8_t big[256];
        uint32_t n = enc_runhist(big, sizeof(big), m);
        check(n > 0, "encodes");
        Reader r(big, n);
        check(r.u8v() == MSG_RUNHIST, "message type is RUNHIST");
        RunHistMsg o{};
        check(dec_runhist(r, o), "decodes");
        check(o.size == sizeof(body), "size");
        check(o.data != nullptr && memcmp(o.data, body, sizeof(body)) == 0,
              "the serialized bytes survive intact");
        check(o.hash == m.hash, "the hash rides along");
        free(o.data);

        // A zero-length or absurd length must be refused rather than turned
        // into an allocation -- same contract as dec_catdata.
        RunHistMsg bad{}; bad.size = 0; bad.data = body;
        check(enc_runhist(big, sizeof(big), bad) == 0, "a zero-length history is refused");
    }

    printf("\n-- StateDumpMsg: the desync dump --\n");
    {
        // 40 records of 26 bytes is not the real CatState; the point is that
        // the codec carries whatever the sender packs and refuses anything
        // whose count, stride and size do not agree with each other.
        const uint32_t kCount = 40, kStride = 26;
        uint8_t body[kCount * kStride];
        for (uint32_t i = 0; i < sizeof(body); ++i) body[i] = (uint8_t)(i * 11 + 3);

        StateDumpMsg m{};
        m.battle_id = 0xdeadbeefcafe1234ull;
        m.turn = 21; m.count = kCount; m.stride = kStride;
        m.size = kCount * kStride; m.data = body;

        uint8_t big[2048];
        uint32_t n = enc_statedump(big, sizeof(big), m);
        check(n > 0, "encodes");
        Reader r(big, n);
        check(r.u8v() == MSG_STATEDUMP, "message type is STATEDUMP");
        StateDumpMsg o{};
        check(dec_statedump(r, o), "decodes");
        check(o.battle_id == m.battle_id, "battle id");
        check(o.turn == 21, "the turn the table was hashed at");
        check(o.count == kCount && o.stride == kStride, "count and stride");
        check(o.data != nullptr && memcmp(o.data, body, sizeof(body)) == 0,
              "every cat record survives intact");
        free(o.data);

        // The check that stops two builds from confidently diffing nonsense:
        // a size that does not equal count * stride is malformed, not a hint.
        uint8_t bad[2048];
        memcpy(bad, big, n);
        bad[1 + 8 + 4 + 4 + 4] ^= 0x01;   // corrupt the low byte of `size`
        Reader rb(bad, n); rb.u8v();
        StateDumpMsg ob{};
        check(!dec_statedump(rb, ob), "size != count * stride is refused");
        check(ob.data == nullptr, "and it allocated nothing");

        StateDumpMsg empty{}; empty.data = body;
        check(enc_statedump(big, sizeof(big), empty) == 0,
              "an empty dump is refused rather than sent");
    }

    printf("\n-- a truncated frame fails, it does not read past the end --\n");
    {
        ActionMsg a{}; a.type = 2; a.turn = 9; strcpy_s(a.gon, "Spit");
        uint32_t n = enc_action(buf, sizeof(buf), a);
        for (uint32_t cut = 1; cut < n; ++cut) {
            Reader r(buf, cut); r.u8v();
            ActionMsg b{};
            dec_action(r, b);          // must not crash; result is don't-care
        }
        check(true, "every truncation length parsed without faulting");

        Reader r(buf, n - 1); r.u8v();
        ActionMsg b{};
        check(!dec_action(r, b), "one byte short is rejected");
    }

    printf("\n-- an over-long string is truncated without desynchronising --\n");
    {
        // A 300-byte name is malformed input, not a transport error. The cursor
        // must still advance by the whole field or every later field shifts.
        Writer w(buf, sizeof(buf));
        w.u8v(MSG_ACTION);
        w.u64v(9);                      // battle_id, ahead of turn on the wire
        w.u32v(1); w.u8v(0); w.u8v(2); w.u8v(0); w.u8v(0); w.u8v(0); w.u8v(0);
        w.i32v(0); w.i32v(0); w.i32v(0); w.i32v(0);
        char big[200]; memset(big, 'x', sizeof(big)); big[199] = 0;
        w.str(big);                     // Writer clamps to 255
        w.u32v(0xFEEDFACE);             // a sentinel after the string

        Reader r(buf, w.len); r.u8v();
        ActionMsg a{};
        check(dec_action(r, a), "decodes");
        check(a.battle_id == 9 && a.turn == 1, "battle_id and turn survive ahead of it");
        check(strlen(a.gon) == sizeof(a.gon) - 1, "name truncated to the field");
        check(r.u32v() == 0xFEEDFACE, "the cursor still lands on the next field");
    }

    printf("\n-- SAVEFILE carries the bytes, and owns them on arrival --\n");
    {
        // A stand-in for the real thing: the shipped saves are sqlite3 files
        // that open with this literal, and the transfer is byte-for-byte, so
        // the header is the cheapest end-to-end sanity check there is.
        uint8_t save[4096];
        memcpy(save, "SQLite format 3", 16);
        for (size_t i = 16; i < sizeof(save); ++i) save[i] = (uint8_t)(i * 31u);

        SaveFileMsg out{};
        out.slot = 2;
        out.size = (uint32_t)sizeof(save);
        out.hash = savefile_hash(save, out.size);
        strcpy(out.name, "steamcampaign03.sav");
        out.data = save;

        uint32_t cap = savefile_frame_size(out);
        uint8_t* frame = (uint8_t*)malloc(cap);
        uint32_t n = enc_savefile(frame, cap, out);
        check(n > out.size, "encodes to more than the payload");

        Reader r(frame, n);
        check(r.u8v() == MSG_SAVEFILE, "type byte");
        SaveFileMsg in{};
        check(dec_savefile(r, in), "decodes");
        check(in.slot == 2, "slot survives");
        check(in.size == out.size, "size survives");
        check(strcmp(in.name, "steamcampaign03.sav") == 0, "name survives");
        check(in.data != nullptr, "the decoder allocated the payload");
        check(in.data != save, "and it is a copy, not the sender's buffer");
        check(memcmp(in.data, save, out.size) == 0, "bytes survive intact");
        check(savefile_hash(in.data, in.size) == out.hash, "hash agrees with the sender's");
        free(in.data);
        free(frame);
    }

    printf("\n-- a truncated or oversized SAVEFILE allocates nothing --\n");
    {
        uint8_t payload[64] = {};
        SaveFileMsg out{};
        out.slot = 0; out.size = sizeof(payload);
        out.hash = savefile_hash(payload, out.size);
        strcpy(out.name, "steamcampaign01.sav");
        out.data = payload;

        uint8_t frame[512];
        uint32_t n = enc_savefile(frame, sizeof(frame), out);
        check(n != 0, "encodes");

        // Chop the last byte off: the header still parses, the body does not.
        Reader r(frame, n - 1); r.u8v();
        SaveFileMsg in{};
        check(!dec_savefile(r, in), "a short frame is rejected");
        check(in.data == nullptr, "and nothing is left allocated");

        // A length nobody could honour must be refused before the malloc.
        Writer w(frame, sizeof(frame));
        w.u8v(MSG_SAVEFILE);
        w.u32v(0); w.u32v(kMaxSaveBytes + 1); w.u64v(0); w.str("x.sav");
        Reader r2(frame, w.len); r2.u8v();
        SaveFileMsg in2{};
        check(!dec_savefile(r2, in2), "an oversized length is rejected");
        check(in2.data == nullptr, "and allocates nothing");

        // Zero-length is not a save file, and neither is a null one.
        SaveFileMsg empty{};
        empty.size = 0; empty.data = payload;
        check(enc_savefile(frame, sizeof(frame), empty) == 0, "enc refuses size 0");
        SaveFileMsg nodata{};
        nodata.size = 16; nodata.data = nullptr;
        check(enc_savefile(frame, sizeof(frame), nodata) == 0, "enc refuses a null payload");
    }

    printf("\n-- INVENTORY: three blobs, and an empty one is not an error --\n");
    {
        // The distinction that matters. dec_catdata treats size 0 as malformed
        // because a cat always serializes to something; an inventory bucket is
        // legitimately empty, and a decoder that rejected that would refuse to
        // sync a run whose trash can happened to be empty -- while an encoder
        // that dropped it would leave the client's old contents in place,
        // because the client APPLIES a bucket by clearing and repopulating.
        static uint8_t back[300], store[7];
        for (size_t i = 0; i < sizeof(back);  ++i) back[i]  = (uint8_t)(i * 5 + 1);
        for (size_t i = 0; i < sizeof(store); ++i) store[i] = (uint8_t)(0xA0 + i);

        InventoryMsg out{};
        out.coins = -3; out.food = 41; out.boxes = 0;   // negative: it is an int
        out.hash  = 0x0123456789ABCDEFull;
        out.size[0] = sizeof(back);  out.data[0] = back;
        out.size[1] = sizeof(store); out.data[1] = store;
        out.size[2] = 0;             out.data[2] = nullptr;   // empty trash

        static uint8_t frame[4096];
        uint32_t n = enc_inventory(frame, sizeof(frame), out);
        check(n != 0, "encodes");
        check(n <= inventory_frame_size(out), "fits the advertised frame size");

        Reader r(frame, n);
        check(r.u8v() == MSG_INVENTORY, "type byte");
        InventoryMsg in{};
        check(dec_inventory(r, in), "decodes");
        check(in.coins == -3 && in.food == 41 && in.boxes == 0, "scalars survive");
        check(in.hash == out.hash, "hash survives");
        check(in.size[0] == sizeof(back) && in.size[1] == sizeof(store), "sizes");
        check(in.size[2] == 0 && in.data[2] == nullptr, "an empty bucket stays empty");
        check(in.data[0] && memcmp(in.data[0], back, sizeof(back)) == 0, "backpack bytes");
        check(in.data[1] && memcmp(in.data[1], store, sizeof(store)) == 0, "storage bytes");
        check(r.pos == n, "consumes the whole frame");
        for (uint32_t i = 0; i < kInvBuckets; ++i) free(in.data[i]);

        // All three empty is a real inventory too -- a fresh run. Its own
        // buffer, so the truncation checks below still measure the full frame.
        static uint8_t frame_empty[128];
        InventoryMsg none{};
        uint32_t n2 = enc_inventory(frame_empty, sizeof(frame_empty), none);
        check(n2 != 0, "an entirely empty inventory still encodes");
        Reader r2(frame_empty, n2); r2.u8v();
        InventoryMsg in2{};
        check(dec_inventory(r2, in2), "and decodes");
        check(in2.size[0] == 0 && in2.size[1] == 0 && in2.size[2] == 0, "as empty");

        // Truncation and absurd lengths must be refused before any malloc.
        Reader r3(frame, n - 1); r3.u8v();
        InventoryMsg in3{};
        check(!dec_inventory(r3, in3), "a short frame is rejected");
        for (uint32_t i = 0; i < kInvBuckets; ++i)
            check(in3.data[i] == nullptr, "and leaves nothing allocated");

        Writer w(frame, sizeof(frame));
        w.u8v(MSG_INVENTORY);
        w.i32v(0); w.i32v(0); w.i32v(0); w.u64v(0);
        w.u32v(8); w.u32v(kMaxInvBytes + 1); w.u32v(0);
        Reader r4(frame, w.len); r4.u8v();
        InventoryMsg in4{};
        check(!dec_inventory(r4, in4), "an oversized bucket length is rejected");
        for (uint32_t i = 0; i < kInvBuckets; ++i)
            check(in4.data[i] == nullptr, "including the bucket before it");

        // A length with no bytes behind it is an encoder bug, not a wire state.
        InventoryMsg bad{};
        bad.size[0] = 16; bad.data[0] = nullptr;
        check(enc_inventory(frame, sizeof(frame), bad) == 0,
              "enc refuses a length with a null payload");
    }

    printf("\n-- the encoders refuse a buffer that is too small --\n");
    {
        uint8_t tiny[4];
        Hello h{};
        check(enc_hello(tiny, sizeof(tiny), h) == 0, "enc_hello returns 0, not garbage");
        ActionMsg a{}; a.type = 2;
        check(enc_action(tiny, sizeof(tiny), a) == 0, "enc_action returns 0");
    }

    printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
