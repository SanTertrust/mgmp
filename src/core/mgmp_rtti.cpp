#include "mgmp_rtti.h"
#include "mgmp_mem.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace mgmp {
namespace {

// MSVC x64 RTTI, as laid out in the image.
#pragma pack(push, 4)
struct CompleteObjectLocator {
    uint32_t signature;        // 1 on x64
    uint32_t offset;
    uint32_t cdOffset;
    uint32_t pTypeDescriptor;  // RVA
    uint32_t pClassDescriptor; // RVA
    uint32_t pSelf;            // RVA of this COL -- how we recover the imagebase
};
#pragma pack(pop)

// ".?AVMoveAbility@glaiel@@" -> "glaiel::MoveAbility"
//
// Templates ("?$") are left raw: unmangling them properly means dragging in
// dbghelp, which is not thread-safe and not worth it for a trace log.
bool decode_type_name(const char* mangled, char* out, size_t out_size) {
    if (!mangled || out_size < 2) return false;
    if (mangled[0] != '.' || mangled[1] != '?') return false;
    if (mangled[2] != 'A') return false;
    if (mangled[3] != 'V' && mangled[3] != 'U') return false;  // class / struct

    const char* p = mangled + 4;
    if (strstr(p, "?$")) {
        strncpy_s(out, out_size, mangled, _TRUNCATE);
        return true;
    }

    // Split on '@' into namespace-inner-first order, then emit reversed.
    const char* tok[16];
    size_t      len[16];
    int         count = 0;

    const char* start = p;
    for (const char* q = p; *q && count < 16; ++q) {
        if (*q == '@') {
            if (q == start) break;              // "@@" terminator
            tok[count] = start;
            len[count] = (size_t)(q - start);
            ++count;
            start = q + 1;
        }
    }
    if (count == 0) return false;

    size_t o = 0;
    for (int i = count - 1; i >= 0; --i) {
        if (o && o + 2 < out_size) { out[o++] = ':'; out[o++] = ':'; }
        size_t n = len[i];
        if (o + n >= out_size) n = out_size - o - 1;
        memcpy(out + o, tok[i], n);
        o += n;
        if (o + 1 >= out_size) break;
    }
    out[o] = 0;
    return true;
}

} // namespace

const char* rtti_class_name(const void* obj, char* buf, size_t buf_size) {
    if (!buf || buf_size < 2) return "?";
    buf[0] = '?';
    buf[1] = 0;
    if (!obj) return buf;

    void* vptr = nullptr;
    if (!mem_read(obj, &vptr, sizeof(vptr)) || !vptr) return buf;

    void* col_ptr = nullptr;
    if (!mem_read((const uint8_t*)vptr - sizeof(void*), &col_ptr, sizeof(col_ptr)) || !col_ptr)
        return buf;

    CompleteObjectLocator col{};
    if (!mem_read(col_ptr, &col, sizeof(col))) return buf;
    if (col.signature != 1) return buf;          // not the x64 layout
    if (col.pSelf == 0 || col.pTypeDescriptor == 0) return buf;

    uintptr_t image_base = (uintptr_t)col_ptr - col.pSelf;
    // TypeDescriptor: { void* pVFTable; void* spare; char name[]; }
    const char* mangled = (const char*)(image_base + col.pTypeDescriptor + 2 * sizeof(void*));

    char raw[256];
    if (!mem_read(mangled, raw, sizeof(raw))) return buf;
    raw[sizeof(raw) - 1] = 0;

    if (!decode_type_name(raw, buf, buf_size)) {
        strncpy_s(buf, buf_size, raw, _TRUNCATE);
    }
    return buf;
}

} // namespace mgmp
