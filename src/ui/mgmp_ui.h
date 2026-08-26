// mgmp_ui.h -- the ImGui debug panel. Layer 5, diagnostics.
//
// WHY THIS IS A SECOND OVERLAY AND NOT PART OF mgmp_overlay.cpp
//
// mgmp_overlay draws the PEER's pointer. It is part of the session: it has
// nothing to show until a peer connects, it is gated on net_cursors, and every
// pixel it draws describes somebody else.
//
// This draws for the person at this keyboard, and its most useful moment is
// when there is no session at all -- the connect controls are on it. Sharing a
// module would mean one `on` flag standing for two unrelated questions, and the
// first bug would be the panel disappearing because the peer cursors were
// turned off.
//
// It does share the DRAW SITE, because there is only one: the takeover of
// SDL_GL_SwapWindow's SDL_DYNAPI slot that mgmp_overlay installs (see trap 4 in
// CLAUDE.md for why that is a jump-table write and not a hook).
// overlay_swap_detour calls us AFTER the peer pointers, so the panel sits on
// top of them, and both run before the previous implementation presents.
//
// WHY THE WIN32 BACKEND AND NOT THE SDL3 ONE
//
// imgui_impl_sdl3 calls forty-odd SDL_* functions directly. Every SDL symbol in
// this process is a thunk through the SDL_DYNAPI table, and the address behind
// a thunk statically is the DEFAULT stub -- the one SDL_InitDynamicAPI
// replaces, i.e. the single address guaranteed not to be the function. Each of
// those forty would need its slot RVA decoded from its own `jmp cs:off_...`
// operand, and getting one wrong lands on a NEIGHBOURING function whose stub
// shares the same prologue, so a pinned-build check passes and an unrelated
// function runs with our arguments. That trap has already cost this project one
// std::bad_alloc.
//
// imgui_impl_win32 needs one HWND and a WndProc subclass. Nothing to decode.
//
// WHAT IT MAY AND MAY NOT DO
//
// May: show. The panel is a view onto state the mod already computes, and it is
// as safe as the log file it mirrors -- one peer may have it open and the other
// closed with no consequence, exactly like the peer cursors.
//
// May not: change anything the SIMULATION reads. A runtime toggle for
// hook_timedelay, replay_brains or a fence would make the debug UI a desync
// source, and a desync whose cause is "someone clicked a checkbox forty minutes
// ago" is the worst kind to diagnose. Diagnostic-only switches (traces, whether
// a mismatch halts) are fair game; anything a brain or an RNG draw can observe
// is not.
#pragma once

#include <cstdint>

namespace mgmp {

// Reads the ui* keys and installs the WndProc subclass. Builds no ImGui state:
// the GL objects and the font atlas need a current context, and there is none
// until the first swap arrives.
void ui_init();

void ui_shutdown();

// From overlay_swap_detour, AFTER overlay_on_swap and before the previous
// SDL_GL_SwapWindow. Lazily creates the ImGui context on the first call, which
// is the first moment a GL context is guaranteed current.
//
// Does nothing when ui = 0, when the panel is hidden, or when init failed.
void ui_on_swap(void* window);

// Whether the panel currently owns the mouse or the keyboard. The WndProc uses
// it to decide whether to pass a message on to the game; exposed because a
// click that lands on a button must not also land on the board behind it.
bool ui_captures_input();

} // namespace mgmp
