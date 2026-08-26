// mgmp_aim -- show the peer's aim: the range and AOE tiles the other player is
// looking at right now, before they commit anything.
//
// WHY THIS COSTS ALMOST NO NEW CODE. The game already draws exactly this, from
// the PlayerBrain's own update, sub_140775EB0 -- `this` in r14:
//
//     mov  dword ptr [rsp+...], 13h    ; the layer
//     mov  r9,  [r14+360h]             ; iVec2D direction
//     mov  r8,  [r14+358h]             ; iVec2D target
//     mov  rdx, [r14+3D8h]
//     mov  rcx, r14
//     call glaiel::Brain::DrawAbilityAOE   ; 0x14013A030
//
// So the whole feature is: read those three fields on the peer that owns the
// cat, put them on the wire, and make that call on the peer that does not. No
// new art, no component, no overlay, no coordinate maths -- the tiles are drawn
// by the same code, at the same layer, in the same frame slot, so a remote aim
// looks exactly like a local one because it IS one.
//
// THE SOURCE, AND THAT IS THE CORRECTION. Version 21 read the fields off the
// CACHED DECISION (Brain+0x220..238, which is what Brain::UpdateDecision draws
// from). That state exists for the sliver between a click and the action being
// applied, not for the seconds a player spends choosing, and it showed: the
// host published 121 aims and the client drew zero tiles. Version 22 reads the
// selection above instead.
//
// THE RANGE TILES, AND THE ONE THAT COST A RUN. Version 22 also mirrored the
// reachable set by calling sub_140138A10, on the reasoning that it sits one
// instruction above each of the game's own DrawAbilityAOE sites and must be the
// other half of the preview. It is not a draw: it applies STATUSES, and
// mirroring it mutated the non-owning peer's simulation thousands of times a
// battle. Version 23 removed it; version 24 brought it back with
// T_HighlightRefresh swallowing sub_140151CE0 for the duration and the whole
// roster fenced across the call. The full reason is at the bottom of the kCalls
// table in mgmp_addresses.h and must be read before touching that call.
//
// THE ATTACK RANGE UNDER A MOVE (version 25). Selecting Move shows a second
// thing the highlight does not produce: where the cat could attack from the
// square under the mouse. The game gets it by DISPLACING THE CAT to that square,
// asking the attack slot for its range, and moving it back -- so mirroring it
// means making that round trip on this peer too. It is bracketed by the same
// state fence, and a trip that fails to come home turns the feature off for the
// session rather than leaving a cat somewhere it never walked to.
//
// WHAT IT IS NOT. An AIM is not a decision and must never be treated as one.
// The same aim is sent dozens of times and then abandoned when the player
// changes their mind; the decision, when it comes, still arrives as an ACTION
// through the lockstep path and nothing about that changes. Nothing here is
// hashed, replayed or acknowledged -- this module could be deleted mid-battle
// and the simulation would not notice. That is the same contract mgmp_cursor
// has, and it is why AIM is allowed to be sent on a timer.
//
// THE ONE THING THAT NEEDED CARE. DrawAbilityAOE is a call INTO the game on a
// path the other peer is not taking, so it must not be able to move the
// simulation. The reverse-reachability pass already done for the fence found
// exactly one RNG site under it -- Ability::ResolveKnockbackDirection case 19,
// `knockback_mode random` -- and exactly one shipped ability uses that mode
// (MegaGuppy_DropTrash, target_mode none, in no pool, unselectable). That is
// good evidence and it is still only evidence, so this module does not rely on
// it: it saves the 32 bytes of TLS+0x178 before the call and restores them
// after. Whatever the draw does to the simulation stream is undone, measurably,
// rather than argued away.
#pragma once

#include <cstdint>

#include "mgmp_proto.h"

namespace mgmp {

void aim_set_base(uintptr_t base);
void aim_init();
void aim_shutdown();

// Called from h_UpdateDecision with the Brain the game is about to tick.
//
// Both halves live in one call because they are two branches of one question:
// does this peer own the cat whose turn it is? If it does, publish what the
// player is aiming (throttled, on change). If it does not, draw whatever the
// owner last told us. A cat nobody is aiming draws nothing, and an AI cat is
// neither published nor drawn.
void aim_on_update_decision(void* brain);

// True only while this peer is inside its own call to the ability highlight,
// drawing ANOTHER player's aim on a cat it does not own. h_HighlightRefresh
// swallows sub_140151CE0 -- the apply_status half -- for exactly that window.
//
// Read on every call of a function the game itself calls constantly, so it is a
// plain global load and nothing more. It is set and cleared around one call,
// on the game's own thread, so there is no window for it to leak.
bool aim_highlight_suppressed();

// Called once, after hooks_install has decided what is actually live.
//
// The ability highlight may only be called while T_HighlightRefresh is running
// to swallow its apply_status half. If that hook did not install, the tiles are
// dropped and the aim is drawn without them -- the ONE thing that must never
// happen is calling the highlight unguarded, which is what cost a run.
void aim_on_hooks_installed();

// A held aim belongs to one battle and one cat. Both are dropped when the
// battle changes, so a stale preview cannot survive into the next fight.
void aim_on_message(const AimMsg& m);

uint32_t aim_sent();
uint32_t aim_drawn();

} // namespace mgmp
