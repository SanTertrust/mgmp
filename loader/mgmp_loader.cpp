// mgmp_loader.exe -- L0 loader.
//
// Launches the game suspended, injects mgmp.dll, then resumes. Suspended-launch
// (rather than attaching to a running process) is what lets the harness hook
// ApplicationBase::initSystems, which runs once during startup.
//
// The game is left untouched on disk: nothing is copied into its directory and
// no DLL next to the exe is proxied. That also sidesteps the fact that this
// install's steam_api64.dll slot is already occupied.
//
//   mgmp_loader.exe                       -- game path from mgmp.json
//   mgmp_loader.exe <path\to\Mewgenics.exe> [args...]
//   mgmp_loader.exe --attach <pid>        -- inject into an already-running process
//
// Diagnostic env vars:
//   MGMP_NOINJECT=1  launch suspended and resume without injecting -- isolates
//                    "the game dislikes CREATE_SUSPENDED" from "the game
//                    dislikes our DLL".
//   MGMP_WAIT=1      block until the game exits and print its exit code.
//   MGMP_NOFOLLOW=1  do not follow a Steam re-launch; report it and stop, which
//                    is what you want when you are trying to see the relaunch
//                    itself rather than work around it.
#include "mgmp_addresses.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <string>

#include "json.hpp"

namespace {

void die(const wchar_t* what) {
    DWORD e = GetLastError();
    fflush(stdout);   // ExitProcess does not flush the CRT, and losing the
                      // preceding progress lines makes the error unreadable
    wchar_t msg[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, msg, 512, nullptr);
    fwprintf(stderr, L"[!] %s failed (%lu): %s\n", what, e, msg);
    ExitProcess(1);
}

void loader_dir(wchar_t* out) {
    GetModuleFileNameW(nullptr, out, MAX_PATH);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
}

// Pulls "game" out of mgmp.json next to the loader.
//
// The loader parses the file a second time rather than sharing mgmp_config.cpp,
// because it needs exactly one string and runs before the DLL exists. Parsing
// without exceptions for the same reason the DLL does: a stray comma in a
// config file must produce a message, not a crash report.
bool game_from_config(const wchar_t* dir, wchar_t* out, size_t out_len) {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\mgmp.json", dir);

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    nlohmann::json j = nlohmann::json::parse(in, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) {
        fwprintf(stderr, L"[!] mgmp.json is not valid JSON\n");
        return false;
    }
    auto it = j.find("game");
    if (it == j.end() || !it->is_string()) return false;

    const std::string s = it->get<std::string>();
    if (s.empty()) return false;
    size_t conv = 0;
    mbstowcs_s(&conv, out, out_len, s.c_str(), _TRUNCATE);
    return out[0] != 0;
}

// --- pre-main injection -----------------------------------------------------
//
// CreateRemoteThread(LoadLibraryW) cannot be used on a process straight out of
// CREATE_SUSPENDED: nothing but ntdll and the exe is mapped, so the thread
// starts on an unmapped address and takes an access violation that kills the
// process. (GetExitCodeThread then reports the exception code, which looks like
// a valid non-NULL HMODULE -- the failure is silent.)
//
// So: park the process at its own entry point with a two-byte `jmp $`, let the
// real loader run to completion, inject once kernel32 is mapped, then restore
// the bytes. That still gets us in before main, which is what hooking
// ApplicationBase::initSystems requires.

typedef LONG (NTAPI* PFN_NtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);

bool remote_image_base(HANDLE proc, uintptr_t* out) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto q = (PFN_NtQueryInformationProcess)GetProcAddress(ntdll, "NtQueryInformationProcess");
    if (!q) return false;

    struct { PVOID Reserved1; PVOID PebBaseAddress; PVOID R2[2]; ULONG_PTR UniqueProcessId; PVOID R3; } pbi = {};
    if (q(proc, 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi), nullptr) < 0) return false;
    if (!pbi.PebBaseAddress) return false;

    // PEB.ImageBaseAddress is at +0x10 on x64.
    uintptr_t base = 0;
    if (!ReadProcessMemory(proc, (BYTE*)pbi.PebBaseAddress + 0x10, &base, sizeof(base), nullptr))
        return false;
    *out = base;
    return true;
}

