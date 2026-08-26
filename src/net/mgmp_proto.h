// mgmp_proto.h -- the wire format. Phase 4, layer 2.
//
// WHAT GOES ON THE WIRE, AND WHY IT IS SO LITTLE.
//
// Only TurnAction types 2 (use ability) and 3 (end turn) are ever transmitted.
// Types 6 (reaction broadcast) and 7 (invoke a std::function) are generated
// locally on both peers and must NOT be sent -- settled exhaustively in phase 1:
//
//   - ApplyTurnAction has exactly two call sites and is not virtual, so the
//     decision queue is the sole route in;
//   - all ~65 type-7 creation sites are passive/status/reaction callbacks
//     (OnAppliedToCharacter, OnReceivedDamage, CheckCounter, late_update, ...)
//     and not one of them is a brain;
//   - type 6 bypasses the queue entirely -- NextTurn calls ApplyTurnAction
//     directly at 0x1408E111E once per turn.
//
// That is what makes this protocol small enough to be a few hundred bytes a
// turn: a std::function cannot go on a socket, and it never has to.
//
// IDENTITY IS AUTHORED DATA, NOT POINTERS. Measured across runs C and D of the
// same battle, 0 of 21 ability pointers and 0 of 21 actor pointers matched. An
// ACTION therefore carries the ability's *slot* (the game's own FindAbility
// scheme: move/attack/bonus/spellN) plus its authored GON name. The receiver
// resolves by slot and validates by name -- and a disagreement between the two
// is itself a desync signal, caught before the wrong ability is ever played.
// See mgmp_ability.h for why the slot index is stable by construction.
//
// FRAMING. Length-prefixed, little-endian, u32 payload length not counting the
// length field itself. TCP is deliberate: head-of-line blocking is irrelevant
// when the game is turn-based (you are blocked on that message anyway), and a
// turn is a few hundred bytes you want reliable and ordered.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace mgmp {

// Bumped on any incompatible change to the encodings below -- AND on any change
// to what a peer COMPUTES from them, which is a wider contract than it sounds
// and was widened the hard way.
//
// Version 8 carries no new message and no changed field. What changed is the
// state hash: facing came out of it. Two peers running builds 7 and 8 handshake
// happily, agree on rng and queue, and then halt on turn 0 with a state-only
// mismatch -- which reads exactly like a game desync and is not one. The peer's
// hash in that halt was byte-identical to the value BOTH peers had agreed on in
// the previous session, which is the tell, and it cost a session to spot.
//
// So: if the value a peer puts in a message changes meaning, that is a protocol
// change even when every byte stays where it was. Refusing at connect is free;
// diagnosing it from a turn-0 halt is not.
constexpr uint32_t kProtoVersion = 26;  // 2 CONTROL, 3 ENTERNODE, 4 SAVEFILE, 5 epoch, 6 CATDATA, 7 INVENTORY, 8 state hash drops facing + roster cap 254, 9 peer envelope + PEERS (up to 4 players), 10 state hash gains ElementList (tiles + equipment), 11 CURSOR, 12 state hash gains live-list membership and stops hashing departed cats, 13/14/15 CURSOR churn while the pointer moved from the board to the screen, 16 CURSOR carries the cursor-art index -- the peer's pointer is now the game's own texture for the state their game is in, 17 CHOICE -- event and level-up option choices, which is what replaces RUNSTATE, 18 the per-battle epoch COUNTER becomes a u64 battle_id -- the node seed both peers already share -- so battle identity survives a peer restarting, 19 CHOICE carries the node seed it was made on -- a held choice used to have no idea which node it belonged to and could surface on a later one, 20 RUNHIST (the run-history object the event roller reads) + NODEHASH (the meta layer finally gets the per-node check the battle layer has had per turn since version 5), 21 AIM -- the range/AOE tiles the other player is aiming at, drawn on this peer by the game's own Brain::DrawAbilityAOE; cosmetic like CURSOR and hashed by nothing, 22 AIM is read from the PLAYERBRAIN'S SELECTION (PlayerBrain+0x3D8/+0x358/+0x360) instead of the cached decision, and the receiver draws the RANGE tiles as well as the AOE -- not one byte of AimMsg moved, which is exactly the kind of change version 12 established has to bump anyway, 23 the range-tile call that came with 22 is REMOVED -- sub_140138A10 applies statuses rather than drawing, so mirroring it mutated the non-owning peer's simulation and cost a run; the bump exists so a peer still running 22 cannot join and do it, 24 the highlight is back and the tiles with it, but only ever called with sub_140151CE0 -- its apply_status half -- swallowed by T_HighlightRefresh and with the whole roster's cat state fenced across the call, 25 a Move aim also shows the ATTACK RANGE from the hovered square -- the game gets those tiles by displacing the cat and moving it back, so the receiver now makes two TacticsObject::Move calls per frame inside the same state fence; not one byte of AimMsg moved, version 12's rule again, 26 STATEDUMP -- on a hash mismatch each peer sends the per-cat table its own hash was taken over, so the log that halts also names the cat and the field instead of requiring two log files side by side

// A frame's payload may not exceed this. RUNSTATE (phase 5) is the only message
// that will ever approach it; everything in phase 4 is under 128 bytes.
constexpr uint32_t kMaxPayload = 8u << 20;   // 8 MiB

// --- more than two players --------------------------------------------------
//
// TOPOLOGY IS A STAR AND THE HOST IS THE HUB. Not a mesh, for three reasons,
// and the third is the one that decides it:
//
//   1. The host already owns the run. Every meta-layer push (SAVEFILE, CATDATA,
//      INVENTORY, ENTERNODE) is host-authored, so the hub exists either way.
//   2. Three links at four players instead of six.
//   3. ONLY THE HOST NEEDS A REACHABLE PORT. In a mesh every player forwards a
//      port to every other player. That is the difference between "my friend
//      joined" and an evening of router configuration.
//
// A client's ACTION reaches the other clients as host -> relay. The extra hop
// costs a round trip that a turn-based game does not notice, and the relay is
// done on the host's RECEIVE thread, so it never touches the game thread.
//
// Peer 0 is always the host. Client ids are handed out at accept time and are
// never reused within a session, so an id survives another peer disconnecting.
// The control split is derived from a peer's POSITION in the sorted id list
// rather than from the raw id, so a gap left by a departed peer does not shift
// anybody's cats.
constexpr uint8_t kMaxPeers = 4;
constexpr uint8_t kHostPeer = 0;
constexpr uint8_t kNoPeer   = 0xFF;

// THE FRAME ENVELOPE.
//
//     [u32 len][u8 from][payload ...]        len counts `from` + payload
//
// `from` is the id of the peer that ORIGINATED the message, not the id of the
// peer we received it from. The host overwrites it when relaying, which is what
// lets client B tell an action of client A's apart from one of the host's.
//
// It lives in the envelope rather than in each message for the same reason the
// epoch does not: every message needs it, including the ones added later, and a
// field that must be remembered in nine encoders is a field that will be
// forgotten in the tenth.
constexpr uint32_t kEnvelopeBytes = 1;

enum MsgType : uint8_t {
    MSG_HELLO    = 1,   // both, at connect
    MSG_WELCOME  = 2,   // host -> client: accepted, here is your control set
    MSG_REFUSE   = 3,   // either: incompatible, with a human-readable reason
    MSG_ACTION   = 4,   // acting peer -> other: one type-2 or type-3 decision
    MSG_HASH     = 5,   // both, at each turn boundary
    MSG_HALT     = 6,   // on desync: stop and dump
    MSG_PING     = 7,   // keepalive; carries nothing
    MSG_CONTROL  = 8,   // both, per battle: this peer's half of the split
    MSG_ENTERNODE= 9,   // host -> client: the map node the host entered
    MSG_SAVEFILE =10,   // host -> client: the whole save file, bytes and all
    MSG_CATDATA  =11,   // host -> client: one cat, serialized by the game itself
    MSG_INVENTORY=12,   // host -> client: the whole run inventory
    MSG_PEERS    =13,   // host -> each client: who is in the session, and who you are
    MSG_CURSOR   =14,   // both, while in a battle: the tile this peer is pointing at
    MSG_CHOICE   =15,   // host -> client: which option the host picked on a
                        // WorldEvent or LevelUpScreen
    MSG_RUNHIST  =16,   // host -> client: the run-history object, whole
    MSG_NODEHASH =17,   // both, at each map node: the meta layer's own hash
    MSG_AIM      =18,   // acting peer -> other: what the human at the other
                        // keyboard is currently AIMING, before they commit it
    MSG_STATEDUMP=19,   // both, ONLY after a hash has already disagreed: the
                        // per-cat table that hash was taken over, so the peer
                        // can print the actual diff instead of two log files
                        // that have to be lined up by hand
};

