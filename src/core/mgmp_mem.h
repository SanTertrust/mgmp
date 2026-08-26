// mgmp_mem.h -- SEH-guarded reads of game memory.
//
// Everything we log comes from pointers whose meaning is still a guess (the
// TurnAction layout, the `this` of a class we only know by RTTI). A wrong guess
// must produce a bad log line, never a crash in the game process.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

// Returns false if any byte of [src, src+n) faulted.
bool mem_read(const void* src, void* dst, size_t n);

// Returns false if the destination faulted. Used only by the replayer, which
// overwrites Brain::GetChoice's hidden return buffer.
bool mem_write(void* dst, const void* src, size_t n);

// "aa bb cc .." into out; writes "<unreadable>" if the region faults.
// Always NUL-terminates. Returns the number of bytes actually dumped.
size_t mem_hexdump(char* out, size_t out_size, const void* src, size_t n);

} // namespace mgmp
