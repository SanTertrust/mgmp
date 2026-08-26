#pragma once
// mgmp_nodehash -- the meta layer's per-node hash. The check the battle layer
// has had per turn since protocol version 5 and this half of the game never
// had at all.
//
// WHY IT IS NEEDED, IN ONE SENTENCE: the meta layer is synced by DETERMINISM
// plus a replicated choice, and determinism sync fails silently.
//
// The longer version is that it fails worse than silently. The per-node CATDATA
// and INVENTORY pushes OVERWRITE a divergence rather than report it, so a meta
// split does not surface where it happened -- it surfaces later, somewhere
// else, as a battle desync with nothing in either log to connect the two. The
// unexplained turn-0 mismatch of 2026-08-25 has exactly that shape: it followed
// a node that resolved two level-ups and an event, and diverged in both rng and
// state before a single turn had been played.
//
// WHAT IS HASHED, AND WHY EACH FIELD EARNED ITS PLACE. These are the inputs the
// event roller and the node itself actually read -- see mgmp_runhist.h for the
// reverse engineering behind the first two:
//
//   rng[4]     TLS+0x178, the simulation stream.
//   hist_hash  the run history -- the used-event list the pool draw skips over.
//   cats_hash  the run's cat ids IN ORDER, because sub_1400AACD0 indexes that
//              list with one xoshiro step to choose the event's subject cat. A
//              different order is a different cat from the same draw.
//   cat_count  carried beside the hash so a mismatch says whether the rosters
//              are different lengths or merely different.
//   inv_hash   coins, food, boxes and the three bucket counts. Cheap, and it
//              covers the one thing an event most often changes.
//   event[]    at the event sample point only: the name of the event that was
//              actually chosen.
//
// TWO SAMPLE POINTS, answering different questions -- see NodeHashMsg.
//
// IT REPORTS, IT DOES NOT HALT. Deliberately, and not for the same reason the
// battle layer halts. A battle desync means every subsequent turn is fiction; a
// meta divergence usually means one number differs and the run is still
// playable, and stopping a co-op session dead over a coin count would be worse
// than telling the players. `net_nodehash_halt` exists for a session that wants
// the strict behaviour, and defaults off.
//
// SYMMETRIC. Both peers compute and send their own; each compares arrivals
// against a ring of what it sent, in both directions, for the reason
// mgmp_hashring.h spells out -- a host-authoritative hash could only ever
// report that the CLIENT disagreed, and it is the host that is authoritative
// about the run, so the direction that matters is the one it cannot carry.

#include <cstdint>

namespace mgmp {

struct NodeHashMsg;

// Only the MewDirector pointer VARIABLE, which is data and has no prologue to
// check. Everything read through it is range-checked at the point of use, the
// way mgmp_catsync validates the same slot.
void nodehash_set_base(uintptr_t base);

void nodehash_init();
void nodehash_shutdown();

// Called by mgmp_follow from both peers' paths into a node, immediately before
// EnterNode runs. See kNodePointEnter.
void nodehash_on_node(uint64_t node_seed, uint32_t node_index);

// Called from the WorldEvent::update hook. Samples once per screen, on the
// first tick at which the event's name can be read.
void nodehash_on_event_screen(void* world_event);

void nodehash_on_message(uint8_t from, const NodeHashMsg& m);

} // namespace mgmp
