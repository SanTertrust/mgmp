#include "mgmp_turnaction.h"

#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_mem.h"
#include "mgmp_rtti.h"

#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

const char* type_name(uint32_t t) {
    switch (t) {
    case TA_None:    return "none";
    case TA_Ability: return "ability";
    case TA_EndTurn: return "endturn";
    case TA_Invoke:  return "invoke";
    default:         return "?";
    }
}

} // namespace

void format_turn_action(char* out, size_t out_size, const void* ta, bool raw_bytes) {
    if (!out || out_size == 0) return;
    out[0] = 0;
    if (!ta) { strncpy_s(out, out_size, "ta=<none>", _TRUNCATE); return; }

    TurnAction a{};
    if (!mem_read(ta, &a, sizeof(a))) {
        strncpy_s(out, out_size, "ta=<unreadable>", _TRUNCATE);
        return;
    }

    int n = _snprintf_s(out, out_size, _TRUNCATE, "type=%u(%s)", a.type, type_name(a.type));
    if (n < 0) return;

    // A poll carries no payload; printing its zeroed fields is just noise.
    if (a.type != TA_None) {
        char abil[160], actor[160];
        rtti_class_name(a.ability, abil, sizeof(abil));
        rtti_class_name(a.actor, actor, sizeof(actor));
        int m = _snprintf_s(out + n, out_size - n, _TRUNCATE,
                            " abil=%s target=(%d,%d) dir=(%d,%d) actor=%s",
                            abil, a.target_x, a.target_y, a.dir_x, a.dir_y, actor);
        if (m > 0) n += m;
    }

    if (tune::kPointers && a.type != TA_None) {
        int m = _snprintf_s(out + n, out_size - n, _TRUNCATE,
                            " [abil=%p actor=%p]", a.ability, a.actor);
        if (m > 0) n += m;
    }

    if (raw_bytes && tune::kTaDump) {
        char hex[832];
        mem_hexdump(hex, sizeof(hex), ta, tune::kTaDump);
        _snprintf_s(out + n, out_size - n, _TRUNCATE, " raw=[%s]", hex);
    }
}

uint64_t turn_action_digest(const void* ta) {
    TurnAction a{};
    if (!ta || !mem_read(ta, &a, sizeof(a))) return 0;

    // FNV-1a over the meaningful fields only. +0x04 is uninitialized padding and
    // would otherwise make identical actions look different.
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    };
    mix(&a.type, sizeof(a.type));
    mix(&a.ability, sizeof(a.ability));
    mix(&a.target_x, sizeof(int32_t) * 4);
    mix(&a.actor, sizeof(a.actor));
    return h;
}

} // namespace mgmp