// A save file is a plain sqlite3 database (the shipped ones start with the
// literal "SQLite format 3 ") and the campaign save measures about 45 KB. The
// cap is generous but bounded: a garbled length must not make the receiver
// allocate wildly, and there is no legitimate save anywhere near this size.
constexpr uint32_t kMaxSaveBytes = 4u << 20;   // 4 MiB

// ---------------------------------------------------------------------------
// Little-endian cursor writer/reader.
//
// Hand-rolled rather than memcpy-of-struct because the struct layout must not
// leak onto the wire: TurnAction+0x04 is uninitialised padding that differs
// between runs for no reason (it read 0x85 in one copy and the ASCII "pone" --
// stale stack -- in another), and anything that blits a struct will eventually
// carry a field like that across and desync two peers over garbage.
// ---------------------------------------------------------------------------
struct Writer {
    uint8_t* buf;
    uint32_t cap;
    uint32_t len = 0;
    bool     ok  = true;

    Writer(uint8_t* b, uint32_t c) : buf(b), cap(c) {}

    void raw(const void* p, uint32_t n) {
        if (!ok || len + n > cap) { ok = false; return; }
        memcpy(buf + len, p, n);
        len += n;
    }
    void u8v(uint8_t v)   { raw(&v, 1); }
    void u32v(uint32_t v) { raw(&v, 4); }
    void u64v(uint64_t v) { raw(&v, 8); }
    void i32v(int32_t v)  { raw(&v, 4); }

    // Length-prefixed string, u8 length. Every string in this protocol is a GON
    // name or a diagnostic reason, both well under 256 bytes.
    void str(const char* s) {
        size_t n = s ? strlen(s) : 0;
        if (n > 255) n = 255;
        u8v((uint8_t)n);
        raw(s, (uint32_t)n);
    }
};

struct Reader {
    const uint8_t* buf;
    uint32_t       len;
    uint32_t       pos = 0;
    bool           ok  = true;

    Reader(const uint8_t* b, uint32_t n) : buf(b), len(n) {}

    void raw(void* p, uint32_t n) {
        if (!ok || pos + n > len) { ok = false; return; }
        memcpy(p, buf + pos, n);
        pos += n;
    }
    uint8_t  u8v()  { uint8_t v = 0;  raw(&v, 1); return v; }
    uint32_t u32v() { uint32_t v = 0; raw(&v, 4); return v; }
    uint64_t u64v() { uint64_t v = 0; raw(&v, 8); return v; }
    int32_t  i32v() { int32_t v = 0;  raw(&v, 4); return v; }

    // Always NUL-terminates when it succeeds. On overflow the field is
    // truncated and `ok` stays true -- a 300-byte GON name is malformed input,
    // not a transport error, and it will fail the name cross-check anyway.
    void str(char* out, uint32_t out_size) {
        if (!out_size) { ok = false; return; }
        out[0] = 0;
        uint8_t n = u8v();
        if (!ok) return;
        if (pos + n > len) { ok = false; return; }
        uint32_t copy = n < out_size - 1 ? n : out_size - 1;
        memcpy(out, buf + pos, copy);
        out[copy] = 0;
        pos += n;                          // skip the whole field, not just the
                                           // part that fit -- truncating must
                                           // not desynchronise the cursor.
    }
};

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

// Sent by both peers immediately on connect.
//
// gpak_hash is NOT optional and the connection is refused without a match.
// Both peers need byte-identical game data because RollChance @ 0x14094B550
// takes NO draw when p >= 1.0 -- so the stream *position* depends on which
// procs were possible, not on which fired. Two peers that disagree about
// whether a chance was 0.9 or 1.0 desync even when the visible outcome is the
// same. That has to be caught here, not on turn 30.
struct Hello {
    uint32_t proto      = kProtoVersion;
    uint64_t gpak_hash  = 0;   // resources.gpak content hash
    uint64_t build_hash = 0;   // Mewgenics.exe build identity
    char     name[64]   = {};  // display name, diagnostics only
};

// Host -> client, after a HELLO it accepts. `cats` is the set of cat indices
// the CLIENT may decide for; everything else is the host's. Both peers simulate
// every cat identically -- this is a control split, not a state split.
struct Welcome {
    // The simulation stream, TLS+0x178, all four xoshiro256 words. Sending it
    // is what makes "copy the host's save" unnecessary in the long run: the
    // client does not need to reproduce the host's RNG *history*, only to
    // arrive at the same state. Entering a battle does not re-seed -- measured
    // across four separate launches, all of which entered at the identical
    // s0=967e2d6d328620b1 -- so the state travels with the save, and sending it
    // here overrides whatever the client's own save would have supplied.
    uint64_t rng_state[4]   = {};
    uint8_t  cat_count      = 0;
    uint8_t  cats[32]       = {};
};

// Host -> each client, individually rather than broadcast, because `you` is
// different in every copy. Re-sent to everyone whenever the membership changes,
// so a peer that joins third does not leave the first two believing they are
// splitting the cats two ways.
//
// `ids` is sorted ascending and always contains kHostPeer. A peer's INDEX in
// this array -- not its id -- is its position in the control split, which is
// what keeps the split stable across a disconnect that leaves a gap.
struct PeersMsg {
    uint8_t you            = kNoPeer;   // the receiving peer's own id
    uint8_t count          = 0;         // members, host included
    uint8_t ids[kMaxPeers] = {};
};

// THE BATTLE EPOCH, carried by every per-battle message (ACTION, HASH, CONTROL).
//
// Turn numbering restarts at 0 in every battle, so a turn index alone does not
// identify anything: a message still in flight when both peers cross a battle
// boundary arrives looking exactly like a message about the battle they just
// entered. A stale HASH would then be compared against the new battle's turn N
// and manufacture a desync out of nothing; a stale ACTION is worse, because it
// would be injected as a real decision for whichever cat index it names.
//
// The identity is `battle_id`: the 64-bit node seed the battle was entered
// with, read out of MapNode+0x118 by the MapScreen::EnterNode hook on BOTH
// peers. It is not negotiated, not counted and not derived from anything local
// -- it is the same number on both machines because they entered the same node,
// which is the same reason this protocol has never needed a SEED message.
//
// IT USED TO BE A PER-PROCESS COUNTER, AND THAT BROKE ON RECONNECT. g.epoch
// started at 0 in lockstep_init and only ever incremented, so a client that
// relaunched sat at 0 while the host was at 108. All three consume paths gate
// on exact equality -- pend_take, verify_control, the hash matcher -- so the
// client discarded the host's actions, the host discarded the client's as
// stale, and the join barrier never opened. A silent stall in the first battle
// after any reconnect.
//
// Adopting the host's counter on join was the other candidate and is worse,
// because the bump is OBSERVATIONAL: ++epoch fires when lockstep notices the
// character list changed at a turn boundary, so "which epoch is this battle"
// is not knowable at the moment of adoption -- a host mid-fight has already
// bumped, a host on an event node has not and will not. A shared value has no
// such moment. Same rule as ability slots and CatData ids: identity comes from
// something both processes can read, never from a number one of them counted.
//
// Three cases on receipt, and they are genuinely different:
//
//   a RETIRED battle   the sender is still talking about a battle we have left.
//                      Drop it. This is the race the field exists to close.
//   our current one    normal.
//   anything else      the sender reached a battle we have not. HOLD it:
//                      dropping here is what made a peer entering a battle late
//                      lose the actions taken without it (see the late-join gap
//                      in CLAUDE.md), and holding costs nothing because the
//                      queues are keyed on battle_id and will not hand one over
//                      early.
//
// Ordering came free with a counter and does not with an id, which is why the
// receiver keeps a small set of the battle_ids it has RETIRED. That set is the
// only thing separating "old" from "not yet" -- and unlike a counter it stays
// correct across a restart, because it is populated from battles this peer
// actually played rather than from how many it has counted.
//
// Deliberately NOT a halt condition. A peer legitimately running a battle ahead
// or behind produces both mismatches, so halting on them would fire constantly
// on exactly the lag the lockstep is built to tolerate.

