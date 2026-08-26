#include "mgmp_rng.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_record.h"

#include <windows.h>
#include <intrin.h>
#include <cstring>

namespace mgmp {
namespace {

typedef int     (__fastcall* fn_randint)  (int    n, uint64_t* state);
typedef double  (__fastcall* fn_randfloat)(double m, uint64_t* state);
typedef __m128  (__fastcall* fn_rand2)    (double m, uint64_t* state);
typedef bool    (__fastcall* fn_rollchance)(double p, double scale, uint64_t* state);

fn_randint   o_randint   = nullptr;
fn_randfloat o_randfloat = nullptr;
fn_rand2     o_rand2     = nullptr;
fn_rollchance o_rollchance = nullptr;

uintptr_t g_base = 0;

volatile LONG64 g_total  = 0;
volatile LONG64 g_global = 0;

// Return address -> RVA. Absolute addresses would differ between runs under
// ASLR and make every record differ; the RVA is stable for a pinned build.
inline uint32_t site_rva(void* ret) {
    uintptr_t a = (uintptr_t)ret;
    uintptr_t b = g_base;
    if (!b || a < b) return 0;
    uintptr_t d = a - b;
    return d > 0xFFFFFFFFull ? 0 : (uint32_t)d;
}

// The common tail of all three detours: decide whether this draw matters,
// count it, and record it.
//
// Ordering note: `s0` must be sampled BEFORE the original runs, because the
// original mutates the state in place. That is the entire reason these detours
// cannot be written as a simple "call original, then log".
inline void note(uint8_t fn, void* ret, uint64_t s0, uint64_t result, bool global,
                 const void* state) {
    InterlockedIncrement64(&g_total);
    if (global) InterlockedIncrement64(&g_global);

    if (!record_active()) return;
    if (!global && tune::kRngGlobalOnly) {
        record_note_skipped();
        return;
    }
    record_rng(fn, site_rva(ret), s0, result, global, state);
}

} // namespace

uint64_t* rng_global_stream() {
    // gs:[0x58] is the TEB's ThreadLocalStoragePointer; slot 0 is the main
    // module's static TLS block, and the shared stream sits at +0x178 in it.
    char** tls = (char**)__readgsqword(0x58);
    if (!tls) return nullptr;
    char* block = tls[0];
    if (!block) return nullptr;
    return (uint64_t*)(block + 0x178);
}

// ---- detours --------------------------------------------------------------
//
// Each one reads state[0] first, forwards, then records. `state` is supplied by
// the caller and is always a live 32-byte array, so the read needs no SEH
// guard -- if it were bad the game would already have crashed inside the
// original on the very next instruction.

int __fastcall h_randint(int n, uint64_t* state) {
    void*    ret = _ReturnAddress();
    bool     glb = (state == rng_global_stream());
    uint64_t s0  = state ? state[0] : 0;
    int      r   = o_randint(n, state);
    note(RNG_INT, ret, s0, (uint64_t)(uint32_t)r, glb, state);
    return r;
}

double __fastcall h_randfloat(double m, uint64_t* state) {
    void*    ret = _ReturnAddress();
    bool     glb = (state == rng_global_stream());
    uint64_t s0  = state ? state[0] : 0;
    double   r   = o_randfloat(m, state);
    uint64_t bits;
    memcpy(&bits, &r, sizeof(bits));
    note(RNG_FLOAT, ret, s0, bits, glb, state);
    return r;
}

__m128 __fastcall h_rand2(double m, uint64_t* state) {
    void*    ret = _ReturnAddress();
    bool     glb = (state == rng_global_stream());
    uint64_t s0  = state ? state[0] : 0;
    __m128   r   = o_rand2(m, state);
    uint64_t bits;
    memcpy(&bits, &r, sizeof(bits));       // low half is enough as a checksum
    note(RNG_TWO, ret, s0, bits, glb, state);
    return r;
}

// RollChance(p, scale, state) -- the proc/crit gate.
//
// Recorded even though the draw it makes is ALSO recorded by the rand2 hook
// underneath it. That is the point: this frame is the only one that knows which
// passive rolled, because rand2's return address lands inside RollChance and
// RollChance's lands in the caller. The decoder can pair them by sequence.
//
// When p >= 1.0 it short-circuits and takes no draw at all. That case is still
// recorded (result = 1, s0 unchanged) because "a proc that was guaranteed"
// still has to match between peers -- a peer that thought it was 0.9 would take
// a draw here and desync the stream position, not just the outcome.
bool __fastcall h_rollchance(double p, double scale, uint64_t* state) {
    void*    ret = _ReturnAddress();
    bool     glb = (state == rng_global_stream());
    uint64_t s0  = state ? state[0] : 0;
    bool     r   = o_rollchance(p, scale, state);
    note(RNG_ROLL, ret, s0, (uint64_t)r, glb, state);
    return r;
}

void* rng_detour_randint()   { return (void*)&h_randint;   }
void* rng_detour_randfloat() { return (void*)&h_randfloat; }
void* rng_detour_rand2()     { return (void*)&h_rand2;     }
void* rng_detour_rollchance(){ return (void*)&h_rollchance;}

void** rng_original_randint()   { return (void**)&o_randint;   }
void** rng_original_randfloat() { return (void**)&o_randfloat; }
void** rng_original_rand2()     { return (void**)&o_rand2;     }
void** rng_original_rollchance(){ return (void**)&o_rollchance;}

void rng_counters(uint64_t* total, uint64_t* global) {
    if (total)  *total  = (uint64_t)InterlockedCompareExchange64(&g_total, 0, 0);
    if (global) *global = (uint64_t)InterlockedCompareExchange64(&g_global, 0, 0);
}

void rng_set_base(uintptr_t base) { g_base = base; }

} // namespace mgmp
