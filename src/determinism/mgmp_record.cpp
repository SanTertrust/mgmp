#include "mgmp_record.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_rtti.h"

#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// 8 MB. Sized so a long battle never flushes mid-turn: at 32 bytes a draw that
// is ~260k draws of headroom, and the flush at each EV_TURN keeps it far from
// full in practice. Allocated once, up front -- a realloc in the draw path
// would be exactly the kind of timing perturbation this format exists to avoid.
const size_t kBufCap = 8u << 20;

CRITICAL_SECTION g_cs;
bool      g_cs_ready = false;
HANDLE    g_file     = INVALID_HANDLE_VALUE;
uint8_t*  g_buf      = nullptr;
size_t    g_used     = 0;
uint32_t  g_seq      = 0;
volatile LONG64 g_skipped = 0;

// The clock. `g_qpc_origin` is sampled once in record_init so every record
// carries ticks-since-start rather than a raw counter -- two captures are then
// directly comparable without subtracting a per-run origin. `g_qpc_freq` goes
// into EV_META because nothing else in the format can convert ticks to
// seconds; QPC's frequency is fixed for the life of the process but is not a
// constant across machines.
LARGE_INTEGER g_qpc_origin = {};
LARGE_INTEGER g_qpc_freq   = {};

// Ticks since record_init. Cheap on purpose: with an invariant TSC this does
// not enter the kernel, which is what makes it safe to call once per record in
// a path that also fires on every RNG draw. See the clock note in the header --
// perturbing frame pacing here would corrupt the one measurement run D needs.
inline uint64_t qpc_now() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (uint64_t)(li.QuadPart - g_qpc_origin.QuadPart);
}

// vptr -> class id. Open addressing, never resized: 1024 slots against ~1390
// component classes total, of which a single battle touches a small fraction.
// If it ever does fill, ids stop being minted and EvAction reports 0 rather
// than blocking the game -- a degraded record beats a stall.
struct ClassSlot { const void* vptr; uint32_t id; };
const size_t kClassSlots = 1024;
ClassSlot g_classes[kClassSlots];
uint32_t  g_next_class_id = 1;   // 0 means "none"

// Interned authored strings (ability GON names). Keyed by the string's own
// bytes, not by an address -- the whole point is that two runs mint the same id
// for the same name, which an address-keyed table could not promise.
const size_t kNameSlots = 512;
const size_t kNameMax   = 64;    // longest GON ability name we will store
struct NameSlot { uint32_t id; char text[kNameMax]; };
NameSlot g_names[kNameSlots];
uint32_t g_next_name_id = 1;     // 0 means "none"

// Actions whose ability was in none of the actor's slots. Expected to stay 0.
volatile LONG g_unknown_slots = 0;

struct Guard {
    Guard()  { EnterCriticalSection(&g_cs); }
    ~Guard() { LeaveCriticalSection(&g_cs); }
};

uintptr_t g_image_lo = 0, g_image_hi = 0;

// Where does this RNG state live? Stack vs heap is the whole question: a state
// in the caller's frame is a per-call temporary and cannot carry anything
// between calls, while a state in a heap allocation lives inside a game object
// and persists -- which makes it real sim state that lockstep must keep in step.
//
// Stack bounds come from the TEB (gs:[0x08] = StackBase, gs:[0x10] = StackLimit)
// and are per-thread, so this must be evaluated on the drawing thread.
uint8_t classify_pointer(const void* p, uint32_t* tls_offset) {
    if (tls_offset) *tls_offset = 0;
    uintptr_t a = (uintptr_t)p;
    if (!a) return STREAM_OTHER;

    // TLS first. A dynamic TLS block is a heap allocation, so checking heap
    // before TLS is what made run C report the battle stream as "HEAP" and hide
    // that it was simply a different offset in the same block.
    char** tls = (char**)__readgsqword(0x58);
    if (tls && tls[0]) {
        uintptr_t base = (uintptr_t)tls[0];
        if (a >= base && a < base + kTlsBlockSpan) {
            uint32_t off = (uint32_t)(a - base);
            if (tls_offset) *tls_offset = off;
            return off == 0x178 ? STREAM_GLOBAL : STREAM_TLS;
        }
    }

    uintptr_t stack_base  = (uintptr_t)__readgsqword(0x08);
    uintptr_t stack_limit = (uintptr_t)__readgsqword(0x10);
    if (a >= stack_limit && a < stack_base) return STREAM_STACK;

    if (g_image_lo && a >= g_image_lo && a < g_image_hi) return STREAM_IMAGE;

    return STREAM_HEAP;
}