// Host -> client, one cat of the run, as the GAME serializes it.
//
// WHY THIS EXISTS. A save file syncs the run at the moment it is written, and
// nothing after. The host then equips a trinket, and the peers diverge in a way
// nothing detects until the battle: `state_hash` covers HP, shield, max HP,
// tile, facing and dead -- not abilities, not equipment -- so the first sign is
// the ability cross-check halting mid-battle with "slot 4:4 (gon
// 'tk_GlowingCoin') is empty on this peer". Measured, 2026-08-24.
//
// WHY IT IS BYTES AND NOT AN ACTION. Replaying the equip was the obvious plan
// and it is a dead end. InventoryItemBox::click() is `void click(void)` on a UI
// box that exists only while the inventory screen is open -- the client is not
// in that screen -- and it merely STARTS a flow (confirmation popup, then an
// AbilityChooser, which is a second decision point) whose actual mutation fans
// out across ~8 CatData helpers. Replaying means driving a UI the peer is not
// in; reimplementing means reproducing 8 mutations exactly, where one miss
// diverges silently. Both are worse than the halt they replace.
//
// So we ship the RESULT instead, through the game's own code:
//
//   glaiel::SerializeCatData(CatData&, ByteStream&, bool)  @ 0x14022E9A0
//
// It is bidirectional -- every field branches on the stream mode at
// ByteStream+0x00 (0 = read, 1 = write) -- so the same function that saves a cat
// on the host loads it on the client. Nothing is reimplemented, and the format
// carries its own version tag (19) as the first field.
//
// `id` is CatData+0x00, serialized right after that version tag and resolvable
// through the run's registry. A real identity, so unlike abilities this needs no
// index scheme and no name cross-check.
struct CatDataMsg {
    uint64_t id    = 0;    // CatData+0x00
    uint32_t size  = 0;
    uint64_t hash  = 0;    // FNV-1a over the bytes; also the host's change check
    uint8_t* data  = nullptr;  // encode: borrowed. decode: owned by the receiver.
};

// A serialized cat is well under a kilobyte in practice. The cap is generous but
// bounded, for the same reason kMaxSaveBytes is: a garbled length must not make
// the receiver allocate wildly.
constexpr uint32_t kMaxCatBytes = 256u * 1024u;

// Host -> client, the whole run inventory, sent alongside CATDATA at every map
// node.
//
// WHY IT EXISTS. Equipping an item MOVES it: out of the run inventory, onto the
// cat. CATDATA syncs the cat half and nothing synced the other, so the client
// kept a duplicate of every item the host equipped -- an item on the cat AND
// still in the backpack. Nothing detected it either: `state_hash` covers HP,
// shield, max HP, tile, facing and dead, and the ability cross-check only looks
// at what the cat can do, so a spare copy of a trinket sitting in a bag on one
// peer is invisible until it is used.
//
// WHY IT IS THREE BLOBS AND NOT AN ITEM LIST. Items have no portable identity.
// On load each Equipment takes `++qword_1413BD998`, a process-global object-id
// counter that fifteen other classes also mint from and that is never written
// to or read from the stream. Host and client mint different ids for the same
// item, so no message may key on one -- which rules out reconciling item by
// item and leaves pushing each bucket whole. Unlike CatData, whose +0x00 id IS
// serialized and IS resolvable, an item's only persistent identity is its GON
// name plus its stat fields.
//
// The three buckets are the three the game itself saves: backpack, storage,
// trash. `size` may legitimately be 0 -- an empty bag is a normal state and
// must survive the round trip, which is why this decoder accepts a zero length
// where dec_catdata treats it as malformed.
//
// The three scalars ride along because they are the rest of what the inventory
// write driver persists, and they are plain ints on the Inventory object -- no
// serializer, no blob, nothing to intercept.
constexpr uint32_t kInvBuckets  = 3;             // backpack, storage, trash
constexpr uint32_t kMaxInvBytes = 512u * 1024u;  // per bucket

struct InventoryMsg {
    int32_t  coins = 0, food = 0, boxes = 0;
    // FNV-1a over the scalars and all three blobs. Doubles as the host's
    // change check, so an unchanged inventory costs three serializes and a
    // compare and sends nothing.
    uint64_t hash = 0;
    uint32_t size[kInvBuckets] = {};
    uint8_t* data[kInvBuckets] = {};  // encode: borrowed. decode: owned.
};

// One decision. This is the entire battle protocol.
struct ActionMsg {
    uint64_t battle_id  = 0;   // which battle; see above
    uint32_t turn       = 0;
    uint8_t  actor      = 0;   // cat index; a cross-check, see mgmp_lockstep.h
    uint8_t  type       = 0;   // 2 = ability, 3 = end turn. Never 6 or 7.
    uint8_t  slot_kind  = 0;   // SLOT_* from mgmp_ability.h
    uint8_t  slot_index = 0;
    uint8_t  b30        = 0;   // TurnAction+0x30, forwarded to Ability::Prime
    uint8_t  b31        = 0;   // TurnAction+0x31
    int32_t  tx = 0, ty = 0;   // target tile
    int32_t  dx = 0, dy = 0;   // direction
    char     gon[64]    = {};  // authored ability name -- the second identity
};

// Exchanged at every turn boundary. Cheap because the game is turn-based.
//
// The queue depth belongs in here as much as the state hash does: types 6 and 7
// exist *because* a passive fired, so if a proc roll differs between peers the
// queue populations diverge immediately -- before any visible state does. It is
// the earliest and cheapest divergence signal available, and it costs no
// serialization of the ~1390 component classes.
struct HashMsg {
    uint64_t battle_id   = 0;   // which battle; see the note above ActionMsg
    uint32_t turn        = 0;
    uint64_t rng_hash    = 0;   // over the 32-byte TLS+0x178 state
    uint64_t state_hash  = 0;   // positions / HP / status counts
    uint32_t queue_depth = 0;   // TurnControl+0x60
    uint32_t queue_sig   = 0;   // rolling hash of pending action types
};

// Sent by BOTH peers once per battle, at their own roster snapshot.
//
// It is a VERIFICATION, not an assignment, and that distinction is what keeps
// it free of races. Both peers derive the same split locally from the same
// roster -- byte-identical by measurement: 29 cats, same brain class per index,
// two processes with completely different heap addresses -- so neither waits
// for this to arrive before playing.
//
// What it buys is the check that used to be impossible. `Welcome` could catch
// two peers claiming the SAME cat, but a cat claimed by NEITHER was invisible
// and surfaced only as a battle that stalled on that cat's turn with nothing in
// either log to explain it. Two lists plus a shared roster make both halves
// checkable, and sending it in both directions puts the result in both logs.
//
// `humans` is included for the same reason the roster is printed: if the peers
// disagree on how many cats a human drives, every index below is meaningless,
// and saying so beats diffing the lists.
struct ControlMsg {
    uint64_t battle_id = 0;   // which battle; see the note above ActionMsg
    uint32_t humans   = 0;    // human-brained cats the sender counted
    uint8_t  count    = 0;    // how many of them the sender claims
    uint8_t  cats[32] = {};   // their indices, ascending
};

