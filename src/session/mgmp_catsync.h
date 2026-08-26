#pragma once
// mgmp_catsync -- the host's cats are pushed to the client, as bytes the GAME
// produced, through the game's own serializer.
//
// THE BUG THIS CLOSES, measured 2026-08-24. Both peers loaded byte-identical
// saves (hash 78887bfc13cd5b04 on both). The host then equipped a trinket during
// the adventure. Four turns into the next battle:
//
//   !! HALT at turn 4: cat 19 slot 4:4 (gon 'tk_GlowingCoin') is empty on this peer
//
// A save file syncs the run at the moment it is written and nothing after it, so
// every meta-layer change since then is invisible. Worse, it is invisible to the
// detector too: `state_hash` covers HP, shield, max HP, tile, facing and dead --
// not abilities and not equipment -- so the peers agreed on every hash right up
// to the halt. What caught it was the ability cross-check (resolve by slot,
// validate by GON name), which is the one thing in the battle layer that looks
// at what a cat can DO rather than at what has happened to it.
//
// WHY BYTES AND NOT A REPLAYED ACTION. Replicating the click was the obvious
// plan, and the map layer's success makes it look easy. It is a dead end:
//
//   - glaiel::InventoryItemBox::click() @ 0x14034DF60 IS a clean boundary by the
//     usual test -- exactly one code caller, only data xref is .pdata, so not
//     virtual. But it is `void click(void)`: everything about what was clicked
//     lives in `this`, a UI box that exists only while the inventory screen is
//     open, and the client is not in that screen.
//   - it only STARTS a flow. Confirmation popup, then an AbilityChooser -- a
//     second decision point -- then lambdas whose actual mutation fans out over
//     ~8 CatData helpers. The bottom of that stack, sub_1400B3920, has 40 call
//     sites: a shared container primitive, the same trap as
//     CustomVector<T*>::push_back.
//
// So replaying means driving a UI flow on a peer that is not in it, and
// reimplementing means reproducing eight mutations exactly, where a single miss
// diverges SILENTLY. Both are worse than the halt they would replace.
//
// WHAT MAKES THE THIRD OPTION WORK:
//
//   void glaiel::SerializeCatData(CatData&, ByteStream&, bool)  @ 0x14022E9A0
//
// is BIDIRECTIONAL. Every field branches on the stream mode at ByteStream+0x00
// (0 = read, 1 = write), so the same function that saves a cat on the host loads
// one on the client. We reimplement nothing; we move bytes the game wrote into
// the game's own reader. The format even carries its own version tag (19) as its
// first field, and both peers run the same pinned build.
//
// Identity is free here, unlike everywhere else in this project. CatData+0x00 is
// a u64 id, serialized right after that version tag, and the run holds a
// registry that maps it to a CatData*. No index scheme, no name cross-check, no
// pointer that dies between runs.
//
// This is RUNSTATE from CLAUDE.md's protocol table, arriving early and scoped to
// cats.
//
// WHAT IT DOES NOT COVER, stated plainly: equipping MOVES an item from the run
// inventory onto the cat. Syncing CatData alone leaves the client with the item
// on the cat AND still in its inventory -- a duplicate. The run inventory is
// separate state and needs the same treatment; nothing here or in `state_hash`
// will notice. See "the inventory half" in the .cpp.

#include <cstdint>

namespace mgmp {

struct CatDataMsg;

// Resolves and prologue-checks the game functions this module calls. Called
// from hooks_verify_module alongside rng_set_base, because a bad address here
// is a refusal at startup rather than a crash three screens later.
void catsync_set_base(uintptr_t base);

void catsync_init();
void catsync_shutdown();

// HOST: serialize every cat in the run, send the ones whose bytes changed since
// the last push. Called where the host enters a map node -- the point both peers
// already synchronize on, and the last moment before a battle can be built out
// of a stale cat. Cheap and idempotent: unchanged cats cost a serialize and a
// hash compare, and send nothing.
void catsync_publish(const char* why);

// HOST: forget what was already sent, so the next catsync_publish is a FULL
// push. For a reconnecting peer -- see the comment on the definition.
void catsync_forget();

// CLIENT: resolve m.id in this peer's run and deserialize over it.
//
// A cat that arrives while this peer is in a battle is HELD, not applied and
// not dropped -- writing it would edit state the fight derives from outside the
// hashed stream, and dropping it loses it for good, because the host dedupes
// against the last hash it sent.
void catsync_on_message(const CatDataMsg& m);

// CLIENT: apply every held cat, if any. Called from the map-follow tick,
// immediately before this peer enters the node the host entered -- the same
// point invsync_apply_pending uses, and for the same two reasons: it is off the
// battle screen, and it is still in time for whatever the node opens.
void catsync_apply_pending(const char* why);

} // namespace mgmp