void write_all(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    while (n) {
        DWORD wrote = 0;
        DWORD chunk = (DWORD)(n > 0x10000000u ? 0x10000000u : n);
        if (!WriteFile(g_file, p, chunk, &wrote, nullptr) || wrote == 0) return;
        p += wrote;
        n -= wrote;
    }
}

// Caller holds the lock.
void flush_locked() {
    if (g_used && g_file != INVALID_HANDLE_VALUE) write_all(g_buf, g_used);
    g_used = 0;
}

// Caller holds the lock. Appends one header + payload.
void emit_locked(uint8_t kind, uint8_t flags, const void* payload, uint16_t len) {
    if (!g_buf) return;
    size_t need = sizeof(EvHead) + len;
    if (g_used + need > kBufCap) flush_locked();
    if (need > kBufCap) return;                 // impossible for our records

    EvHead h;
    h.seq   = g_seq++;
    h.kind  = kind;
    h.flags = flags;
    h.len   = len;
    h.qpc   = qpc_now();
    memcpy(g_buf + g_used, &h, sizeof(h));
    g_used += sizeof(h);
    if (len) {
        memcpy(g_buf + g_used, payload, len);
        g_used += len;
    }
}

// Thread index, so EV_RNG can carry a 4-bit thread tag instead of a full tid.
// The sim is expected to be single-threaded; the tag exists to *prove* that
// rather than assume it, and to make a stray draw from an audio or loader
// thread obvious in the diff instead of silently interleaved.
const size_t kMaxThreads = 15;
DWORD    g_tids[kMaxThreads] = {};
uint32_t g_tid_count = 0;

// Caller holds the lock.
uint32_t thread_index_locked() {
    DWORD tid = GetCurrentThreadId();
    for (uint32_t i = 0; i < g_tid_count; ++i)
        if (g_tids[i] == tid) return i;
    if (g_tid_count >= kMaxThreads) return kMaxThreads;   // "other"
    uint32_t idx = g_tid_count++;
    g_tids[idx] = tid;
    EvThread t{ (uint32_t)tid, idx };
    emit_locked(EV_THREAD, 0, &t, sizeof(t));
    return idx;
}

} // namespace

void record_set_image(uintptr_t base, uint32_t size) {
    g_image_lo = base;
    g_image_hi = base + size;
}

void record_init(const wchar_t* path, uint32_t image_size, const char* note) {
    if (!g_cs_ready) {
        InitializeCriticalSection(&g_cs);
        g_cs_ready = true;
    }
    if (!path || !path[0]) return;

    g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) {
        log_raw("[!] record: cannot open %S (err %lu)", path, GetLastError());
        return;
    }
    g_buf = (uint8_t*)VirtualAlloc(nullptr, kBufCap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_buf) {
        log_raw("[!] record: cannot commit %zu byte buffer", kBufCap);
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
        return;
    }

    // EV_META: magic, version, and a free-text note describing the run. The
    // note is what tells Run A from Run C six weeks later.
    char meta[512];
    int  n = _snprintf_s(meta, sizeof(meta), _TRUNCATE,
                         "magic=%08X ver=%u image=%08X pid=%lu note=%s",
                         kRecordMagic, kRecordVersion, image_size,
                         GetCurrentProcessId(), note ? note : "");
    if (n < 0) n = 0;

    // Start the clock last, so t=0 is "recording began" and not "the file was
    // opened" -- CreateFileW can block for milliseconds and that time belongs
    // to no frame. Sampled before the first emit_locked so EV_META itself lands
    // at ~0 rather than at some arbitrary offset.
    QueryPerformanceFrequency(&g_qpc_freq);
    QueryPerformanceCounter(&g_qpc_origin);

    Guard g;
    struct {
        uint32_t magic, version, image_size, _pad;
        uint64_t qpc_freq;      // ticks per second, for EvHead::qpc
    } head{ kRecordMagic, kRecordVersion, image_size, 0,
            (uint64_t)g_qpc_freq.QuadPart };
    // Payload is the fixed head followed by the NUL-terminated note.
    uint8_t buf[600];
    memcpy(buf, &head, sizeof(head));
    size_t note_len = (size_t)n + 1;
    if (sizeof(head) + note_len > sizeof(buf)) note_len = sizeof(buf) - sizeof(head);
    memcpy(buf + sizeof(head), meta, note_len);
    buf[sizeof(head) + note_len - 1] = 0;
    emit_locked(EV_META, 0, buf, (uint16_t)(sizeof(head) + note_len));

    log_raw("record : %S (%zu MB buffer)", path, kBufCap >> 20);
}