// Both directions, while a battle is on screen: the board tile this peer is
// pointing at. The first message in this protocol that exists to be SEEN.
//
// A TILE, NOT A PIXEL, and that is the whole reason this is cheap. Two peers
// run at different resolutions, with independently panned cameras and different
// UI scale, so a screen-space cursor would land somewhere else on every other
// machine. A tile index means the same square on every screen, and the game
// already computes it for us: StatusMenu caches the tile under the mouse at
// StatusMenu+124 every frame (bounds-checked against the grid at StatusMenu+72,
// width at +184, height at +188), which is what drives its own ground pip.
//
// PURELY COSMETIC, AND DELIBERATELY OUTSIDE THE LOCKSTEP CONTRACT. Nothing here
// is hashed, nothing here is replayed, and a dropped or stale CURSOR cannot
// desync anything -- the worst case is a cursor that lags or disappears. That
// is why it may be sent on a throttle rather than at a command boundary, and
// why it carries the epoch only so a message crossing a battle boundary can be
// dropped instead of drawn on the wrong board.
//
// `on_board` is separate from the tile rather than encoded as a sentinel
// because "pointing at nothing" is the common case -- reading a tooltip, moving
// to the end-turn button -- and a cursor that freezes at the last tile it
// touched reads as a stuck peer rather than an idle one.
//
// `owns_turn` is what the transparency rule keys on. The receiver could in
// principle derive it (both peers agree on the roster and the split), but the
// sender knows it without ambiguity and one bit is cheaper than making the
// display depend on the control check having completed.
// What the human at the other keyboard is pointing an ability at, BEFORE they
// commit it -- the range/AOE tiles solo players see while they aim.
//
// Deliberately in the same class as CURSOR and not in the same class as ACTION:
// nothing here is hashed, replayed or acknowledged, a dropped or late one can
// only make a preview flicker, and it is therefore allowed to be sent on a
// timer rather than at a command boundary. An AIM is NOT a decision -- the same
// aim can be sent a hundred times and then abandoned. The decision, when it
// comes, still arrives as an ACTION and nothing about that changes.
//
// The three fields that matter are exactly the arguments the game itself passes
// to glaiel::Brain::DrawAbilityAOE @ 0x14013A030 from Brain::UpdateDecision:
//
//     mov rdx, [rdi+228h]     ; the Ability*  -> slot_kind / slot_index here
//     mov r8,  [rdi+230h]     ; iVec2D target -> tx, ty
//     mov r9,  [rdi+238h]     ; iVec2D dir    -> dx, dy
//
// The ability crosses the wire as a SLOT, never a pointer, for the reason the
// whole project settled long ago: 0 of 21 ability pointers matched between two
// runs. The GON name rides along as the same independent cross-check ACTION
// uses -- resolve by slot, validate by name.
struct AimMsg {
    uint64_t battle_id  = 0;   // which battle; a mismatch is dropped, not halted
    uint8_t  cat        = 0;   // roster index of the cat being aimed
    uint8_t  active     = 0;   // 0 = stopped aiming, draw nothing
    uint8_t  slot_kind  = 0;   // SLOT_* from mgmp_ability.h
    uint8_t  slot_index = 0;
    int32_t  tx = 0, ty = 0;   // target tile
    int32_t  dx = 0, dy = 0;   // direction
    char     gon[64]    = {};  // authored ability name -- the second identity
};

struct CursorMsg {
    uint64_t battle_id = 0;   // which battle; a mismatch is dropped, never halted
    int32_t  x         = 0;   // tile coordinates into the tactics grid
    int32_t  y         = 0;
    uint8_t  on_board  = 0;   // 0 = pointing off the grid; draw nothing
    uint8_t  owns_turn = 0;   // 1 = the cat currently deciding is this peer's


    // Where the sender's MOUSE is, as a fraction of its own window: 0,0 is the
    // top-left corner and 1,1 the bottom-right. Resolution-independent by
    // construction, which a pixel position would not be.
    //
    // This is the one field in the message that is NOT a board fact, and the
    // difference matters. The tile above means the same square on every
    // machine whatever the camera is doing; the fraction means the same place
    // on the SCREEN, which is the same place on the board only while both
    // players have the camera in the same position. That is why both are sent:
    // mgmp_cursor draws the tile, mgmp_overlay draws the fraction, and the
    // reticle stays right even when the pointer is merely close.
    float    nx        = 0.0f;
    float    ny        = 0.0f;

    // WHICH CURSOR the sender's game is showing -- an index into the art table
    // in mgmp_overlay.cpp, whose entries are the shipped textures/cursor/*.png
    // file names. The receiver draws that same texture, so a peer hovering a
    // button shows the pointing hand and a peer aiming a spell shows the spell
    // cursor.
    //
    // An INDEX, not the state string: the string is a texture name the receiver
    // would have to trust enough to concatenate into a path, and both peers are
    // the same build by construction (HELLO checks the build hash). The cost is
    // that the table's ORDER is wire format -- appending is safe, reordering is
    // a protocol change. An index we do not know draws the plain arrow.
    uint8_t  mode      = 0;
};

// Host -> client, every time the host enters a map node.
//
// The map is the one meta screen that HAS a command boundary --
// MapScreen::EnterNode(MapScreen*, MapNode*) @ 0x140391050, one code caller and
// not virtual -- so following the host through it costs one message rather than
// a state push.
//
// `seed0` is the important field and it is not decoration. EnterNode's first
// act is to copy MapNode+0x118..0x137 into TLS+0x178, i.e. the node carries the
// 32-byte xoshiro256 state the battle will run on. If the two peers' nodes hold
// different seeds, the battle starts from different RNG and desyncs on its
// first roll -- so comparing one word of it here names the cause at the moment
// it is still fixable, instead of surfacing as a turn-3 hash mismatch.
//
// It also means the protocol never has to SEND a seed: entering the same node
// is what makes the streams equal.
struct EnterNodeMsg {
    uint32_t index      = 0;   // index into MapScreen's node vector (+0x7C/+0x80)
    uint32_t node_count = 0;   // the sender's map size, so a bad index is caught
    uint32_t type       = 0;   // MapNodeType: 5 battle, 6 hard, 8 boss, 12 shop...
    uint64_t seed0      = 0;   // first word of MapNode+0x118
};

