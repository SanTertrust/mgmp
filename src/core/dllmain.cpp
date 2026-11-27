// mgmp.dll -- Mewgenics multiplayer mod, L0 loader payload.
//
// Phase 1 scope (see CLAUDE.md): get code running inside the process at fixed
// RVAs and trace the turn/command boundaries. No networking, no determinism
// work, no state changes.
#include "mgmp_addresses.h"
#include "mgmp_crash.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_hooks.h"
#include "mgmp_resolve.h"
#include "mgmp_log.h"
#include "mgmp_session.h"
#include "mgmp_ui.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace {

wchar_t g_dll_dir[MAX_PATH] = {};

// Raw-Win32 breadcrumb, independent of the CRT and of the trace log itself.
// When injection goes wrong the interesting failures happen before the logger
// exists, and "no log file" is indistinguishable from "the thread never ran".
void breadcrumb(const char* what) {
    wchar_t path[MAX_PATH];
    if (g_dll_dir[0])
        swprintf_s(path, L"%s\\mgmp_boot.log", g_dll_dir);
    else
        wcscpy_s(path, L"mgmp_boot.log");

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char  line[256];
    int   n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%s\r\n", what);
    DWORD written = 0;
    if (n > 0) WriteFile(h, line, (DWORD)n, &written, nullptr);
    CloseHandle(h);
}

void compute_dll_dir(HMODULE self) {
    GetModuleFileNameW(self, g_dll_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_dll_dir, L'\\');
    if (slash) *slash = 0;
}

DWORD WINAPI init_thread(LPVOID) {
    using namespace mgmp;
    breadcrumb("init_thread: running");

    config_load(g_dll_dir);
    breadcrumb("init_thread: config loaded");

    log_init(config().log_path);
    breadcrumb("init_thread: log open");

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HMODULE   main_mod = GetModuleHandleW(nullptr);
    uintptr_t base     = (uintptr_t)main_mod;

    log_raw("=== mgmp ===");
    log_raw("host   : %S", exe);
    log_raw("base   : %p  (pinned imagebase 0x140000000)", (void*)base);
    log_raw("pid    : %lu", GetCurrentProcessId());
    log_raw("pointers=%d", (int)tune::kPointers);

    char err[256] = {};
    if (!hooks_verify_module(base, err, sizeof(err))) {
        log_raw("[!] REFUSING TO HOOK: %s", err);
        log_raw("[!] Re-run mod/tools/ida/gen_sigs.py against a .i64 of THIS");
        log_raw("[!] build to regenerate mgmp_sigs.generated.h.");
        return 1;
    }
    {
        int resolved = 0, moved = 0, failed = 0;
        mgmp::resolve_counts(&resolved, &moved, &failed);
        log_raw("addresses: %d resolved by signature, %d moved, %d unresolved%s",
                resolved, moved, failed,
                mgmp::resolve_is_pinned_build() ? "  (pinned build)" : "");
    }

    // Opened before the hooks go live, so no draw can arrive with nowhere to go.
    //
    // The recorder's state is always stated, never implied by the absence of a
    // line. `record = 2` once parsed as false and produced a run with no capture
    // and no complaint; the only defence against that class of mistake is for
    // the banner to say plainly which mode it is in.
    if (config().parse_warnings[0])
        log_raw("[!] mgmp.json: %s", config().parse_warnings);

    // The debug panel. Started before the session so its log pane already
    // exists when the handshake starts talking, and independently of it because
    // it works -- and is worth having -- with no session at all.
    //
    // It draws from the swap takeover rather than from a frame hook, so unlike
    // everything below it does not need hook_framebegin, which net_role forces
    // on. That is what lets it come up in a plain single-player launch.
    ui_init();

    // Phase 4. Started before the hooks so the frame hook has somewhere to pump
    // to on its very first call, but it opens no socket unless net_role names
    // a role -- an unconfigured ini must stay single-player.
    if (session_start()) {
        log_raw("net    : %s   control = %s", session_status(),
                config().net_control_count ? "see below" : "(nothing -- observer)");
        for (uint32_t i = 0; i < config().net_control_count; ++i)
            log_raw("         local cat %u", (unsigned)config().net_control[i]);
    } else {
        log_raw("net    : OFF (set net.role to \"host\" or \"client\" in mgmp.json)");
    }

    // Say it loudly. Every other hook observes; this one edits the game, so a
    // capture taken with it on is not a capture of the shipped game and the log
    // must not let that go unnoticed weeks later.
    // Same rule, for the same reason: a run that does not stop at the first
    // hash mismatch is a run that cannot claim the peers stayed in sync, and
    // the log has to say so at the top rather than leave it to be inferred from
    // an absent HALT line weeks later.
    if (!config().net_desync_halt)
        log_raw("[!] desync HALT is DISABLED (debug.desync_halt = false) -- a hash"
                " mismatch will be reported and the run will CONTINUE; this run"
                " cannot show that the peers stayed in sync");

    // These two are compile-time now (mgmp_tuning.h), which makes announcing
    // them MORE important rather than less: a setting that cannot be checked by
    // opening a config file has to be visible in the log, or the only way to
    // find out what a run was made with is to find the binary that made it.
    if (config().hook[T_SaveScumPenalty])
        log_raw("[!] savescum penalty is DISABLED (tune::kHookSaveScum)"
                " -- this run does NOT match shipped behaviour");
    if (config().hook[T_TimeDelayTick])
        log_raw("[!] TimeDelayStatusApplication is TURN-DRIVEN (tune::kHookTimeDelay,"
                " %u turn(s)) -- this run does NOT match shipped behaviour",
                tune::kTimeDelayTurns);
    if (config().hook[T_DirectorUpdate] && config().hook[T_SceneUnlockedUpdate])
        log_raw("[!] Battle Scene::unlocked_update is LOCKSTEPPED to 60 Hz"
                " Director::update (tune::kHookBattleUnlocked)");

    // Before the hooks, so a fault inside hooks_install itself is caught too.
    crash_install((uintptr_t)GetModuleHandleW(nullptr));

    int n = hooks_install();
    if (n <= 0) {
        log_raw("[!] no hooks installed (%d)", n);
        return 1;
    }
    log_raw("=== %d hooks live ===", n);

    // Tell the loader it can unpark the game. Without this the loader restores
    // the two bytes it patched into FrameBegin while MinHook may be writing its
    // own jump over the same bytes -- and FrameBegin is itself a hook target.
    wchar_t ev_name[64];
    swprintf_s(ev_name, L"Local\\mgmp_hooks_ready_%lu", GetCurrentProcessId());
    if (HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, ev_name)) {
        SetEvent(ev);
        // Deliberately leaked: the loader may not have waited on it yet.
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        compute_dll_dir(module);
        breadcrumb("DllMain: PROCESS_ATTACH");
        // Do the real work off the loader lock. When injected into a suspended
        // process this still runs before the game's entry point gets going.
        CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
        breadcrumb("DllMain: init thread created");
        break;
    case DLL_PROCESS_DETACH:
        mgmp::hooks_uninstall();
        // After the hooks are gone, so nothing can append to a closed stream.
        mgmp::session_shutdown();
        mgmp::log_shutdown();
        break;
    }
    return TRUE;
}
