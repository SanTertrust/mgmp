// Compiled without C++ objects in the guarded functions so MSVC accepts
// __try/__except alongside /EHsc.
#include "mgmp_mem.h"

#include <windows.h>
#include <cstring>
#include <cstdio>

namespace mgmp {

// A FAULT INSIDE THESE FUNCTIONS IS THE MECHANISM WORKING, NOT A CRASH.
//
// Reading a pointer whose meaning is a guess is the entire premise of this
// module: mem_read returns false and the caller reports "unreadable". But the
// access violation is a real first-chance exception on the way there, so the
// crash handler's vectored filter saw every one of them, wrote a sixteen-frame
// stack for it, and -- worse -- spent its four-record budget on them. A run
// that walked a scene list through a teardown filled the log with dumps of its
// own guarded reads and would then have had nothing left to say about a real
// fault. Measured 2026-08-28: four such dumps and the cap reached, in the
// window where the actual failure happened.
//
// So the guard advertises itself. Thread-local because two threads may be in
// here at once and one must not silence the other's genuine crash; a plain int
// because __try/__except may not share a function with anything that needs
// unwinding.
thread_local int t_guard_depth = 0;

bool mem_guard_active() { return t_guard_depth != 0; }

bool mem_read(const void* src, void* dst, size_t n) {
    if (!src || !dst || n == 0) return false;
    ++t_guard_depth;
    bool ok;
    __try {
        memcpy(dst, src, n);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    --t_guard_depth;
    return ok;
}

// The one place the mod writes into the game rather than reading it: the
// replayer overwrites Brain::GetChoice's return buffer. Guarded for the same
// reason mem_read is -- the buffer is a stack address handed to us by a
// function whose ABI we recovered rather than were told.
bool mem_write(void* dst, const void* src, size_t n) {
    if (!dst || !src || n == 0) return false;
    ++t_guard_depth;
    bool ok;
    __try {
        memcpy(dst, src, n);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    --t_guard_depth;
    return ok;
}

// See the header. Every field is read through mem_read, so this is safe on a
// pointer whose meaning is still a guess -- which is the usual case, since the
// only reason to read a std::string out of the game is to check what an offset
// actually holds.
bool mem_read_std_string(const void* str, char* out, size_t out_size) {
    if (!str || !out || out_size == 0) return false;
    out[0] = 0;
    uint64_t size = 0, cap = 0;
    if (!mem_read((const uint8_t*)str + 16, &size, sizeof(size))) return false;
    if (!mem_read((const uint8_t*)str + 24, &cap,  sizeof(cap)))  return false;
    if (size >= out_size || size > 4096) return false;
    const void* chars = str;
    if (cap > 15) {
        if (!mem_read(str, &chars, sizeof(chars)) || !chars) return false;
    }
    if (size && !mem_read(chars, out, (size_t)size)) { out[0] = 0; return false; }
    out[size] = 0;
    return true;
}

size_t mem_hexdump(char* out, size_t out_size, const void* src, size_t n) {
    if (!out || out_size == 0) return 0;
    out[0] = 0;

    // 3 chars per byte plus the terminator.
    size_t max_bytes = (out_size - 1) / 3;
    if (n > max_bytes) n = max_bytes;
    if (n == 0) return 0;

    static const size_t kChunk = 256;
    unsigned char buf[kChunk];
    if (n > kChunk) n = kChunk;

    if (!mem_read(src, buf, n)) {
        strncpy_s(out, out_size, "<unreadable>", _TRUNCATE);
        return 0;
    }

    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        static const char* hex = "0123456789ABCDEF";
        out[o++] = hex[buf[i] >> 4];
        out[o++] = hex[buf[i] & 0xF];
        out[o++] = (i + 1 == n) ? '\0' : ' ';
    }
    out[o ? o - 1 : 0] = 0;
    return n;
}

} // namespace mgmp
