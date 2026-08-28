// mgmp_mem.h -- SEH-guarded reads of game memory.
//
// Everything we log comes from pointers whose meaning is still a guess (the
// TurnAction layout, the `this` of a class we only know by RTTI). A wrong guess
// must produce a bad log line, never a crash in the game process.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

// Whether this thread is currently inside one of the guarded copies below.
//
// An access violation raised in there is the design working -- see the comment
// in the .cpp. The crash handler asks this so it does not dump a stack for,
// and spend its report budget on, a fault that is about to be caught two frames
// up. It counts them instead.
bool mem_guard_active();

// Returns false if any byte of [src, src+n) faulted.
bool mem_read(const void* src, void* dst, size_t n);

// Returns false if the destination faulted. Used only by the replayer, which
// overwrites Brain::GetChoice's hidden return buffer.
bool mem_write(void* dst, const void* src, size_t n);

// Read an MSVC std::string out of the game.
//
// Layout: union { char buf[16]; char* ptr; } at +0, size at +16, capacity at
// +24 -- capacity > 15 means the heap form. Confirmed independently as the
// 32-byte stride of SaveSelection's name vector and as the shape Button::Click
// reads its own name's length from (Button+504 text, Button+520 size).
//
// Always NUL-terminates on success. False if any part faulted or the size is
// implausible, in which case `out` is left empty -- a wrong offset must produce
// no name rather than a confident wrong one.
bool mem_read_std_string(const void* str, char* out, size_t out_size);

// "aa bb cc .." into out; writes "<unreadable>" if the region faults.
// Always NUL-terminates. Returns the number of bytes actually dumped.
size_t mem_hexdump(char* out, size_t out_size, const void* src, size_t n);

} // namespace mgmp
