// Compiled without C++ objects in the guarded functions so MSVC accepts
// __try/__except alongside /EHsc.
#include "mgmp_mem.h"

#include <windows.h>
#include <cstring>
#include <cstdio>

namespace mgmp {

bool mem_read(const void* src, void* dst, size_t n) {
    if (!src || !dst || n == 0) return false;
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The one place the mod writes into the game rather than reading it: the
// replayer overwrites Brain::GetChoice's return buffer. Guarded for the same
// reason mem_read is -- the buffer is a stack address handed to us by a
// function whose ABI we recovered rather than were told.
bool mem_write(void* dst, const void* src, size_t n) {
    if (!dst || !src || n == 0) return false;
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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
