#pragma once
// mgmp_runhist -- the run history at *(MewDirector+1424), pushed whole from the
// host, through the game's own bidirectional serializer.
//
// WHY THIS OBJECT EXISTS ON THE WIRE AT ALL.
//
// Because the event a node shows is NOT a pure function of the node seed, which
// is what the whole "replicate the choice, not the effect" design had assumed.
// MapScreen::select_event @ 0x140395D10 delegates to sub_1408DA560, and that
// function reads three things before it reads the stream:
//
//   1. THE RUN'S CAT ROSTER. sub_1400AACD0 is a single inline xoshiro step used
//      to index the list, so the same stream state picks a different cat if the
//      list differs in order or length. The chosen cat is the event's subject.
//   2. THAT CAT'S PASSIVES. `ExcludeFromEvents` filters the pool;
//      `ChanceToForceEvent` can force one outright, on a rand2 gated by the
//      cat's own stat. An event can therefore be decided by a cat rather than
//      by a roll.
//   3. THE USED-EVENT LIST -- tracker+96, tested by sub_1408D9EB0 and appended
//      to by sub_1408D9E50. The pool draw retries until it finds one that is
//      not in it.
//
// So two peers holding identical streams and identical cats still roll
// different events the moment their used-event lists differ by a single entry.
// Nothing pushed that list: CATDATA covers cats, INVENTORY covers items, and
// this is neither. It rides in the SAVE (sub_1408DD2F0 is called only from
// save_adventure and ContinueAdventure), which is exactly why rejoining always
// appeared to "fix" the run and why a session that never rejoined drifted
// further and further apart.
//
// Observed shape of that drift, 2026-08-25: an event whose options read 'dex'
// where the host had 'int', and three nodes later two peers building event
// screens with different numbers of options entirely.
//
// WHAT IT ALSO HOLDS: the per-node-type counters bumped on every return to the
// map (two 19-int arrays, one slot per MapNodeType), a handful of scalars, and
// two more string lists beside the used-event one.
//
// WHOLE OBJECT, NOT A DIFF, for the reason mgmp_invsync gives about items: the
// entries have no portable identity worth reconciling and the object is small.
// One serialize per node, deduped against the last push by hash, so an
// unchanged history costs one serialize and a compare and sends nothing.
//
// WHAT THIS DOES NOT DO. It does not make the meta layer verified -- it removes
// one silent divergence source. The verification is mgmp_nodehash, and the two
// are deliberately separate: this module can be wrong in a way that keeps both
// peers WRONG TOGETHER, and only a hash computed independently on each side
// would notice.

#include <cstdint>

namespace mgmp {

struct RunHistMsg;

// Resolves and prologue-checks sub_1408DD2F0. Called from hooks_install beside
// catsync_set_base, and scoped to its own call the way that one is: drift in an
// address belonging to another module must not turn THIS feature off under the
// wrong name.
void runhist_set_base(uintptr_t base);

void runhist_init();
void runhist_shutdown();

// Host: serialize and send, unless the bytes are what we sent last time.
void runhist_publish(const char* why);

// Drop the "already sent this" cache so the next publish sends whatever the
// bytes are. Same contract, and the same reason, as catsync_forget: the dedupe
// is per-RUN, not per-peer, so a peer that reconnects would otherwise be told
// nothing at all about a history that has not changed since the last node.
void runhist_forget();

// Client: apply the host's copy over ours.
// A history that arrives while this peer is in a battle is HELD, not applied
// and not dropped -- this is the object the event roller reads, so writing it
// mid-fight changes what the next node rolls, and dropping it is permanent
// (the host dedupes against the last hash it sent).
void runhist_on_message(const RunHistMsg& m);

// CLIENT: apply the held run history, if there is one. Called from the
// map-follow tick, immediately before this peer enters the node the host
// entered -- the same point invsync_apply_pending and catsync_apply_pending use.
void runhist_apply_pending(const char* why);

// FNV-1a over this peer's own serialized run history, or 0 if it cannot be
// read. Used by mgmp_nodehash; costs one serialize, so it is called at node
// boundaries and nowhere else.
uint64_t runhist_hash();

} // namespace mgmp
