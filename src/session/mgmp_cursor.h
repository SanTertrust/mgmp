#pragma once
// mgmp_cursor -- every player sees where the others are pointing.
//
// The first thing this mod DRAWS. Everything before it moved state between two
// processes; this puts a mark on the board, which means it needed an answer to
// a question the rest of the project never had to ask: how do you render
// something in a game whose entire UI is a hand-written Flash player?
//
// THE ANSWER WAS NOT "MAKE A COMPONENT". The obvious route is to clone what
// StatusMenu::init @ 0x140816F60 does for the local player -- it builds two
// RendererIso components (StatusMenu+80 from the animation
// "GroundMouseCursorPip", StatusMenu+88 from "MouseCursorPip3D") and update
// shows or hides them each frame. Cloning that means allocating through the
// game's component system, parenting into a scene, and owning a lifetime that
// has to survive a battle ending, a peer disconnecting and a scene teardown we
// do not control. Every one of those is a crash we would have to earn.
//
// IMMEDIATE MODE MAKES ALL OF THAT GO AWAY. The battle screen also runs a
// glaiel::ImmediateModeGameUI, and Brain::UpdateDecision draws the local
// target cursor on it with a single call -- id "TargetCursor", animation
// "target", layer 6, frame -1 -- while Brain::DrawAbilityAOE draws whole tile
// sets the same way ("AreaIndicator", "KnockbackArrow"). Submitting a piece is
// one call per frame:
//
//     tile_piece(ui, id, layer, anim, tile, rgba, frame, scale)
//
// and the identity is the STRING. The same id next frame is the same piece, so
// it persists and animates; stop submitting it and it disappears on its own.
// There is nothing to allocate, nothing to free, nothing to unhook when a peer
// leaves, and nothing that can outlive the battle. A peer that stops sending
// simply stops being drawn.
//
// THE HOOK IS StatusMenu::update, AND IT IS BOTH HALVES AT ONCE.
//
//   READ. StatusMenu caches the board tile under the mouse at +124 every frame
//   -- it has to, that is what positions its own pips -- after bounds-checking
//   it against the grid at +72 (width +184, height +188). So the mouse ->
//   isometric tile projection never has to be reimplemented, or even
//   understood. The game does the work and leaves the answer in a field.
//
//   DRAW. The same function already submits immediate-mode pieces, so adding
//   ours after the original runs puts them in the same frame, on the same UI,
//   at the same point in the pipeline as the cursor the local player sees.
//
// WHY A TILE AND NOT A PIXEL. Two peers run at different resolutions, with
// independently panned cameras and different UI scale. A screen-space cursor
// would point at a different square on every other machine, which is worse than
// no cursor at all -- it would be confidently wrong. A tile index means the same
// square everywhere, costs 10 bytes, and is exactly the granularity a tactics
// game is played at.
//
// TRANSPARENCY IS THE TURN INDICATOR, and it comes free. The colour argument is
// four floats and the consumer premultiplies rgb by a before writing a into
// Renderer+0x60 (a double that the base constructor sub_14005A580 sets to 1.0),
// so alpha is a plain parameter -- no shader, no blend state, no second draw.
// That makes one rule cover both cases the design needs:
//
//     the peer whose cat is deciding is opaque; everybody else is faded.
//
// For a remote peer that is the alpha in its colour. For the LOCAL player it is
// a write of the same value to Renderer+0x60 on the game's own two pips, which
// is safe to do every frame and reverts to 1.0 the moment cursors are switched
// off. Reading a board then tells you whose move it is without reading a name.
//
// COSMETIC, AND DELIBERATELY OUTSIDE THE LOCKSTEP CONTRACT. Nothing here is
// hashed, replayed or acknowledged. A CURSOR that is dropped, late or stale
// cannot desync anything -- the worst case is a cursor that lags or vanishes --
// which is why it is the one message allowed to be sent on a timer rather than
// at a command boundary.

#include <cstdint>

namespace mgmp {

struct CursorMsg;

// Verifies and resolves the two game functions this module CALLS
// (Component::im_game_ui and ImmediateModeGameUI::tile_piece). Failure is
// non-fatal and turns the overlay off: a cursor is a nicety, and a bad call
// address would run an unrelated function with our arguments on the game's
// stack. Called from hooks_install alongside lockstep_set_base.
void cursor_set_base(uintptr_t base);

void cursor_init();
void cursor_shutdown();

// From h_StatusMenuUpdate, AFTER the original has run. Reads this peer's
// hovered tile and sends it if it moved, draws every peer's cursor that is
// still fresh, and sets the alpha on the game's own two pips.
//
// Safe to call with anything: it validates the grid pointer and the tile before
// it touches either, and does nothing at all when the session is not Ready.
void cursor_on_status_menu(void* status_menu);

// From the lockstep pump. Records where one peer is pointing; the drawing
// happens on the next StatusMenu::update, not here.
void cursor_on_message(uint8_t from, const CursorMsg& c);

} // namespace mgmp