bool entry_point_of(HANDLE proc, uintptr_t image_base, uintptr_t* out) {
    IMAGE_DOS_HEADER dos = {};
    if (!ReadProcessMemory(proc, (void*)image_base, &dos, sizeof(dos), nullptr)) return false;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return false;

    IMAGE_NT_HEADERS64 nt = {};
    if (!ReadProcessMemory(proc, (BYTE*)image_base + dos.e_lfanew, &nt, sizeof(nt), nullptr)) return false;
    if (nt.Signature != IMAGE_NT_SIGNATURE) return false;

    *out = image_base + nt.OptionalHeader.AddressOfEntryPoint;
    return true;
}

bool write_code(HANDLE proc, uintptr_t addr, const void* bytes, size_t n) {
    DWORD old = 0;
    if (!VirtualProtectEx(proc, (void*)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = WriteProcessMemory(proc, (void*)addr, bytes, n, nullptr) != 0;
    VirtualProtectEx(proc, (void*)addr, n, old, &old);
    FlushInstructionCache(proc, (void*)addr, n);
    return ok;
}

// Blocks until the main thread is actually spinning on the parked entry point,
// and leaves it suspended there.
//
// "kernel32 is mapped" is NOT a usable signal: kernel32 is mapped early in
// LdrpInitializeProcess, long before the entry point is reached, so suspending
// on it catches the thread inside the loader while it holds the loader lock --
// the remote LoadLibraryW then deadlocks and the process dies with
// STATUS_DLL_INIT_FAILED (0xC0000142). RIP == entry is the real condition: it
// can only be true after loader initialization has finished.
bool wait_until_parked(HANDLE thread, uintptr_t entry, DWORD timeout_ms) {
    DWORD waited = 0;
    while (waited < timeout_ms) {
        if (SuspendThread(thread) != (DWORD)-1) {
            alignas(16) CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(thread, &ctx) && ctx.Rip == entry)
                return true;                       // left suspended on purpose
            ResumeThread(thread);
        }
        Sleep(5);
        waited += 5;
    }
    return false;
}

// True if `t` is sitting exactly on `addr`, in which case it is LEFT SUSPENDED.
// Otherwise it is resumed and nothing is disturbed.
bool check_thread_parked(HANDLE t, uintptr_t addr) {
    if (SuspendThread(t) == (DWORD)-1) return false;
    alignas(16) CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(t, &ctx) && ctx.Rip == addr) return true;
    ResumeThread(t);
    return false;
}

