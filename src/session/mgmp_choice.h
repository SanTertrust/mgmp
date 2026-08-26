#pragma once
// mgmp_choice -- replicate the PLAYER'S CHOICE on the two decision screens,
// instead of replicating what the choice does.
//
// This module is the answer to "how do shop, level-up and world events sync",
// and the answer turned out to be that almost none of it needs syncing.
//
// ---------------------------------------------------------------------------
// WHY A CHOICE IS ENOUGH
// ---------------------------------------------------------------------------
//
// The reflex is to replicate EFFECTS: the 119 authored commands
// WorldEvent::spin dispatches write the familiar list at MewDirector+1576, the
// next-fight spawn queue at +1552, the adventure-token vector at +1664, legacy
// counters in the save-cache at +56, and more. Shipping all of that is a
// RUNSTATE message with a field list nobody can prove is complete.
//
// None of it is necessary, because both peers already compute it identically:
//
//   1. THE CLIENT RUNS THE EVENT TOO. mgmp_follow calls the game's own
//      EnterNode on the client for the node the host entered -- every node
//      type, not just battles -- so the client opens its own WorldEvent screen.
//      (CLAUDE.md used to claim the opposite. It is wrong; see
//      core/mgmp_hooks.cpp h_MapUpdate, which has always called o_EnterNode.)
//
//   2. ENTERING A NODE RE-SEEDS THE SIMULATION STREAM, FOR EVERY NODE TYPE.
//      EnterNode copies MapNode+0x118 into TLS+0x178 at 0x1403910C2, before any
//      node-type dispatch -- the `sub eax,5 / cmp eax,1` that picks out combat
//      comes later, and the only branch ahead of the copy is the shift key. So
//      two peers begin the same event from a byte-identical RNG state.
//
//   3. EVERY DRAW ON THE EVENT PATH IS ON THAT STREAM. Checked, not assumed:
//      the item-pool draw at 0x1408DB3FE (`mov r8d,178h; mov rax,gs:58h; add
//      r8,[rax]`) which serves get_item_from_pool, the most common effect in
//      the shipped data at 340 occurrences; random_chance's randfloat at
//      0x14092C62D; reward's RollChance at 0x14092CCBA;
//      party_skip_next_fight_chance's rand2 at 0x140924649. No presentation
//      stream (+0x198) anywhere in the subtree.
//
//   4. THE LEVEL-UP ROLLER IS DETERMINISTIC TOO, DESPITE NOT BEING ON THAT
//      STREAM. It runs a HEAP xoshiro state at object+0xB8, which looks like a
//      problem until you read where it is seeded (0x140379CA5):
//
//          mov  rbx, [r15+0A0h]     ; the CatData*
//          mov  rdx, [rbx]          ; CatData+0x00 -- the cat's persistent id
//          call sub_14094ABB0       ; splitmix64(id)
//          mov  r8d, [rbx+0C30h]    ; a per-cat counter
//          call sub_14094AA50       ; xoshiro jump(), applied that many times
//
//      Both inputs live in CatData and both are round-tripped by
//      SerializeCatData (the id right after the version tag, +0xC30 at line 632
//      of the serializer). So the offered options are a pure function of cat
//      state the client already has. It also explains the design: a given cat
//      always rolls the same options, which makes a reroll save-scum-proof.
//
// So the ONE thing neither peer can derive is which button a person pressed.
// That is all that crosses the wire, and it is why this file is short and
// RUNSTATE does not exist.
//
// A LIMIT WORTH STATING. Three of the lower-frequency commands' RNG sources
// (weather_roll, add_weather, next_fight_from_set, next_event_from_set,
// scramble_*, make_old, upgrade_*) were NOT traced to a primitive -- a two-level
// scan found none and the deeper scan was skipped by choice. They are assumed
// to be on the simulation stream like everything else that was checked. If a
// desync ever shows up on an event node and the choice indices matched, these
// are the first suspects.
//
// ---------------------------------------------------------------------------
// THE TWO BOUNDARIES, WHICH ARE THE SAME SHAPE
// ---------------------------------------------------------------------------
//
// Both screens keep their offered options in a 240-BYTE-STRIDE ARRAY and commit
// through a single function, so each needs exactly one hook that does double
// duty: capture the host's pick, swallow the client's.
//
//              options array        commit                    injected by
//   event      WorldEvent+224..232  sub_140937F30(capture)    calling it
//   level-up   LevelUpScreen+864..872  select_option(opt)     sub_140386810
//
// sub_140937F30 is three statements and the middle one IS the choice:
//
//     *(u8*)(MewDirector + 1812)   = 1;             // a choice was made
//     *(void**)(WorldEvent + 256)  = capture[2];    // the chosen option entry
//     return sub_14091AA00(WorldEvent);             // run it
//
// capture is {vftable, WorldEvent*, entry*}, and `entry` is an element of that
// array -- so the host's pick is an index and injecting one is 24 bytes of
// stack. The level-up side is the mirror image: its button callback
// sub_140386810 takes {vftable, LevelUpScreen*, int index} and does the
// `+ 240*index` itself, so we call THAT rather than hand-building a
// LevelUpOption.
//
// RESOLVE BY INDEX, VALIDATE BY NAME -- the rule the battle layer already
// follows for abilities, and the one that caught the equipment bug. The index
// is stable because both peers build their option lists in authored file order
// from the same resources.gpak, which HELLO hashes. The name rides along and a
// mismatch is shouted about, because it means the two lists were BUILT
// differently, which no index can survive.
//
// ---------------------------------------------------------------------------
// WHAT THIS DOES NOT DO
// ---------------------------------------------------------------------------
//
// There is NO per-node hash yet, and that is the real debt this design takes
// on. State-push sync is self-correcting: a divergence gets overwritten by the
// next CATDATA. Determinism sync is not -- a divergence compounds silently,
// which is exactly why the battle layer carries a per-turn hash and halts. The
// meta layer now needs the same and does not have it. Until it does, the
// existing per-node CATDATA/INVENTORY pushes will paper over a divergence
// rather than report it.
#include <cstdint>

