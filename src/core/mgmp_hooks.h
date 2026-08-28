#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

// Checks SizeOfImage and every target prologue against the pinned build.
// Returns false (with a reason in err) if anything drifted -- callers must not
// install hooks in that case.
bool hooks_verify_module(uintptr_t base, char* err, size_t err_size);

// Returns the number of hooks successfully installed, or -1 on MinHook failure.
int  hooks_install();

// Installs any hook that config().hook now asks for and that is not already
// live, and returns how many went in. For the one caller that needs it: the
// panel's connect buttons can start a session in a process launched with
// role = off, where hooks_implied_by(false) skipped the whole co-op surface.
// Without this, such a session runs with no inventory serializer hooks, no
// facing freeze and no combat lock -- see config_set_role.
//
// Safe from the game thread: MinHook does not suspend the calling thread, and
// every target here is a function we are not currently inside (the call site is
// FrameBegin's detour, and FrameBegin is already live by then).
int  hooks_install_late();

void hooks_uninstall();

// Whether one target's detour is actually live -- resolved, enabled, and with a
// trampoline. Asked by a feature that is only SAFE while some other hook is
// running: mgmp_aim will not call the ability highlight unless the suppressor
// that neuters its apply_status half is installed. "The config asked for it" is
// not the same claim and is not good enough here.
bool hooks_is_live(int target);

} // namespace mgmp
