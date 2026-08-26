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
void hooks_uninstall();

// Whether one target's detour is actually live -- resolved, enabled, and with a
// trampoline. Asked by a feature that is only SAFE while some other hook is
// running: mgmp_aim will not call the ability highlight unless the suppressor
// that neuters its apply_status half is installed. "The config asked for it" is
// not the same claim and is not good enough here.
bool hooks_is_live(int target);

} // namespace mgmp
