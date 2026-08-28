// mgmp_crash.cpp -- see mgmp_crash.h.
#include "mgmp_crash.h"

#include "mgmp_log.h"
#include "mgmp_mem.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

constexpr DWORD kCppThrow = 0xE06D7363;   // 'msc' -- the MSVC C++ throw

constexpr int kRingSize   = 24;
constexpr int kMaxFrames  = 16;

struct Record {
    DWORD     code = 0;
    void*     addr = nullptr;
    char      type[96] = {};              // the C++ type name, when we get one
    void*     frames[kMaxFrames] = {};
    USHORT    frame_count = 0;
};

struct State {
    void*     handle = nullptr;
    uintptr_t game_base = 0;
    HMODULE   self = nullptr;
    uintptr_t self_base = 0;

    Record    ring[kRingSize];
    LONG      ring_next = 0;              // monotonic; index is % kRingSize
    LONG      fatal_reported = 0;
    LONG      throws_logged  = 0;
    LONG      guarded_faults = 0;         // caught by mem_read/mem_write
} g;

// How many C++ throws to write to the log AS THEY HAPPEN, rather than only
// ringing them for the unhandled filter.
//
// The filter is not guaranteed a turn. An unhandled C++ exception goes through
// std::terminate -> abort(), which in several CRT configurations never reaches
// SetUnhandledExceptionFilter -- so a ring that is only flushed there can be
// lost exactly when it is needed. Writing the first few immediately means the
// evidence is on disk before anything gets a chance to skip the flush.
//
// Bounded because a game that throws in a loop would otherwise fill the disk,
// and because the interesting throw is near the start of the failure, not
// after ten thousand handled ones.
constexpr LONG kThrowsToLog = 40;

// --- the MSVC throw record --------------------------------------------------
//
// All the pointers inside are 32-bit RVAs relative to the module that threw,
// which x64 passes as ExceptionInformation[3]. Nothing here is documented, but
// it is stable across every MSVC that emits __CxxFrameHandler3/4, and every
// step is bounds-checked because this runs while the process is already sick.
#pragma pack(push, 4)
struct ThrowInfoX      { uint32_t attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray; };
struct CatchableArrayX { int32_t  count; uint32_t types[1]; };
struct CatchableTypeX  { uint32_t properties, pType; };
#pragma pack(pop)

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    // Not a full range check across regions, but enough to stop the common
    // "the pointer is garbage" case from turning a diagnostic into a crash.
    uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return (uintptr_t)p + n <= end;
}

// Undecorated-ish: the TypeDescriptor name is a mangled ".?AVfoo@bar@@". The
// leading ".?AV"/".?AU" is stripped and the rest left alone -- enough to read,
// and it avoids dragging in UnDecorateSymbolName from a crashing thread.
void type_name_of(const EXCEPTION_RECORD* er, char* out, size_t out_n) {
    out[0] = 0;
    if (er->NumberParameters < 4) return;

    const ThrowInfoX* ti = (const ThrowInfoX*)er->ExceptionInformation[2];
    uintptr_t         mb = (uintptr_t)er->ExceptionInformation[3];
    if (!ti || !mb || !readable(ti, sizeof(*ti)) || !ti->pCatchableTypeArray) return;

    const CatchableArrayX* arr = (const CatchableArrayX*)(mb + ti->pCatchableTypeArray);
    if (!readable(arr, sizeof(*arr)) || arr->count < 1) return;

    const CatchableTypeX* ct = (const CatchableTypeX*)(mb + arr->types[0]);
    if (!readable(ct, sizeof(*ct)) || !ct->pType) return;

    // TypeDescriptor: { void* vftable; void* spare; char name[]; }
    const char* name = (const char*)(mb + ct->pType) + 16;
    if (!readable(name, 8)) return;

    if (name[0] == '.' && name[1] == '?' && name[2] == 'A' &&
        (name[3] == 'V' || name[3] == 'U'))
        name += 4;

    size_t i = 0;
    for (; i + 1 < out_n && name[i]; ++i) out[i] = name[i];
    out[i] = 0;
}

// --- symbolising ------------------------------------------------------------

// "<module>+<rva>", which is directly comparable with an address in the IDB
// once the game's imagebase (0x140000000) is added back.
void describe(void* addr, char* out, size_t out_n) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) && mod) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t* leaf = path;
        for (const wchar_t* p = path; *p; ++p) if (*p == L'\\' || *p == L'/') leaf = p + 1;

        char name[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, leaf, -1, name, sizeof(name) - 1, nullptr, nullptr);

        uintptr_t rva = (uintptr_t)addr - (uintptr_t)mod;
        // For the game itself also print the address the IDB uses, so a frame
        // can be pasted straight into a disassembler without arithmetic.
        if ((uintptr_t)mod == g.game_base)
            _snprintf_s(out, out_n, _TRUNCATE, "%s+%08llX (ida %012llX)",
                        name, (unsigned long long)rva,
                        (unsigned long long)(0x140000000ull + rva));
        else
            _snprintf_s(out, out_n, _TRUNCATE, "%s+%08llX", name,
                        (unsigned long long)rva);
        return;
    }
    _snprintf_s(out, out_n, _TRUNCATE, "%016llX <no module>", (unsigned long long)addr);
}

bool frame_is_ours(void* addr) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &mod)) return false;
    return mod != nullptr && (uintptr_t)mod == g.self_base;
}