namespace mgmp {

struct ChoiceMsg;

void choice_set_base(uintptr_t base);
void choice_init();
void choice_shutdown();

// The hooks call these. Each returns true if the ORIGINAL should run.
//
// `cap` is the {vftable, WorldEvent*, entry*} capture block sub_140937F30 takes.
bool choice_on_event_commit(void* cap);
// `screen` / `opt` are select_option's two arguments.
bool choice_on_level_select(void* screen, void* opt);

// Per-frame ticks. Both cache the screen pointer and apply a held choice.
void choice_on_event_update(void* world_event);
void choice_on_level_update(void* level_screen);

void choice_on_message(const ChoiceMsg& m);

// This peer has ENTERED the node with this seed. Called by mgmp_follow from
// remember_node, on both peers, whenever the node actually changes.
//
// It does NOT drop held choices, and that restraint is load-bearing. A choice
// can legitimately arrive for a node this peer has not reached yet -- the host
// finishes node 17's battle and picks a level-up while the client is still
// walking into node 6 -- so "the seed is not the one I am standing in" says
// nothing about staleness. See apply_pending.
//
// What it does do is forget the cached WorldEvent*/LevelUpScreen*. Those
// screens belong to the node just left and may already be destroyed, and
// choice_on_message applies straight into whichever pointer is held.
void choice_on_node_entered(uint64_t node_seed);

// mgmp_follow has DISCARDED a node it was told about without entering it --
// queue overflow, or a validation failure (map size, index range, seed). That
// is the only way a node is passed over now that the pending queue exists, and
// it is therefore the only moment at which a held choice can be known stale.
//
// This is what stops a choice outliving its node, which is the fault that
// corrupted a run on 2026-08-25: node 6's 'int' was applied to node 3's event
// two nodes later, because nothing anywhere knew node 6 had been skipped. See
// ChoiceMsg::node_seed.
void choice_on_node_skipped(uint64_t node_seed);

} // namespace mgmp