// Host -> client: the option the host picked on a decision screen.
//
// THIS IS THE MESSAGE THAT REPLACES RUNSTATE, AND THE REASON IT CAN IS
// DETERMINISM RATHER THAN SERIALIZATION.
//
// Both peers enter the same map node, and EnterNode copies MapNode+0x118 into
// the simulation stream TLS+0x178 UNCONDITIONALLY, before any node-type
// dispatch (0x1403910C2..0x1403910E5; the only branch ahead of it is the shift
// key). So an event node hands both peers a byte-identical RNG state, by the
// same mechanism that already makes battles deterministic. Every RNG draw found
// on the event path lands on that stream: the item-pool draw at 0x1408DB3FE
// (get_item_from_pool, 340 shipped occurrences -- the most common effect in the
// game), random_chance's randfloat, reward's RollChance,
// party_skip_next_fight_chance's rand2.
//
// The level-up roller looks like the exception -- it runs on a HEAP stream at
// object+0xB8 rather than the global one -- and it is not. That state is seeded
// splitmix64(CatData+0x00, the cat's persistent id) and then advanced by
// CatData+0xC30 xoshiro jumps (0x140379CA5..0x140379CE7), and BOTH fields are
// round-tripped by SerializeCatData. It is a pure function of cat state the
// client already has, which is presumably why Tyler wrote it that way: it makes
// a reroll reproducible for a given cat.
//
// What is therefore NOT derivable is the one thing no seed contains: which
// button a person pressed. That is all this message carries. The 119 authored
// effect commands, the familiar list at MewDirector+1576, the next-fight spawn
// queue at +1552, the adventure tokens at +1664, the legacy counters in the
// save-cache at +56 -- none of them need a wire format, because both peers
// compute them from the same seed and the same choice.
//
// RESOLVE BY INDEX, VALIDATE BY NAME. Both screens hold their offered options
// as a 240-byte-stride array (WorldEvent+224..+232, LevelUpScreen+864..+872)
// built in authored file order from data both peers loaded out of the same
// resources.gpak, which HELLO already hashes. The index is stable by
// construction. The name rides along anyway and a mismatch is shouted about,
// because that is the cross-check that caught the equipment bug in the battle
// layer: a disagreement means the two option lists were BUILT differently, and
// that deserves a loud line rather than a silently wrong choice.
//
// NOT hashed, not acknowledged, not replayed -- but unlike CURSOR it is also
// not droppable. A lost CHOICE leaves the client sitting on a screen forever,
// which is a visible stall rather than a wrong game, and that is the trade this
// whole layer already makes everywhere else.
// A CHOICE IS ABOUT ONE NODE, AND UNTIL PROTO 19 IT DID NOT SAY WHICH.
//
// Measured 2026-08-25, and it is the whole reason `node_seed` exists. The host
// entered map node 6 (an event), picked option 0 ('int') and walked on to node
// 17 before the client's map tick had entered node 6 at all. The client held
// the choice -- correctly, its option list did not exist -- and then applied it
// TWO NODES LATER to node 3's event, whose option 0 was 'dex'. The name
// cross-check saw the disagreement, reported it, and obeyed it, because a
// differing label was assumed to mean two option lists built differently rather
// than two different events:
//
//   client  !! event option 0 is 'dex' here but 'int' on the host --
//              taking it anyway, the lists are the same length
//   host    (no event choice published at node 3 at all)
//
// From there the two runs genuinely differed -- different effects applied to
// different cats -- and three nodes later the peers were rolling different
// events outright (2 options against 3), at which point the client's screen
// refused every option and the run had to be restarted.
//
// The lesson is the one this protocol has now learned four times: A PER-SCREEN
// INDEX IS NOT AN IDENTITY. Ability slots, CatData ids and battle_id all
// replaced a local number with something both processes can read, and this is
// the same replacement for the fourth time. The node seed is free -- both peers
// already read MapNode+0x118 to agree on battle_id -- and it makes "is this
// choice about the screen in front of me" answerable instead of assumed.
struct ChoiceMsg {
    uint8_t  kind  = 0;     // kChoiceEvent or kChoiceLevelUp
    uint32_t index = 0;     // option index into that screen's array
    uint32_t count = 0;     // options the sender had, so a bad index is caught
    uint32_t aux   = 0;     // level-up: the LevelUpOption type at +0. event: 0.
    uint64_t node_seed = 0; // MapNode+0x118 of the node the sender was standing
                            // in. 0 means "the sender did not know", which is
                            // treated as "do not check" rather than as a
                            // mismatch -- see choice_on_message.
    char     name[48] = {}; // event: the stat key at entry+64 ("con", "coins",
                            // "none"...). level-up: the option name at +200.
};

constexpr uint8_t kChoiceEvent   = 0;
constexpr uint8_t kChoiceLevelUp = 1;

// Host -> client, once the host has committed to a save file.
//
// This is the bluntest possible answer to "both peers must start from the same
// state", and it is blunt on purpose: the save is a single self-contained
// sqlite3 file, so shipping it whole needs none of the ~1390-class
// serialization that RUNSTATE will. CLAUDE.md's own first-milestone advice was
// "copy the host's save file, do not match checkpoints" -- this is that, done
// by the mod instead of by hand.
//
// WHY IT CARRIES A SLOT INDEX AS WELL AS A NAME. The name is what the host's
// SaveSelection called the file ("steamcampaign01.sav"); the slot is where it
// sat in that screen's name vector. They are not redundant, because
// SaveSelection::init builds one of TWO name arrays -- campaignNN.sav or
// steamcampaignNN.sav -- depending on the build. Two peers could therefore
// agree on the slot and disagree on the name. The client resolves by name and
// falls back to the slot, which makes the mismatch survivable instead of fatal.
//
// `data` is NOT copied by the flat NetMsg queue: the decoder heap-allocates it
// and the consumer must hand the frame back to net_msg_release. That is the one
// allocation on the receive path in this protocol, and it is affordable because
// it happens once per session rather than once per turn.
struct SaveFileMsg {
    uint32_t slot = 0;         // index into SaveSelection's name vector
    uint32_t size = 0;         // bytes of `data`
    uint64_t hash = 0;         // FNV-1a over those bytes, checked after writing
    char     name[64] = {};    // the host's filename for the slot
    uint8_t* data = nullptr;   // encode: borrowed. decode: owned by the receiver.
};

// Host -> client: the run-history object at *(MewDirector+1424), whole.
//
// WHY IT HAS TO BE SYNCED, WHICH WAS NOT OBVIOUS AND IS NOT OPTIONAL.
//
// The event a node shows is NOT a pure function of the node seed. MapScreen::
// select_event @ 0x140395D10 hands off to sub_1408DA560, which:
//
//   1. picks a RANDOM CAT out of the run's roster -- sub_1400AACD0 is one
//      inline xoshiro step used to index the list, so the same stream state
//      picks a different cat if the list differs;
//   2. reads that cat's ExcludeFromEvents (a pool filter) and
//      ChanceToForceEvent (which can FORCE a specific event outright, on a
//      rand2 gated by the cat's own stat);
//   3. draws from the pool in a retry loop that SKIPS EVERY EVENT ALREADY
//      USED -- the used-event list living at tracker+96, tested by
//      sub_1408D9EB0 and appended to by sub_1408D9E50.
//
// So two peers with identical streams and identical cats still roll different
// events the moment their used-event lists differ by one entry. The object is
// in the save (sub_1408DD2F0 is called only from save_adventure and
// ContinueAdventure), which is why a rejoin always "fixed" it and why nothing
// caught it in a session that never rejoined -- CATDATA and INVENTORY were the
// only run state the mod pushed, and this is neither.
//
// It also holds the per-node-type counters bumped on every return to the map
// (two 19-int arrays, one slot per MapNodeType).
//
// WHOLE OBJECT, VIA THE GAME'S OWN BIDIRECTIONAL SERIALIZER, for exactly the
// reasons CatData is done that way: sub_1408DD2F0 branches on ByteStream+0x00
// and carries its own version tag, so the host writes and the client reads with
// no format reimplemented here and nothing to keep in step when Tyler adds a
// field.
struct RunHistMsg {
    uint32_t size = 0;         // bytes of `data`
    uint64_t hash = 0;         // FNV-1a over those bytes
    uint8_t* data = nullptr;   // encode: borrowed. decode: owned by the receiver.
};

// A serialized run history is a few strings and two 19-int arrays. The bound is
// loose enough for a long run's used-event list and tight enough that a garbled
// length cannot make the receiver allocate wildly.
constexpr uint32_t kMaxRunHistBytes = 1u << 20;   // 1 MiB