void record_shutdown() {
    if (!g_cs_ready) return;
    {
        Guard g;
        flush_locked();
        if (g_file != INVALID_HANDLE_VALUE) {
            CloseHandle(g_file);
            g_file = INVALID_HANDLE_VALUE;
        }
        if (g_buf) {
            VirtualFree(g_buf, 0, MEM_RELEASE);
            g_buf = nullptr;
        }
    }
}

bool record_active() { return g_buf != nullptr; }

uint32_t record_intern_class(const void* obj) {
    if (!obj || !g_buf) return 0;

    const void* vptr = nullptr;
    if (!mem_read(obj, &vptr, sizeof(vptr)) || !vptr) return 0;

    // Hash the vptr. Low bits of a vtable address are not well distributed
    // (tables are adjacent and 8-byte aligned), so mix before masking.
    uint64_t h = (uint64_t)vptr;
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 29;
    size_t i = (size_t)(h & (kClassSlots - 1));

    Guard g;
    for (size_t probe = 0; probe < kClassSlots; ++probe) {
        ClassSlot& s = g_classes[i];
        if (s.vptr == vptr) return s.id;
        if (s.vptr == nullptr) {
            char name[192];
            rtti_class_name(obj, name, sizeof(name));
            uint32_t id = g_next_class_id++;
            s.vptr = vptr;
            s.id   = id;

            uint8_t buf[sizeof(EvClass) + sizeof(name)];
            EvClass c{ id };
            memcpy(buf, &c, sizeof(c));
            size_t nl = strlen(name) + 1;
            memcpy(buf + sizeof(c), name, nl);
            emit_locked(EV_CLASS, 0, buf, (uint16_t)(sizeof(c) + nl));
            return id;
        }
        i = (i + 1) & (kClassSlots - 1);
    }
    return 0;   // table full; degrade rather than stall
}

uint32_t record_intern_name(const char* str) {
    if (!str || !str[0] || !g_buf) return 0;

    size_t len = strnlen(str, kNameMax);
    if (len >= kNameMax) return 0;   // implausible for a GON name; do not store

    // FNV-1a over the bytes: same string -> same slot -> same id in every run.
    uint64_t h = 1469598103934665603ULL;
    for (size_t k = 0; k < len; ++k) { h ^= (uint8_t)str[k]; h *= 1099511628211ULL; }
    size_t i = (size_t)(h & (kNameSlots - 1));

    Guard g;
    for (size_t probe = 0; probe < kNameSlots; ++probe) {
        NameSlot& sl = g_names[i];
        if (sl.id != 0 && strncmp(sl.text, str, kNameMax) == 0) return sl.id;
        if (sl.id == 0) {
            uint32_t id = g_next_name_id++;
            sl.id = id;
            memcpy(sl.text, str, len);
            sl.text[len] = 0;

            uint8_t buf[sizeof(EvClass) + kNameMax];
            EvClass c{ id };
            memcpy(buf, &c, sizeof(c));
            memcpy(buf + sizeof(c), str, len + 1);
            emit_locked(EV_NAME, 0, buf, (uint16_t)(sizeof(c) + len + 1));
            return id;
        }
        i = (i + 1) & (kNameSlots - 1);
    }
    return 0;
}

uint32_t record_unknown_slots()   { return (uint32_t)g_unknown_slots; }
void     record_note_unknown_slot() { InterlockedIncrement(&g_unknown_slots); }

// Distinct RNG state pointers. Deliberately small: the question this answers is
// "a handful of persistent streams, or thousands of per-call temporaries?", and
// overflowing the table answers it just as well as filling it would.
const size_t kStreamSlots = 256;
struct StreamSlot { const void* addr; uint32_t id; uint8_t cls; };
StreamSlot g_streams[kStreamSlots];
uint32_t   g_next_stream_id = 1;   // 0 is reserved for the TLS global stream
bool       g_stream_overflow = false;
volatile LONG64 g_cls_stack = 0, g_cls_heap = 0, g_cls_image = 0, g_cls_tls = 0;

