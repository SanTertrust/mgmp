// mgmp_gpak.cpp -- see mgmp_gpak.h for the format and why this reads the
// shipped archive instead of embedding a copy of the art.
#include "mgmp_gpak.h"

#include "mgmp_log.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mgmp {
namespace {

// A 5 GB archive means 64-bit offsets, even though every field in the index is
// 32-bit: the offsets are a running SUM of u32 sizes and overflow long before
// the end of the file.
constexpr uint64_t kMaxEntrySize = 64u * 1024u * 1024u;   // no cursor is bigger
constexpr uint32_t kMaxEntries   = 1u << 20;              // 19900 in this build
constexpr uint16_t kMaxNameLen   = 1024;

// resources.gpak sits beside the game executable, which is where GPak looks.
bool archive_path(char* out, size_t cap) {
    char exe[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;

    char* slash = strrchr(exe, '\\');
    if (!slash) return false;
    *(slash + 1) = 0;

    if (strlen(exe) + strlen("resources.gpak") + 1 > cap) return false;
    strcpy_s(out, cap, exe);
    strcat_s(out, cap, "resources.gpak");
    return true;
}

} // namespace

bool gpak_read(const char* name, uint8_t** out, uint32_t* out_size) {
    *out = nullptr;
    *out_size = 0;

    char path[MAX_PATH * 2] = {};
    if (!archive_path(path, sizeof(path))) return false;

    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        log_line("GPAK", "!! cannot open %s", path);
        return false;
    }

    bool ok = false;
    uint32_t count = 0;
    if (fread(&count, 4, 1, f) != 1 || count == 0 || count > kMaxEntries) {
        log_line("GPAK", "!! %s does not begin with a plausible entry count", path);
        fclose(f);
        return false;
    }

    // One pass over the index, keeping only the entry we came for. The whole
    // index is ~700 KB of names in this build; building a map of it to answer
    // one question would cost more than the read does.
    uint64_t offset = 0, want_off = 0;
    uint32_t want_size = 0;
    bool found = false;
    char entry[kMaxNameLen + 1];

    for (uint32_t i = 0; i < count; ++i) {
        uint16_t len = 0;
        if (fread(&len, 2, 1, f) != 1 || len > kMaxNameLen) break;
        if (fread(entry, 1, len, f) != len) break;
        entry[len] = 0;

        uint32_t size = 0;
        if (fread(&size, 4, 1, f) != 1) break;

        if (!found && strcmp(entry, name) == 0) {
            want_off  = offset;
            want_size = size;
            found     = true;
            // Deliberately NOT breaking: the data section begins after the
            // WHOLE index, so the walk has to finish before the base is known.
        }
        offset += size;
    }

    if (!found) {
        log_line("GPAK", "!! '%s' is not in the archive", name);
        fclose(f);
        return false;
    }
    if (want_size == 0 || want_size > kMaxEntrySize) {
        log_line("GPAK", "!! '%s' has an implausible size (%u bytes)", name, want_size);
        fclose(f);
        return false;
    }

    const long base = ftell(f);   // GPak stores this at GPak+0x304
    if (base < 0) { fclose(f); return false; }

    uint8_t* buf = (uint8_t*)malloc(want_size);
    if (!buf) { fclose(f); return false; }

    if (_fseeki64(f, (int64_t)base + (int64_t)want_off, SEEK_SET) == 0 &&
        fread(buf, 1, want_size, f) == want_size) {
        *out = buf;
        *out_size = want_size;
        ok = true;
    } else {
        free(buf);
        log_line("GPAK", "!! short read on '%s'", name);
    }

    fclose(f);
    return ok;
}

} // namespace mgmp