// Both peers, at every map node: the meta layer's own hash.
//
// THE BATTLE LAYER HAS HAD A PER-TURN HASH SINCE VERSION 5 AND THE META LAYER
// HAD NOTHING, which is the asymmetry this closes. The battle layer earned its
// hash because lockstep fails silently; the meta layer is now synced by
// DETERMINISM plus a replicated choice, which fails silently in exactly the
// same way and had no check at all.
//
// Worse than nothing, in fact: the per-node CATDATA and INVENTORY pushes
// OVERWRITE a divergence rather than report it, so a meta-layer split surfaced
// later, somewhere else, as a battle desync. That is the shape of the
// unexplained turn-0 mismatch of 2026-08-25 (cluster A), which followed a node
// that resolved two level-ups and an event.
//
// TWO SAMPLE POINTS, because they answer different questions:
//
//   kNodePointEnter -- taken by both peers immediately before EnterNode. The
//       rng words are therefore the state the PREVIOUS node left behind, which
//       is the drift signal: EnterNode is about to overwrite it from
//       MapNode+0x118, so a disagreement here is the last moment it is
//       attributable to what just happened rather than to what is about to.
//   kNodePointEvent -- taken on the first WorldEvent::update of a screen. By
//       then the event has been chosen, so this one carries its NAME. Two peers
//       reporting different names is the reported bug, named outright, instead
//       of being inferred three screens later from an option-count refusal.
//
// SYMMETRIC, like CONTROL and unlike every other meta message: both peers send
// their own and check the other's. A host-authoritative hash could only ever
// report that the client disagreed, and it is the host that is authoritative
// about the run -- so the interesting direction is the one a host-only message
// cannot carry.
struct NodeHashMsg {
    uint64_t node_seed  = 0;    // which node this is about -- the same 64 bits
                                // battle_id and ChoiceMsg::node_seed use
    uint32_t node_index = 0;    // for the log line only; seed is the identity
    uint8_t  point      = 0;    // kNodePointEnter / kNodePointEvent
    uint64_t rng[4]     = {};   // TLS+0x178, the simulation stream
    uint64_t hist_hash  = 0;    // over the serialized run history (RunHistMsg)
    uint64_t cats_hash  = 0;    // over the run's cat ids, IN ORDER -- the list
                                // sub_1400AACD0 indexes to pick the event's cat
    uint32_t cat_count  = 0;
    uint64_t inv_hash   = 0;    // coins/food/boxes + the three bucket counts
    char     event[48]  = {};   // kNodePointEvent: WorldEvent+0x1A10. Else "".
};

constexpr uint8_t kNodePointEnter = 0;
constexpr uint8_t kNodePointEvent = 1;

struct HaltMsg {
    uint32_t turn = 0;
    char     reason[192] = {};
};

// THE DESYNC DUMP. Sent by both peers, once, at the moment a hash disagrees --
// and at no other time, which is what keeps it free.
//
// A hash says THAT two peers diverged and can never say what by. The cat table
// was already printed locally, but only into this peer's own log, so naming the
// field that moved meant getting both log files onto one screen and lining up
// forty-odd rows by eye. The state each peer hashed is a few kilobytes; sending
// it once, after the run is already lost, costs nothing and turns that into one
// line per differing field.
//
// It carries the state AS OF THE HASHED TURN, not as of now. That distinction
// is the whole reliability of the thing: a mismatch can be noticed when the
// peer's hash arrives, which is not necessarily the boundary we took ours at,
// and comparing a stale row against a fresh one manufactures differences that
// were never there.
//
// `stride` is sizeof(the sender's record). It is checked rather than assumed:
// two peers built from different revisions would otherwise reinterpret each
// other's bytes and print a confident diff of nonsense -- exactly the failure
// this message exists to prevent one level up.
constexpr uint32_t kMaxDumpCats  = 254;
constexpr uint32_t kMaxDumpBytes = kMaxDumpCats * 256u;

struct StateDumpMsg {
    uint64_t battle_id = 0;
    uint32_t turn      = 0;
    uint32_t count     = 0;          // records in `data`
    uint32_t stride    = 0;          // bytes per record, as the SENDER packs it
    uint32_t size      = 0;          // count * stride
    uint8_t* data      = nullptr;    // encode: borrowed. decode: owned.
};

inline uint32_t statedump_frame_size(const StateDumpMsg& m) { return m.size + 64; }

inline uint32_t enc_statedump(uint8_t* p, uint32_t cap, const StateDumpMsg& m) {
    if (!m.data || m.size == 0 || m.size > kMaxDumpBytes) return 0;
    Writer w(p, cap);
    w.u8v(MSG_STATEDUMP);
    w.u64v(m.battle_id); w.u32v(m.turn);
    w.u32v(m.count); w.u32v(m.stride); w.u32v(m.size);
    w.raw(m.data, m.size);
    return w.ok ? w.len : 0;
}

// Allocates m.data on success and nothing on any failure, so a partial frame
// cannot leak. Same contract as dec_catdata.
inline bool dec_statedump(Reader& r, StateDumpMsg& m) {
    m.data = nullptr;
    m.battle_id = r.u64v();
    m.turn      = r.u32v();
    m.count     = r.u32v();
    m.stride    = r.u32v();
    m.size      = r.u32v();
    if (!r.ok) return false;
    if (m.count == 0 || m.count > kMaxDumpCats) return false;
    if (m.stride == 0 || m.size == 0 || m.size > kMaxDumpBytes) return false;
    if (m.size != m.count * m.stride) return false;
    if (r.pos + m.size > r.len) return false;
    uint8_t* buf = (uint8_t*)malloc(m.size);
    if (!buf) return false;
    memcpy(buf, r.buf + r.pos, m.size);
    r.pos += m.size;
    m.data = buf;
    return true;
}

// --- encode -----------------------------------------------------------------