// Same idea as wait_until_parked, but it does not assume the initial thread is
// the one that runs the frame loop.
//
// The entry-point park (stage 1) can safely watch pi.hThread, because at that
// moment the initial thread is the only one there is. FrameBegin is a different
// situation: by the time startup has finished the process has many threads, and
// if the frame loop is driven from any of them the `jmp $` spins there while
// this loop stares at an initial thread that is parked somewhere else entirely.
// The symptom is a hung game plus a loader that waits out its full timeout --
// which is exactly what a machine that differs from the dev box will show,
// while the dev box passes every time.
//
// Returns an owned handle to the parked thread, left suspended. The primary
// thread is polled every iteration because it is the overwhelmingly likely
// answer and costs one suspend/resume pair; the full enumeration is throttled
// to 4 Hz because suspending every thread in the process at 200 Hz would itself
// slow the startup we are waiting on.
//
// `proc` is watched for death every iteration, because the target exiting is a
// real outcome here rather than an impossible one -- see the call site.
HANDLE wait_until_parked_any(HANDLE proc, DWORD pid, DWORD primary_tid, uintptr_t addr,
                             DWORD timeout_ms, DWORD* out_tid, bool* out_exited) {
    const DWORD kAccess = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT;

    if (out_exited) *out_exited = false;
    HANDLE primary = OpenThread(kAccess, FALSE, primary_tid);
    DWORD  waited  = 0;
    while (waited < timeout_ms) {
        if (WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) {
            if (out_exited) *out_exited = true;
            if (primary) CloseHandle(primary);
            return nullptr;
        }
        if (primary && check_thread_parked(primary, addr)) {
            if (out_tid) *out_tid = primary_tid;
            return primary;
        }

        if (waited % 250 == 0) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te = { sizeof(te) };
                if (Thread32First(snap, &te)) {
                    do {
                        if (te.th32OwnerProcessID != pid) continue;
                        if (te.th32ThreadID == primary_tid) continue;   // just checked
                        HANDLE t = OpenThread(kAccess, FALSE, te.th32ThreadID);
                        if (!t) continue;
                        if (check_thread_parked(t, addr)) {
                            CloseHandle(snap);
                            if (primary) CloseHandle(primary);
                            if (out_tid) *out_tid = te.th32ThreadID;
                            return t;
                        }
                        CloseHandle(t);
                    } while (Thread32Next(snap, &te));
                }
                CloseHandle(snap);
            }
        }

        Sleep(5);
        waited += 5;
    }
    if (primary) CloseHandle(primary);
    return nullptr;
}

// Ground truth for "did the DLL actually load", independent of the remote
// thread's reported exit code -- see the comment at its call site for why the
// exit code alone cannot be trusted.
bool module_loaded(DWORD pid, const wchar_t* path) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = { sizeof(me) };
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szExePath, path) == 0) { found = true; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// --- following a Steam relaunch ---------------------------------------------
//
// SteamAPI_RestartAppIfNecessary makes the process we launched exit(0) and has
// Steam start a fresh one. That replacement is not ours: we did not create it,
// so we cannot park it at its entry point, and by the time we can see it its
// startup is already under way. Late attach is therefore the only option, and
// it is an acceptable one -- the mod hooks nothing that runs before frame 1.

bool proc_path(DWORD pid, wchar_t* out, DWORD cap) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD n  = cap;
    bool  ok = QueryFullProcessImageNameW(h, 0, out, &n) != 0;
    CloseHandle(h);
    return ok;
}

uint64_t as_u64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

// Polls for a process running the same exe, with a different pid, created no
// earlier than `since` -- which is the launched process's own creation time, so
// a Mewgenics that was already running before we started is never mistaken for
// the relaunch.
//
// The timeout is generous on purpose. The relaunch is not "Steam starts a
// process" -- if the Steam client is not already running it has to cold start,
// log in, and possibly run an update check or put a dialog in front of the
// user first. Minutes is a normal outcome, not a pathological one.
//
// It also reports while it waits. A silent wait is precisely what made this
// whole failure unreadable in the first place: a loader sitting there saying
// nothing, next to a game that looks fine.
DWORD wait_for_relaunch(const wchar_t* exe_path, DWORD exclude_pid,
                        uint64_t since, DWORD timeout_ms) {
    const wchar_t* base = wcsrchr(exe_path, L'\\');
    base = base ? base + 1 : exe_path;

    DWORD waited = 0;
    while (waited < timeout_ms) {
        if (waited && waited % 5000 == 0)
            wprintf(L"[*] still waiting for the re-launch (%lus of %lus)%s\n",
                    waited / 1000, timeout_ms / 1000,
                    waited >= 15000 ? L" -- if Steam was not already running, it may be "
                                      L"starting up or asking you something" : L"");
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe = { sizeof(pe) };
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (pe.th32ProcessID == exclude_pid) continue;
                    if (_wcsicmp(pe.szExeFile, base) != 0) continue;

                    // Same basename is not enough -- confirm it is the same file
                    // on disk, and that it is newer than the one we launched.
                    wchar_t full[MAX_PATH] = {};
                    if (!proc_path(pe.th32ProcessID, full, MAX_PATH)) continue;
                    if (_wcsicmp(full, exe_path) != 0) continue;

                    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                           pe.th32ProcessID);
                    if (!h) continue;
                    FILETIME c = {}, e = {}, k = {}, u = {};
                    bool fresh = GetProcessTimes(h, &c, &e, &k, &u) && as_u64(c) >= since;
                    CloseHandle(h);
                    if (!fresh) continue;

                    CloseHandle(snap);
                    return pe.th32ProcessID;
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        Sleep(100);
        waited += 100;
    }
    return 0;
}