void dump_record(const char* why, const Record& r) {
    char at[128];
    describe(r.addr, at, sizeof(at));
    if (r.type[0])
        log_line("CRASH", "%s code %08lX '%s' at %s", why, r.code, r.type, at);
    else
        log_line("CRASH", "%s code %08lX at %s", why, r.code, at);

    bool ours = false;
    for (USHORT i = 0; i < r.frame_count; ++i) {
        char f[128];
        describe(r.frames[i], f, sizeof(f));
        bool mine = frame_is_ours(r.frames[i]);
        if (mine) ours = true;
        log_line("CRASH", "    #%-2u %s%s", (unsigned)i, f, mine ? "   <-- mgmp" : "");
    }
    if (r.frame_count)
        log_line("CRASH", "  %s", ours
                 ? "mgmp.dll IS on this stack -- the mod is in the failing path"
                 : "no mgmp.dll frame on this stack");
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    if (!er) return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = er->ExceptionCode;

    // Debugger/misc noise that is never a fault.
    if (code == DBG_PRINTEXCEPTION_C || code == DBG_PRINTEXCEPTION_WIDE_C ||
        code == 0x406D1388 /* SetThreadName */)
        return EXCEPTION_CONTINUE_SEARCH;

    // A GUARDED READ THAT FAULTED IS NOT A CRASH, AND IT MUST NOT COST THE
    // BUDGET FOR ONE. mem_read exists to survive a pointer whose meaning is a
    // guess, so its access violations are expected traffic -- but they arrive
    // here first, and the four-record cap below was being spent on them. That
    // is the diagnostic failing in exactly the window it was built for: a run
    // that walked a scene list through a teardown wrote four dumps of its own
    // caught reads and then had nothing left to say. Counted, not dumped; the
    // total goes out at shutdown, so "this peer read a lot of dead memory"
    // stays visible without burying anything.
    if (code == EXCEPTION_ACCESS_VIOLATION && mem_guard_active()) {
        InterlockedIncrement(&g.guarded_faults);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    Record r;
    r.code = code;
    r.addr = er->ExceptionAddress;
    if (code == kCppThrow) type_name_of(er, r.type, sizeof(r.type));
    r.frame_count = RtlCaptureStackBackTrace(1, kMaxFrames, r.frames, nullptr);

    LONG slot = InterlockedIncrement(&g.ring_next) - 1;
    g.ring[slot % kRingSize] = r;

    // A C++ throw is first-chance noise until proven otherwise -- the game
    // throws and catches on purpose. Anything else at first chance is already
    // a fault, so it is worth saying immediately, in case the process dies
    // before the unhandled filter gets a turn.
    if (code != kCppThrow) {
        if (InterlockedIncrement(&g.fatal_reported) <= 4)
            dump_record("first-chance", r);
    } else {
        LONG n = InterlockedIncrement(&g.throws_logged);
        if (n <= kThrowsToLog) {
            dump_record("first-chance throw", r);
            if (n == kThrowsToLog)
                log_line("CRASH", "  (throw log capped at %ld -- later ones are still"
                                  " ringed and dumped if the process dies)", kThrowsToLog);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* ep) {
    log_line("CRASH", "==== unhandled exception -- the process is going down ====");

    if (ep && ep->ExceptionRecord) {
        Record r;
        r.code = ep->ExceptionRecord->ExceptionCode;
        r.addr = ep->ExceptionRecord->ExceptionAddress;
        if (r.code == kCppThrow) type_name_of(ep->ExceptionRecord, r.type, sizeof(r.type));
        r.frame_count = RtlCaptureStackBackTrace(1, kMaxFrames, r.frames, nullptr);
        dump_record("FATAL", r);
    }

    // The ring, oldest first. For an unhandled C++ throw the stack is already
    // unwound by the time we get here, so the ring's last entry -- captured at
    // first chance, with the throwing frames still live -- is usually the one
    // that actually names the culprit.
    LONG total = g.ring_next;
    LONG first = total > kRingSize ? total - kRingSize : 0;
    if (total > 0)
        log_line("CRASH", "---- %ld exception(s) seen at first chance, most recent last ----",
                 total);
    for (LONG i = first; i < total; ++i) {
        char label[48];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "  [%ld]", i);
        dump_record(label, g.ring[i % kRingSize]);
    }

    log_shutdown();
    return EXCEPTION_CONTINUE_SEARCH;   // let WER do what it would have done
}

} // namespace

void crash_install(uintptr_t base) {
    if (g.handle) return;
    g.game_base = base;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&crash_install, &g.self);
    g.self_base = (uintptr_t)g.self;

    // First in the chain: we want the throw before anyone else transforms it.
    g.handle = AddVectoredExceptionHandler(1, on_exception);
    SetUnhandledExceptionFilter(on_unhandled);

    log_line("CRASH", "handler armed -- first-chance ring %d deep, %d frames per record;"
                      " a fatal exception dumps module+rva and says whether mgmp.dll is"
                      " on the stack", kRingSize, kMaxFrames);
}

void crash_shutdown() {
    if (!g.handle) return;
    if (g.guarded_faults)
        log_line_lvl(LogLevel::Trace, "CRASH",
                     "%ld guarded read(s) faulted and were caught -- pointers whose "
                     "meaning is a guess, which is what mem_read is for. Not dumped, "
                     "so a real fault still gets the report budget.",
                     g.guarded_faults);
    RemoveVectoredExceptionHandler(g.handle);
    g.handle = nullptr;
}

} // namespace mgmp