inline uint32_t enc_hello(uint8_t* p, uint32_t cap, const Hello& h) {
    Writer w(p, cap);
    w.u8v(MSG_HELLO);
    w.u32v(h.proto); w.u64v(h.gpak_hash); w.u64v(h.build_hash); w.str(h.name);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_welcome(uint8_t* p, uint32_t cap, const Welcome& v) {
    Writer w(p, cap);
    w.u8v(MSG_WELCOME);
    for (int i = 0; i < 4; ++i) w.u64v(v.rng_state[i]);
    w.u8v(v.cat_count);
    for (uint8_t i = 0; i < v.cat_count && i < 32; ++i) w.u8v(v.cats[i]);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_action(uint8_t* p, uint32_t cap, const ActionMsg& a) {
    Writer w(p, cap);
    w.u8v(MSG_ACTION);
    w.u64v(a.battle_id);
    w.u32v(a.turn);
    w.u8v(a.actor); w.u8v(a.type); w.u8v(a.slot_kind); w.u8v(a.slot_index);
    w.u8v(a.b30);   w.u8v(a.b31);
    w.i32v(a.tx); w.i32v(a.ty); w.i32v(a.dx); w.i32v(a.dy);
    w.str(a.gon);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_hash(uint8_t* p, uint32_t cap, const HashMsg& h) {
    Writer w(p, cap);
    w.u8v(MSG_HASH);
    w.u64v(h.battle_id);
    w.u32v(h.turn); w.u64v(h.rng_hash); w.u64v(h.state_hash);
    w.u32v(h.queue_depth); w.u32v(h.queue_sig);
    return w.ok ? w.len : 0;
}

inline uint32_t catdata_frame_size(const CatDataMsg& m) { return m.size + 64; }

inline uint32_t enc_catdata(uint8_t* p, uint32_t cap, const CatDataMsg& m) {
    if (!m.data || m.size == 0 || m.size > kMaxCatBytes) return 0;
    Writer w(p, cap);
    w.u8v(MSG_CATDATA);
    w.u64v(m.id);
    w.u32v(m.size);
    w.u64v(m.hash);
    w.raw(m.data, m.size);
    return w.ok ? w.len : 0;
}

inline uint32_t runhist_frame_size(const RunHistMsg& m) { return m.size + 64; }

inline uint32_t enc_runhist(uint8_t* p, uint32_t cap, const RunHistMsg& m) {
    if (!m.data || m.size == 0 || m.size > kMaxRunHistBytes) return 0;
    Writer w(p, cap);
    w.u8v(MSG_RUNHIST);
    w.u32v(m.size);
    w.u64v(m.hash);
    w.raw(m.data, m.size);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_nodehash(uint8_t* p, uint32_t cap, const NodeHashMsg& m) {
    Writer w(p, cap);
    w.u8v(MSG_NODEHASH);
    w.u64v(m.node_seed); w.u32v(m.node_index); w.u8v(m.point);
    for (int i = 0; i < 4; ++i) w.u64v(m.rng[i]);
    w.u64v(m.hist_hash); w.u64v(m.cats_hash); w.u32v(m.cat_count);
    w.u64v(m.inv_hash);
    w.str(m.event);
    return w.ok ? w.len : 0;
}

inline uint32_t inventory_frame_size(const InventoryMsg& m) {
    uint32_t n = 64;
    for (uint32_t i = 0; i < kInvBuckets; ++i) n += m.size[i];
    return n;
}

inline uint32_t enc_inventory(uint8_t* p, uint32_t cap, const InventoryMsg& m) {
    for (uint32_t i = 0; i < kInvBuckets; ++i) {
        if (m.size[i] > kMaxInvBytes) return 0;
        if (m.size[i] && !m.data[i])  return 0;   // a length with no bytes
    }
    Writer w(p, cap);
    w.u8v(MSG_INVENTORY);
    w.i32v(m.coins); w.i32v(m.food); w.i32v(m.boxes);
    w.u64v(m.hash);
    // Every length first, then every payload. Keeping the sizes contiguous
    // lets the decoder validate the whole frame's arithmetic before it
    // allocates anything, so a garbled length cannot make it allocate one
    // buffer and then discover the next is impossible.
    for (uint32_t i = 0; i < kInvBuckets; ++i) w.u32v(m.size[i]);
    for (uint32_t i = 0; i < kInvBuckets; ++i)
        if (m.size[i]) w.raw(m.data[i], m.size[i]);
    return w.ok ? w.len : 0;
}

// Allocates m.data[*] on success and the caller owns them from that moment. On
// any failure nothing stays allocated -- including buffers taken for earlier
// buckets before a later one proved impossible -- so a partial frame cannot
// leak. Same contract as dec_savefile and dec_catdata.
inline bool dec_inventory(Reader& r, InventoryMsg& m) {
    for (uint32_t i = 0; i < kInvBuckets; ++i) { m.data[i] = nullptr; m.size[i] = 0; }
    m.coins = r.i32v(); m.food = r.i32v(); m.boxes = r.i32v();
    m.hash  = r.u64v();
    if (!r.ok) return false;

    uint32_t size[kInvBuckets] = {};
    uint64_t total = 0;
    for (uint32_t i = 0; i < kInvBuckets; ++i) {
        size[i] = r.u32v();
        if (!r.ok || size[i] > kMaxInvBytes) return false;
        total += size[i];
    }
    if (r.pos + total > r.len) return false;

    for (uint32_t i = 0; i < kInvBuckets; ++i) {
        m.size[i] = size[i];
        if (!size[i]) continue;             // an empty bucket is not an error
        uint8_t* buf = (uint8_t*)malloc(size[i]);
        if (!buf) {
            for (uint32_t j = 0; j < i; ++j) { free(m.data[j]); m.data[j] = nullptr; }
            return false;
        }
        memcpy(buf, r.buf + r.pos, size[i]);
        r.pos += size[i];
        m.data[i] = buf;
    }
    return true;
}

inline uint32_t enc_control(uint8_t* p, uint32_t cap, const ControlMsg& c) {
    Writer w(p, cap);
    w.u8v(MSG_CONTROL);
    w.u64v(c.battle_id);
    w.u32v(c.humans);
    w.u8v(c.count);
    for (uint8_t i = 0; i < c.count && i < 32; ++i) w.u8v(c.cats[i]);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_peers(uint8_t* p, uint32_t cap, const PeersMsg& v) {
    Writer w(p, cap);
    w.u8v(MSG_PEERS);
    w.u8v(v.you);
    w.u8v(v.count);
    for (uint8_t i = 0; i < v.count && i < kMaxPeers; ++i) w.u8v(v.ids[i]);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_cursor(uint8_t* p, uint32_t cap, const CursorMsg& c) {
    Writer w(p, cap);
    w.u8v(MSG_CURSOR);
    w.u64v(c.battle_id);
    w.u32v((uint32_t)c.x);
    w.u32v((uint32_t)c.y);
    w.u8v(c.on_board);
    w.u8v(c.owns_turn);
    // Bit-cast rather than scaled to an integer: both peers are x86-64 running
    // the same build, so the representation is identical, and a fixed-point
    // encoding would quantise the one value whose whole point is that it moves
    // smoothly.
    uint32_t bits;
    memcpy(&bits, &c.nx, 4); w.u32v(bits);
    memcpy(&bits, &c.ny, 4); w.u32v(bits);
    w.u8v(c.mode);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_aim(uint8_t* p, uint32_t cap, const AimMsg& m) {
    Writer w(p, cap);
    w.u8v(MSG_AIM);
    w.u64v(m.battle_id);
    w.u8v(m.cat); w.u8v(m.active); w.u8v(m.slot_kind); w.u8v(m.slot_index);
    w.u32v((uint32_t)m.tx); w.u32v((uint32_t)m.ty);
    w.u32v((uint32_t)m.dx); w.u32v((uint32_t)m.dy);
    w.str(m.gon);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_enter_node(uint8_t* p, uint32_t cap, const EnterNodeMsg& m) {
    Writer w(p, cap);
    w.u8v(MSG_ENTERNODE);
    w.u32v(m.index); w.u32v(m.node_count); w.u32v(m.type); w.u64v(m.seed0);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_choice(uint8_t* p, uint32_t cap, const ChoiceMsg& m) {
    Writer w(p, cap);
    w.u8v(MSG_CHOICE);
    w.u8v(m.kind); w.u32v(m.index); w.u32v(m.count); w.u32v(m.aux);
    w.u64v(m.node_seed);
    w.str(m.name);
    return w.ok ? w.len : 0;
}

// Needs a buffer of at least savefile_frame_size(m) bytes -- far larger than
// the 512-byte stack buffer every other message uses, which is why net.cpp
// gives this one its own send path.
inline uint32_t savefile_frame_size(const SaveFileMsg& m) { return m.size + 128; }

inline uint32_t enc_savefile(uint8_t* p, uint32_t cap, const SaveFileMsg& m) {
    if (!m.data || m.size == 0 || m.size > kMaxSaveBytes) return 0;
    Writer w(p, cap);
    w.u8v(MSG_SAVEFILE);
    w.u32v(m.slot); w.u32v(m.size); w.u64v(m.hash); w.str(m.name);
    w.raw(m.data, m.size);
    return w.ok ? w.len : 0;
}

inline uint64_t savefile_hash(const void* p, uint32_t n) {
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

inline uint32_t enc_halt(uint8_t* p, uint32_t cap, const HaltMsg& h) {
    Writer w(p, cap);
    w.u8v(MSG_HALT);
    w.u32v(h.turn); w.str(h.reason);
    return w.ok ? w.len : 0;
}

inline uint32_t enc_refuse(uint8_t* p, uint32_t cap, const char* reason) {
    Writer w(p, cap);
    w.u8v(MSG_REFUSE); w.str(reason);
    return w.ok ? w.len : 0;
}

// --- decode -----------------------------------------------------------------
// Each takes a Reader already positioned past the u8 message type.

inline bool dec_hello(Reader& r, Hello& h) {
    h.proto = r.u32v(); h.gpak_hash = r.u64v(); h.build_hash = r.u64v();
    r.str(h.name, sizeof(h.name));
    return r.ok;
}

inline bool dec_welcome(Reader& r, Welcome& v) {
    for (int i = 0; i < 4; ++i) v.rng_state[i] = r.u64v();
    v.cat_count = r.u8v();
    if (v.cat_count > 32) return false;
    for (uint8_t i = 0; i < v.cat_count; ++i) v.cats[i] = r.u8v();
    return r.ok;
}

inline bool dec_action(Reader& r, ActionMsg& a) {
    a.battle_id = r.u64v();
    a.turn = r.u32v();
    a.actor = r.u8v(); a.type = r.u8v(); a.slot_kind = r.u8v(); a.slot_index = r.u8v();
    a.b30 = r.u8v();   a.b31 = r.u8v();
    a.tx = r.i32v(); a.ty = r.i32v(); a.dx = r.i32v(); a.dy = r.i32v();
    r.str(a.gon, sizeof(a.gon));
    // Reject anything that is not a real decision at the edge. Types 6 and 7
    // are locally generated; if one ever arrives, the sender is broken and
    // applying it would double-fire a reaction on this peer.
    return r.ok && (a.type == 2 || a.type == 3);
}

inline bool dec_hash(Reader& r, HashMsg& h) {
    h.battle_id = r.u64v();
    h.turn = r.u32v(); h.rng_hash = r.u64v(); h.state_hash = r.u64v();
    h.queue_depth = r.u32v(); h.queue_sig = r.u32v();
    return r.ok;
}

// Allocates m.data on success; the caller owns it from that moment. On any
// failure nothing is allocated and m.data stays null, so a partial frame cannot
// leak. Same contract as dec_savefile.
inline bool dec_catdata(Reader& r, CatDataMsg& m) {
    m.data = nullptr;
    m.id   = r.u64v();
    m.size = r.u32v();
    m.hash = r.u64v();
    if (!r.ok) return false;
    if (m.size == 0 || m.size > kMaxCatBytes) return false;
    if (r.pos + m.size > r.len) return false;
    uint8_t* buf = (uint8_t*)malloc(m.size);
    if (!buf) return false;
    memcpy(buf, r.buf + r.pos, m.size);
    r.pos += m.size;
    m.data = buf;
    return true;
}

// Allocates m.data on success; nothing on any failure, so a partial frame
// cannot leak. Same contract as dec_catdata.
inline bool dec_runhist(Reader& r, RunHistMsg& m) {
    m.data = nullptr;
    m.size = r.u32v();
    m.hash = r.u64v();
    if (!r.ok) return false;
    if (m.size == 0 || m.size > kMaxRunHistBytes) return false;
    if (r.pos + m.size > r.len) return false;
    uint8_t* buf = (uint8_t*)malloc(m.size);
    if (!buf) return false;
    memcpy(buf, r.buf + r.pos, m.size);
    r.pos += m.size;
    m.data = buf;
    return true;
}

inline bool dec_nodehash(Reader& r, NodeHashMsg& m) {
    m.node_seed  = r.u64v();
    m.node_index = r.u32v();
    m.point      = r.u8v();
    for (int i = 0; i < 4; ++i) m.rng[i] = r.u64v();
    m.hist_hash  = r.u64v();
    m.cats_hash  = r.u64v();
    m.cat_count  = r.u32v();
    m.inv_hash   = r.u64v();
    r.str(m.event, sizeof(m.event));
    return r.ok;
}

inline bool dec_control(Reader& r, ControlMsg& c) {
    c.battle_id = r.u64v();
    c.humans = r.u32v();
    c.count  = r.u8v();
    if (c.count > 32) return false;
    for (uint8_t i = 0; i < c.count; ++i) c.cats[i] = r.u8v();
    return r.ok;
}

inline bool dec_peers(Reader& r, PeersMsg& v) {
    v.you   = r.u8v();
    v.count = r.u8v();
    if (v.count == 0 || v.count > kMaxPeers) return false;
    for (uint8_t i = 0; i < v.count; ++i) v.ids[i] = r.u8v();
    if (!r.ok) return false;
    // The list must be sorted and must contain the receiver, because the split
    // is computed from a position in it. A malformed one would silently give two
    // peers the same position, which is double control of the same cat.
    bool saw_you = (v.you == v.ids[0]);
    for (uint8_t i = 1; i < v.count; ++i) {
        if (v.ids[i] <= v.ids[i - 1]) return false;
        if (v.ids[i] == v.you) saw_you = true;
    }
    return saw_you;
}

inline bool dec_aim(Reader& r, AimMsg& m) {
    m.battle_id  = r.u64v();
    m.cat        = r.u8v();
    m.active     = r.u8v();
    m.slot_kind  = r.u8v();
    m.slot_index = r.u8v();
    m.tx = (int32_t)r.u32v(); m.ty = (int32_t)r.u32v();
    m.dx = (int32_t)r.u32v(); m.dy = (int32_t)r.u32v();
    r.str(m.gon, sizeof(m.gon));
    // Both coordinates reach a game function that indexes the tactics grid, so
    // they are bounded here rather than there. Loose on purpose, the same way
    // CURSOR's are: the real grid size is only known with a live StatusMenu,
    // and a wild-but-bounded tile draws nothing instead of faulting.
    if (m.tx < -1000 || m.tx > 1000 || m.ty < -1000 || m.ty > 1000) return false;
    if (m.dx <   -8  || m.dx >    8 || m.dy <   -8  || m.dy >    8) return false;
    return r.ok;
}

inline bool dec_cursor(Reader& r, CursorMsg& c) {
    c.battle_id = r.u64v();
    c.x         = (int32_t)r.u32v();
    c.y         = (int32_t)r.u32v();
    c.on_board  = r.u8v();
    c.owns_turn = r.u8v();
    uint32_t bits = r.u32v(); memcpy(&c.nx, &bits, 4);
    bits          = r.u32v(); memcpy(&c.ny, &bits, 4);
    // A NaN or a wild fraction reaches a vertex shader, so it is rejected here
    // rather than drawn somewhere impossible. The bound is loose on purpose --
    // the sender already clamps, and a pointer half a screen off the edge is
    // better evidence of a bug than a pointer silently pinned to a corner.
    if (!(c.nx > -1.0f && c.nx < 2.0f) || !(c.ny > -1.0f && c.ny < 2.0f)) return false;
    c.mode = r.u8v();
    // A tile index reaches an array subscript on the drawing side, so it is
    // range-checked here rather than there. The bound is deliberately loose --
    // the real grid bounds are only known with a live StatusMenu -- but it
    // rejects the garbage that would matter.
    if (c.on_board && (c.x < 0 || c.y < 0 || c.x > 4095 || c.y > 4095)) return false;
    return r.ok;
}

inline bool dec_enter_node(Reader& r, EnterNodeMsg& m) {
    m.index = r.u32v(); m.node_count = r.u32v(); m.type = r.u32v(); m.seed0 = r.u64v();
    return r.ok;
}

inline bool dec_choice(Reader& r, ChoiceMsg& m) {
    m.kind  = r.u8v();
    m.index = r.u32v();
    m.count = r.u32v();
    m.aux   = r.u32v();
    m.node_seed = r.u64v();
    r.str(m.name, sizeof(m.name));
    return r.ok;
}

// Allocates m.data on success. The caller owns it from that moment; on any
// failure nothing is allocated and m.data stays null, so a partial frame cannot
// leak.
inline bool dec_savefile(Reader& r, SaveFileMsg& m) {
    m.data = nullptr;
    m.slot = r.u32v();
    m.size = r.u32v();
    m.hash = r.u64v();
    r.str(m.name, sizeof(m.name));
    if (!r.ok) return false;
    if (m.size == 0 || m.size > kMaxSaveBytes) return false;
    if (r.pos + m.size > r.len) return false;
    uint8_t* buf = (uint8_t*)malloc(m.size);
    if (!buf) return false;
    memcpy(buf, r.buf + r.pos, m.size);
    r.pos += m.size;
    m.data = buf;
    return true;
}

inline bool dec_halt(Reader& r, HaltMsg& h) {
    h.turn = r.u32v();
    r.str(h.reason, sizeof(h.reason));
    return r.ok;
}

inline const char* msg_name(uint8_t t) {
    switch (t) {
        case MSG_HELLO:   return "HELLO";
        case MSG_WELCOME: return "WELCOME";
        case MSG_REFUSE:  return "REFUSE";
        case MSG_ACTION:  return "ACTION";
        case MSG_HASH:    return "HASH";
        case MSG_HALT:    return "HALT";
        case MSG_PING:    return "PING";
        case MSG_CONTROL: return "CONTROL";
        case MSG_ENTERNODE: return "ENTERNODE";
        case MSG_SAVEFILE: return "SAVEFILE";
        case MSG_CATDATA:  return "CATDATA";
        case MSG_INVENTORY: return "INVENTORY";
        case MSG_PEERS:   return "PEERS";
        case MSG_CURSOR:  return "CURSOR";
        case MSG_CHOICE:  return "CHOICE";
        case MSG_RUNHIST: return "RUNHIST";
        case MSG_NODEHASH: return "NODEHASH";
        case MSG_AIM:     return "AIM";
        case MSG_STATEDUMP: return "STATEDUMP";
        default:          return "?";
    }
}

} // namespace mgmp