// A remote LoadLibraryW thread in a process whose loader has not yet mapped
// kernel32 starts on an unmapped address and takes the process down with it --
// the same failure documented above for CREATE_SUSPENDED. We did not create
// this process, so we cannot know how far along it is; wait for kernel32 to
// appear in its module list before touching it.
bool wait_modules_ready(DWORD pid, DWORD timeout_ms) {
    DWORD waited = 0;
    while (waited < timeout_ms) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me = { sizeof(me) };
            bool found = false;
            if (Module32FirstW(snap, &me)) {
                do {
                    if (_wcsicmp(me.szModule, L"kernel32.dll") == 0) { found = true; break; }
                } while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);
            if (found) return true;
        }
        // ERROR_BAD_LENGTH is expected while the target's module list is still
        // being built; it is a reason to retry, not to give up.
        Sleep(50);
        waited += 50;
    }
    return false;
}

bool inject(HANDLE proc, const wchar_t* dll_path) {
    SIZE_T bytes = (wcslen(dll_path) + 1) * sizeof(wchar_t);

    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) die(L"VirtualAllocEx");

    if (!WriteProcessMemory(proc, remote, dll_path, bytes, nullptr)) die(L"WriteProcessMemory");

    // kernel32 is at the same base in every process on a given boot, so the
    // local address of LoadLibraryW is valid in the target -- but only once the
    // target's loader has actually mapped kernel32. In a freshly created
    // suspended process it has not, and a remote thread starting there dies of
    // an access violation that GetExitCodeThread reports as a "nonzero handle".
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto    load = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
    if (!load) die(L"GetProcAddress(LoadLibraryW)");

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQueryEx(proc, (void*)load, &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) {
        fwprintf(stderr, L"[!] LoadLibraryW (%p) is not mapped in the target "
                         L"(state=0x%lX type=0x%lX) -- kernel32 is not loaded there yet.\n",
                 (void*)load, mbi.State, mbi.Type);
        return false;
    }

    // Same address, same bytes? If the target's kernel32 sits elsewhere, or the
    // export is hooked, the remote thread starts on something that is not the
    // function we think it is.
    {
        unsigned char here[16] = {}, there[16] = {};
        memcpy(here, (void*)load, sizeof(here));
        SIZE_T got = 0;
        ReadProcessMemory(proc, (void*)load, there, sizeof(there), &got);
        if (memcmp(here, there, sizeof(here)) != 0) {
            wprintf(L"[*] LoadLibraryW bytes differ between loader and target:\n    loader: ");
            for (int i = 0; i < 16; ++i) wprintf(L"%02X ", here[i]);
            wprintf(L"\n    target: ");
            for (int i = 0; i < 16; ++i) wprintf(L"%02X ", there[i]);
            wprintf(L"\n");
        }
        wprintf(L"[*] target region base %p size 0x%zX protect 0x%lX\n",
                mbi.AllocationBase, mbi.RegionSize, mbi.Protect);
    }

    // Bring-up probe: does a remote thread survive at all here? A thread that
    // only calls Sleep() still runs every loaded DLL's DLL_THREAD_ATTACH first,
    // so a fault here indicts thread creation rather than LoadLibraryW.
    wchar_t probe_flag[8];
    if (GetEnvironmentVariableW(L"MGMP_PROBE", probe_flag, 8) > 0 && probe_flag[0] == L'1') {
        auto sleep_fn = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "Sleep");
        HANDLE pth = CreateRemoteThread(proc, nullptr, 0, sleep_fn, (void*)10, 0, nullptr);
        if (!pth) {
            wprintf(L"[*] probe: CreateRemoteThread(Sleep) failed, error %lu\n", GetLastError());
        } else {
            WaitForSingleObject(pth, 5000);
            DWORD prc = 0;
            GetExitCodeThread(pth, &prc);
            CloseHandle(pth);
            wprintf(L"[*] probe: remote Sleep() thread exit code 0x%08lX\n", prc);
        }
    }

    HANDLE th = CreateRemoteThread(proc, nullptr, 0, load, remote, 0, nullptr);
    if (!th) die(L"CreateRemoteThread");

    WaitForSingleObject(th, 15000);
    DWORD rc = 0;
    GetExitCodeThread(th, &rc);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);

    wprintf(L"[*] remote LoadLibraryW thread exit code 0x%08lX\n", rc);

    // rc is only the low 32 bits of the HMODULE LoadLibraryW actually returned
    // -- CreateRemoteThread's exit code is a DWORD even though the real return
    // value is a 64-bit pointer. A DLL base address is 64KB-aligned but
    // otherwise ASLR-random, so on a genuine SUCCESS the low 32 bits land
    // >= 0xC0000000 (and so look exactly like an NTSTATUS failure) about 1 in
    // 4 launches by pure chance. Measured: exit code 0xF4F30000, 64KB-aligned,
    // no matching crash in the Application event log -- a false positive from
    // treating rc's high bits as meaningful, not a real fault. The only ground
    // truth is whether the DLL is actually loaded, so check that instead.
    if (module_loaded(GetProcessId(proc), dll_path)) return true;

    if (rc >= 0xC0000000) {
        fwprintf(stderr, L"[!] remote thread exit code 0x%08lX looks like an NTSTATUS, "
                         L"and the DLL is NOT loaded in the target -- the remote "
                         L"thread faulted instead of loading it.\n", rc);
    } else {
        fwprintf(stderr, L"[!] LoadLibraryW did not load the DLL in the target "
                         L"(thread exit 0x%08lX) -- is mgmp.dll x64 and are its "
                         L"dependencies present?\n", rc);
    }
    return false;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // progress must survive a hard exit

    wchar_t dir[MAX_PATH];
    loader_dir(dir);

    wchar_t dll[MAX_PATH];
    // MGMP_DLL lets a bring-up test inject something known-innocuous (e.g.
    // C:\Windows\System32\version.dll) to tell "the target rejects injection"
    // apart from "our payload is broken".
    if (GetEnvironmentVariableW(L"MGMP_DLL", dll, MAX_PATH) == 0)
        swprintf_s(dll, L"%s\\mgmp.dll", dir);
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"[!] mgmp.dll not found next to the loader: %s\n", dll);
        return 1;
    }

    // --- attach mode ---
    if (argc >= 3 && _wcsicmp(argv[1], L"--attach") == 0) {
        DWORD  pid  = (DWORD)wcstoul(argv[2], nullptr, 10);
        HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                  FALSE, pid);
        if (!proc) die(L"OpenProcess");
        wprintf(L"[*] attaching to pid %lu\n", pid);
        bool ok = inject(proc, dll);
        CloseHandle(proc);
        wprintf(ok ? L"[+] injected.\n" : L"[!] injection failed.\n");
        return ok ? 0 : 1;
    }

    // --- launch mode ---
    wchar_t game[MAX_PATH] = {};
    int     first_game_arg = 2;
    if (argc >= 2) {
        wcsncpy_s(game, argv[1], _TRUNCATE);
    } else if (!game_from_config(dir, game, MAX_PATH)) {
        fwprintf(stderr,
                 L"usage: mgmp_loader.exe <path\\to\\Mewgenics.exe> [args...]\n"
                 L"       mgmp_loader.exe --attach <pid>\n"
                 L"   or set  \"game\": \"<path>\"  in mgmp.json next to the loader.\n");
        return 1;
    }
    if (GetFileAttributesW(game) == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"[!] game exe not found: %s\n", game);
        return 1;
    }

    // The game resolves resources.gpak relative to the working directory, so it
    // must be the game's own folder, not the loader's.
    wchar_t workdir[MAX_PATH];
    wcsncpy_s(workdir, game, _TRUNCATE);
    if (wchar_t* slash = wcsrchr(workdir, L'\\')) *slash = 0;

    wchar_t cmdline[4096];
    swprintf_s(cmdline, L"\"%s\"", game);
    for (int i = first_game_arg; i < argc; ++i) {
        wcsncat_s(cmdline, L" ", _TRUNCATE);
        wcsncat_s(cmdline, argv[i], _TRUNCATE);
    }

    STARTUPINFOW        si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(game, cmdline, nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, workdir, &si, &pi))
        die(L"CreateProcessW");

    wprintf(L"[*] launched suspended: pid %lu\n", pi.dwProcessId);

    wchar_t flag[8];
    bool no_inject = GetEnvironmentVariableW(L"MGMP_NOINJECT", flag, 8) > 0 && flag[0] == L'1';
    bool wait_exit = GetEnvironmentVariableW(L"MGMP_WAIT", flag, 8) > 0 && flag[0] == L'1';

    auto abort_launch = [&](const wchar_t* why) -> int {
        fwprintf(stderr, L"[!] %s\n", why);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    };

    if (no_inject) {
        wprintf(L"[*] MGMP_NOINJECT=1 -- resuming without injecting\n");
        ResumeThread(pi.hThread);
    } else {
        uintptr_t image_base = 0, entry = 0;
        if (!remote_image_base(pi.hProcess, &image_base))
            return abort_launch(L"could not read the target's PEB image base");
        if (!entry_point_of(pi.hProcess, image_base, &entry))
            return abort_launch(L"could not read the target's PE entry point");

        uintptr_t park_at = image_base + mgmp::kParkRva;
        wprintf(L"[*] image base %p, entry %p, park %p (FrameBegin)\n",
                (void*)image_base, (void*)entry, (void*)park_at);

        // --- stage 1: hold at the entry point ---------------------------------
        // The game image is mapped but nothing has run, so this is where we get
        // to plant the real park point.
        unsigned char saved_entry[2] = {};
        if (!ReadProcessMemory(pi.hProcess, (void*)entry, saved_entry, sizeof(saved_entry), nullptr))
            return abort_launch(L"could not read the entry point bytes");
        const unsigned char park_insn[2] = { 0xEB, 0xFE };   // jmp $
        if (!write_code(pi.hProcess, entry, park_insn, sizeof(park_insn)))
            return abort_launch(L"could not patch the entry point");

        ResumeThread(pi.hThread);
        if (!wait_until_parked(pi.hThread, entry, 30000))
            return abort_launch(L"timed out waiting for the target to reach its entry point");

        // --- stage 2: move the park to FrameBegin -----------------------------
        unsigned char actual_sig[16] = {};
        if (!ReadProcessMemory(pi.hProcess, (void*)park_at, actual_sig, sizeof(actual_sig), nullptr))
            return abort_launch(L"could not read the park point");
        if (memcmp(actual_sig, mgmp::kParkSig, sizeof(actual_sig)) != 0)
            return abort_launch(L"park point prologue does not match the pinned build "
                                L"-- refusing to patch (re-derive kParkRva)");

        unsigned char saved_park[2] = { actual_sig[0], actual_sig[1] };
        if (!write_code(pi.hProcess, park_at, park_insn, sizeof(park_insn)))
            return abort_launch(L"could not patch the park point");

        // Let the game start up for real: CRT init, initSystems, Steam init.
        write_code(pi.hProcess, entry, saved_entry, sizeof(saved_entry));
        alignas(16) CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(pi.hThread, &ctx)) { ctx.Rip = entry; SetThreadContext(pi.hThread, &ctx); }
        ResumeThread(pi.hThread);

        wprintf(L"[*] running startup, waiting for the first frame...\n");
        DWORD  parked_tid = 0;
        bool   exited     = false;
        HANDLE parked = wait_until_parked_any(pi.hProcess, pi.dwProcessId,
                                              GetThreadId(pi.hThread),
                                              park_at, 120000, &parked_tid, &exited);
        if (exited) {
            // The process we patched died during startup without ever reaching
            // frame 1. On a genuine Steam copy that is not a crash, it is the
            // design working as intended:
            //
            //   glaiel::SteamAPI::init @ 0x140A06630
            //     call SteamAPI_RestartAppIfNecessary
            //     jnz  0x140A067F9   ->  xor ecx,ecx / call exit
            //
            // Steam relaunches the game as a BRAND NEW process, and this one
            // exits(0). Everything we patched and everything we were waiting for
            // belonged to the corpse; the window on screen is the replacement,
            // unpatched and un-injected. It looks exactly like a hang because
            // the game is visibly fine and the loader is visibly stuck.
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            wprintf(L"[*] the target exited during startup (code %lu) before reaching frame 1 "
                    L"-- this is SteamAPI_RestartAppIfNecessary; Steam is re-launching the "
                    L"game as a new process.\n", code);

            // Only chase a clean exit. exit(0) is what the restart path does; a
            // startup crash exits nonzero, and following that would turn a
            // legible failure into a confusing hunt for a process that is never
            // going to appear.
            wchar_t nofollow[8];
            bool follow = code == 0 &&
                          !(GetEnvironmentVariableW(L"MGMP_NOFOLLOW", nofollow, 8) > 0 &&
                            nofollow[0] == L'1');

            DWORD new_pid = 0;
            if (follow) {
                FILETIME c = {}, e = {}, k = {}, u = {};
                uint64_t since = GetProcessTimes(pi.hProcess, &c, &e, &k, &u) ? as_u64(c) : 0;
                // Overridable, because the right ceiling depends on how cold
                // that machine's Steam is and we cannot know that from here.
                DWORD   follow_ms = 300000;   // 5 minutes
                wchar_t env[16];
                if (GetEnvironmentVariableW(L"MGMP_FOLLOW_TIMEOUT", env, 16) > 0) {
                    DWORD secs = (DWORD)wcstoul(env, nullptr, 10);
                    if (secs) follow_ms = secs * 1000;
                }
                wprintf(L"[*] waiting for the re-launched process (up to %lus; "
                        L"MGMP_FOLLOW_TIMEOUT=<seconds> to change)...\n", follow_ms / 1000);
                new_pid = wait_for_relaunch(game, pi.dwProcessId, since, follow_ms);
            }

            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);

            if (!new_pid) {
                fwprintf(stderr,
                    L"[!] no re-launched process appeared, so the game is NOT injected.\n"
                    L"    Fixes: put a steam_appid.txt containing the app id next to "
                    L"Mewgenics.exe\n"
                    L"    (the id is the number in steamapps\\appmanifest_<id>.acf), which stops\n"
                    L"    the re-launch happening at all -- or start the game normally and use:\n"
                    L"        mgmp_loader.exe --attach <pid>\n");
                return 1;
            }

            wprintf(L"[*] following the re-launch into pid %lu\n", new_pid);
            if (!wait_modules_ready(new_pid, 30000))
                fwprintf(stderr, L"[!] kernel32 never showed up in pid %lu's module list; "
                                 L"injecting anyway\n", new_pid);

            HANDLE np = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                    PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                    FALSE, new_pid);
            if (!np) {
                fwprintf(stderr, L"[!] could not open pid %lu (%lu) -- attach by hand with: "
                                 L"mgmp_loader.exe --attach %lu\n",
                         new_pid, GetLastError(), new_pid);
                return 1;
            }
            bool nok = inject(np, dll);
            CloseHandle(np);
            if (!nok) {
                fwprintf(stderr, L"[!] injection into the re-launched process failed.\n");
                return 1;
            }
            // Deliberately NOT the pre-frame-1 path: this process was started by
            // Steam and is already running, so hooks land mid-startup or later.
            // Nothing the mod hooks runs before frame 1, so this is equivalent in
            // practice -- but it is the less-exercised route, and saying so here
            // is what makes an odd log afterwards interpretable.
            wprintf(L"[+] injected into the re-launched game (late attach). "
                    L"trace -> %s\\mgmp_trace.log\n", dir);
            return 0;
        }
        if (!parked) {
            // Two very different failures land here and they need different
            // fixes, so read the park point back before blaming the timeout.
            // If our `jmp $` is gone, nothing was ever going to hit it: the code
            // page was restored under us (an overlay, an AV, an anti-cheat), and
            // no amount of waiting helps.
            unsigned char now[2] = {};
            if (ReadProcessMemory(pi.hProcess, (void*)park_at, now, sizeof(now), nullptr) &&
                (now[0] != 0xEB || now[1] != 0xFE)) {
                fwprintf(stderr, L"[!] the park patch at FrameBegin is gone -- expected EB FE, "
                                 L"found %02X %02X. Something in the target restored that code "
                                 L"page after we wrote it.\n", now[0], now[1]);
                TerminateProcess(pi.hProcess, 1);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                return 1;
            }
            return abort_launch(L"timed out waiting for the game to reach its first frame "
                                L"(the park bytes are still in place, so no thread ever "
                                L"executed FrameBegin)");
        }
        if (parked_tid != GetThreadId(pi.hThread))
            wprintf(L"[*] note: the frame loop runs on thread %lu, not the initial thread %lu\n",
                    parked_tid, GetThreadId(pi.hThread));

        // --- stage 3: inject, then let frame 1 proceed ------------------------
        //
        // Restore the park bytes *first*. The thread is suspended with RIP
        // sitting on this instruction, so it cannot run away, and the DLL must
        // see a pristine prologue: FrameBegin is itself a hook target, and
        // MinHook builds its trampoline by copying the bytes it finds there. If
        // `jmp $` were still in place, that infinite loop would be copied into
        // the trampoline and every call to the original would hang.
        write_code(pi.hProcess, park_at, saved_park, sizeof(saved_park));

        // Create the readiness event before injecting so the DLL cannot signal
        // into a name that does not exist yet.
        wchar_t ev_name[64];
        swprintf_s(ev_name, L"Local\\mgmp_hooks_ready_%lu", pi.dwProcessId);
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ev_name);

        wprintf(L"[*] parked before frame 1; injecting %s\n", dll);
        bool ok = inject(pi.hProcess, dll);

        if (ok && ready) {
            if (WaitForSingleObject(ready, 30000) != WAIT_OBJECT_0)
                wprintf(L"[!] timed out waiting for hooks-ready; unparking anyway\n");
            else
                wprintf(L"[*] hooks reported ready\n");
        }
        if (ready) CloseHandle(ready);

        // RIP is already on park_at; set it explicitly so a hook installed over
        // that address is entered cleanly on the very first frame.
        // Resume the thread that is actually parked, which is not necessarily
        // pi.hThread -- resuming the wrong one leaves the game hung forever.
        ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(parked, &ctx)) { ctx.Rip = park_at; SetThreadContext(parked, &ctx); }
        ResumeThread(parked);
        CloseHandle(parked);

        if (!ok) return abort_launch(L"injection failed");
    }

    wprintf(L"[+] resumed. trace -> %s\\mgmp_trace.log\n", dir);

    int rc = 0;
    if (wait_exit) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        wprintf(L"[*] game exited with code %lu (0x%08lX)\n", code, code);
        rc = (int)code;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return rc;
}