uint32_t record_intern_stream(const void* state, uint8_t cls, uint32_t tls_offset) {
    if (!state || cls == STREAM_GLOBAL || !g_buf) return 0;

    switch (cls) {
    case STREAM_STACK: InterlockedIncrement64(&g_cls_stack); break;
    case STREAM_HEAP:  InterlockedIncrement64(&g_cls_heap);  break;
    case STREAM_IMAGE: InterlockedIncrement64(&g_cls_image); break;
    case STREAM_TLS:   InterlockedIncrement64(&g_cls_tls);   break;
    default: break;
    }

    // A stack address is a frame slot: it repeats constantly across unrelated
    // calls and carries no state between them, so interning it would fill the
    // table with noise and hide the pointers that actually matter.
    if (cls == STREAM_STACK) return 0;

    uint64_t h = (uint64_t)state;
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 29;
    size_t i = (size_t)(h & (kStreamSlots - 1));

    for (size_t probe = 0; probe < kStreamSlots; ++probe) {
        StreamSlot& s = g_streams[i];
        if (s.addr == state) return s.id;
        if (s.addr == nullptr) {
            uint32_t id = g_next_stream_id++;
            s.addr = state;
            s.id   = id;
            s.cls  = cls;
            EvStream ev{ id, cls, {0,0,0}, (uint64_t)state, tls_offset, 0 };
            emit_locked(EV_STREAM, 0, &ev, sizeof(ev));
            return id;
        }
        i = (i + 1) & (kStreamSlots - 1);
    }
    g_stream_overflow = true;
    return kStreamOverflowId;
}

void record_stream_stats(uint32_t* distinct, bool* overflowed,
                         uint64_t* stack, uint64_t* heap, uint64_t* image,
                         uint64_t* tls) {
    if (distinct)   *distinct   = g_next_stream_id - 1;
    if (overflowed) *overflowed = g_stream_overflow;
    if (stack) *stack = (uint64_t)InterlockedCompareExchange64(&g_cls_stack, 0, 0);
    if (heap)  *heap  = (uint64_t)InterlockedCompareExchange64(&g_cls_heap,  0, 0);
    if (image) *image = (uint64_t)InterlockedCompareExchange64(&g_cls_image, 0, 0);
    if (tls)   *tls   = (uint64_t)InterlockedCompareExchange64(&g_cls_tls,   0, 0);
}

void record_rng(uint8_t fn, uint32_t site, uint64_t s0, uint64_t result,
                bool global, const void* state) {
    if (!g_buf) return;

    uint32_t tls_off = 0;
    uint8_t  cls = classify_pointer(state, &tls_off);

    Guard g;
    uint32_t sid = (cls == STREAM_GLOBAL) ? 0u
                                          : record_intern_stream(state, cls, tls_off);
    EvRng r{ site, log_turn(), s0, result, sid, cls, {0,0,0} };
    uint8_t flags = (uint8_t)(fn & kRngFnMask);
    if (global) flags |= kRngGlobal;
    flags |= (uint8_t)(thread_index_locked() << kRngThreadShift);
    emit_locked(EV_RNG, flags, &r, sizeof(r));
}

void record_action(const EvAction& a) {
    if (!g_buf) return;
    Guard g;
    emit_locked(EV_ACTION, 0, &a, sizeof(a));
}

void record_queue(const EvQueue& q) {
    if (!g_buf) return;
    Guard g;
    emit_locked(EV_QUEUE, 0, &q, sizeof(q));
}

void record_frame(uint32_t frame) {
    if (!g_buf) return;
    EvFrame f{ frame, log_turn() };
    Guard g;
    emit_locked(EV_FRAME, 0, &f, sizeof(f));
}

void record_turn(uint32_t turn, const void* tc,
                 uint64_t draws_total, uint64_t draws_global) {
    if (!g_buf) return;
    EvTurn t{ turn, 0, (uint64_t)tc, draws_total, draws_global,
              (uint64_t)InterlockedCompareExchange64(&g_skipped, 0, 0) };
    Guard g;
    emit_locked(EV_TURN, 0, &t, sizeof(t));
    flush_locked();   // turn boundaries are the natural, timing-neutral flush point
}

void record_flush() {
    if (!g_buf) return;
    Guard g;
    flush_locked();
}

uint64_t record_skipped_draws() {
    return (uint64_t)InterlockedCompareExchange64(&g_skipped, 0, 0);
}

// Called from the RNG hooks for a draw that used a scratch state while
// rng_global_only is on.
void record_note_skipped() {
    InterlockedIncrement64(&g_skipped);
}

} // namespace mgmp
