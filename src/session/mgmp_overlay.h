// mgmp_overlay.h -- the peer pointer, drawn in SCREEN space with OpenGL.
//
// WHY THIS EXISTS SEPARATELY FROM mgmp_cursor.cpp
//
// mgmp_cursor draws through the game's immediate-mode UI, which places
// everything on the BOARD: a piece is anchored to a tile (or to a world-space
// Vec3D) and is depth-sorted against the scenery. That is exactly right for the
// reticle -- "which square is my partner over" is a board fact, and it stays
// correct however either player has panned their camera.
//
// It is exactly wrong for a mouse pointer. A pointer is a screen object: it
// moves continuously rather than snapping to a square, it is never occluded,
// and it belongs on top of the HUD. Drawing the game's own cursor art through
// the board path produced all three failures in turn -- it snapped to tiles, it
// was depth-sorted behind the front rows, and it rendered as a dark tile-sized
// smear instead of a pointer.
//
// So this module leaves the game's renderer alone and draws afterwards.
//
// WHY OPENGL AND NOT SDL_Renderer
//
// SDL_Renderer is not in play. ApplicationBase::RefreshWindow asks for a CORE
// 3.2 context (SDL_GL_SetAttribute(CONTEXT_MAJOR, 3), (CONTEXT_MINOR, 2),
// (CONTEXT_PROFILE_MASK, CORE)) and presents with SDL_GL_SwapWindow; nothing in
// the binary calls SDL_CreateRenderer. SDL's 2D renderer therefore does not
// exist in this process, and the drawing API that does is raw GL.
//
// Core profile means there is no fixed-function pipeline: no glBegin, no
// glVertex, no matrix stack. The overlay owns a shader program, a VAO and a
// VBO of its own, which is most of the code here.
//
// WHERE IT DRAWS
//
// In a hook on SDL_GL_SwapWindow, BEFORE the original. That is the last
// instruction before the frame is presented, so nothing the game draws can
// cover us and we cannot disturb a pass that is still in progress.
//
// WHAT CROSSES THE WIRE, AND THE ONE HONEST CAVEAT
//
// A NORMALISED WINDOW POSITION -- the mouse divided by the window size, so it
// is resolution-independent and lands in the same place on a 1080p and a 4K
// screen. It is NOT camera-independent: if the two players have panned to
// different parts of the map, a peer's pointer sits at the same place on your
// SCREEN, which is a different place on the BOARD.
//
// That is a real limitation and it is why the tile reticle stays. The two
// answer different questions and both are worth having:
//
//   reticle (mgmp_cursor)  which SQUARE the peer is over -- always correct
//   pointer (this file)    where the peer's MOUSE is     -- correct when the
//                          cameras agree, which is the normal case
//
// Sending a board position instead would make the pointer camera-correct, but
// it needs a continuous mouse->world unprojection that the game only exposes
// already rounded to a tile (StatusMenu+124), which is the very rounding this
// module exists to avoid.
#pragma once

#include <cstdint>

namespace mgmp {

// Resolves SDL_GetWindowSize and takes over SDL_GL_SwapWindow by writing the
// SDL_DYNAPI jump-table slot -- see kRva_SdlSwapSlot for why that is a pointer
// write rather than a hook. Builds no GL objects: there is no context to build
// them in until the first swap actually arrives.
void overlay_set_base(uintptr_t base);

void overlay_init();
void overlay_shutdown();

// From our SDL_GL_SwapWindow detour, BEFORE the previous implementation runs.
// `window` is the SDL_Window* the game is about to present.
//
// Does nothing at all when the overlay is off, the session is not Ready, or the
// GL objects cannot be built. Every GL state it touches is saved and restored,
// because the game's next frame inherits whatever we leave behind.
void overlay_on_swap(void* window);

// The local mouse position as a fraction of the window, and which cursor art
// the game is showing for it, both refreshed on every swap. mgmp_cursor reads this when it builds a CURSOR message; it lives here
// because the swap hook is the only place that has the window to divide by.
//
// Returns false before the first swap, or if the window size is unusable.
bool overlay_local_pointer(float& nx, float& ny, uint8_t& mode);

struct CursorMsg;

// From the lockstep pump, alongside cursor_on_message. The two consume the same
// message: mgmp_cursor takes the tile out of it and this takes the fraction.
void overlay_on_message(uint8_t from, const CursorMsg& c);

} // namespace mgmp
