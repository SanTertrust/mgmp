#pragma once
// mgmp_combatlock -- the ability bar greys itself out on somebody else's cat.
//
// A QoL fix for a confusion the control split creates and never explained. Two
// players share one roster, so for half the human turns the combat menu on your
// screen belongs to a cat you do not control. It looked exactly like a menu you
// could use: full brightness, hover states, tooltips. Clicking did nothing --
// lockstep overwrites Brain::GetChoice unconditionally for a cat you do not own
// -- and "nothing" is indistinguishable from a dropped packet, a stall, or a
// bug. The information was already in the session; it just never reached a
// pixel.
//
// WHAT IT DOES. While the bar's subject is a human cat owned by a PEER, every
// button in it is forced into the state the game already uses for a spell you
// cannot afford. No new art, no new component, no overlay: the same greyed
// frame, the same dimming, from the same state machine.
//
// WHY IT NEEDS TWO HOOKS, which is the whole design and the only non-obvious
// part. Each frame CombatMenu::update walks its button vector and for every
// entry (a) recomputes the button's state, then (b) calls that button's own
// update, which is what turns the state into a SWF frame. Writing the state
// after CombatMenu::update returns is therefore always one tick late and always
// undone: the next tick's recompute lifts a disabled button back out with
// sub_1409767B0(button, true) before anything is drawn, so the bar renders "up"
// forever and the write is invisible. The only seam between the recompute and
// the draw is the entry to Button::update.
//
// So: CombatMenu::update marks the scope and decides ownership once; the
// Button::update hook, which fires for every button in the game, does its work
// only inside that scope. That scope flag is also what makes hooking a function
// this hot acceptable -- outside a combat menu tick the detour is one load and a
// branch, and it can never touch a button that is not on the bar.
//
// WHY THE SUBJECT AND NOT lockstep_local_actor(). The cursor fade asks "is the
// cat being polled right now mine", which is refreshed at a GetChoice poll --
// and GetChoice is only called when the brain has nothing cached, so between a
// decision and its animation the answer is simply the last one. Good enough for
// a cursor that is about to move anyway; wrong for a menu that stays on screen
// across that entire window and would flicker. CombatMenu+280 names the cat the
// bar is FOR, which is the question actually being asked.
//
// FAIL OPEN, EVERYWHERE. Every unknown -- no session, no snapshot, a stale
// Character ref, a cat that is not in the roster, an unreadable vector -- leaves
// the bar alone. A menu greyed out when it should not be reads as a broken game;
// a menu bright when it should be grey is only this feature not helping.
//
// NOT A LOCKSTEP CHANGE. Nothing here is hashed, sent, or able to reach a
// decision: it writes one int on a UI object after the game has already computed
// it, in the presentation half of the frame. The clicks it stops were already
// being discarded at Brain::GetChoice, so no command that used to exist stops
// existing. It is not a protocol change and it does not need a version bump.

#include <cstdint>

namespace mgmp {

// From the CombatMenu::update hook, around the original. `enter` decides
// ownership from the bar's subject and opens the scope; `leave` closes it and
// must run even if the original throws, hence the guard in the detour.
void combatlock_enter(void* combat_menu);
void combatlock_leave();

// From the Button::update hook, BEFORE the original. Forces the disabled state
// while the scope above is open and the subject belongs to a peer. Inert
// otherwise, which is every button in the game except this bar's.
void combatlock_on_button(void* button);

// For the panel: whether the bar is currently being held down, and how many
// buttons the last tick covered.
bool     combatlock_engaged();
uint32_t combatlock_buttons();

} // namespace mgmp
