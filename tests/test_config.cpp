// test_config.cpp -- covers the mgmp.json reader.
//
// This one is here because the config parser is the only code in the mod that
// reads a file a HUMAN wrote, which makes it the only code whose input is not
// produced by something else we control. Every other parser in the project
// reads bytes another copy of the mod sent.
//
// The cases that matter are the ones where a setting is IGNORED: a value of the
// wrong type, a key that is out of range, a whole file that will not parse.
// Each of those has to keep the default and say so, because a config value that
// goes missing quietly is the exact failure the old ini's truthy() note was
// written about -- `record = 2` once parsed as false and produced a run with no
// capture and no complaint. Build and run:
//     cl /nologo /EHsc /std:c++17 /I..\src\core /I..\third_party test_config.cpp ..\src\core\mgmp_config.cpp && test_config.exe
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_addresses.h"   // the Target enum the hook assertions name

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>

using namespace mgmp;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL  %s\n", what); ++g_fail; }
    else       printf("  ok    %s\n", what);
}

static wchar_t g_dir[MAX_PATH];

// Writes the given text as mgmp.json in a scratch directory and loads it.
// config_load has process-wide state, so every case reloads the whole thing --
// which is also how the mod uses it.
static const Config& load(const char* text) {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\mgmp.json", g_dir);
    if (text) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    } else {
        DeleteFileW(path);
    }
    config_load(g_dir);
    return config();
}

static bool warned() { return config().parse_warnings[0] != 0; }

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(g_dir, L"%smgmp_test_config", tmp);
    CreateDirectoryW(g_dir, nullptr);

    printf("-- a missing file is a working configuration, but it says so --\n");
    {
        const Config& c = load(nullptr);
        check(_stricmp(c.net_role, "off") == 0, "role defaults to off");
        check(c.net_port == 27600,              "port defaults to 27600");
        check(c.net_control_auto,               "control defaults to auto");
        check(c.ui,                             "the panel defaults on");
        check(warned(),                         "and the absence is reported");
    }

    printf("-- the ordinary case --\n");
    {
        const Config& c = load(R"({
            "net":   { "role": "client", "addr": "10.0.0.4", "port": 27611 },
            "ui":    { "enabled": false, "key": "F4" },
            "debug": { "join_barrier": false, "follow_delay_ms": 2500 }
        })");
        check(_stricmp(c.net_role, "client") == 0, "role read");
        check(_stricmp(c.net_addr, "10.0.0.4") == 0, "addr read");
        check(c.net_port == 27611,             "port read");
        check(!c.ui,                           "panel turned off");
        check(c.ui_key == 0x73,                "F4 resolves to VK_F4 (0x73)");
        check(!c.net_join_barrier,             "join barrier turned off");
        check(c.net_follow_delay_ms == 2500,   "follow delay read");
        check(c.net_follow,                    "an unmentioned key keeps its default");
        check(!warned(),                       "and nothing is reported");
    }

    printf("-- net.control has exactly two legal shapes --\n");
    {
        const Config& c = load(R"({ "net": { "control": [0, 2] } })");
        check(!c.net_control_auto,      "an array is an explicit split");
        check(c.net_control_count == 2, "both indices kept");
        check(c.net_control[0] == 0 && c.net_control[1] == 2, "in order");
    }
    {
        // The empty array is the observer case and must NOT collapse to auto:
        // "this peer decides for nothing" is a thing you can ask for.
        const Config& c = load(R"({ "net": { "control": [] } })");
        check(!c.net_control_auto,      "an empty array is not auto");
        check(c.net_control_count == 0, "and controls nothing");
    }
    {
        // This is what net_test.ps1 used to emit for -HostCats 0, because
        // PowerShell enumerates the output of an if-expression and a
        // single-element array came out as a bare number. The parser must not
        // guess: falling back to auto silently would mean the requested split
        // was never applied and nothing said so.
        const Config& c = load(R"({ "net": { "control": 0 } })");
        check(c.net_control_auto, "a bare number is refused, not guessed at");
        check(warned(),           "and the refusal is reported");
    }

    printf("-- a value of the wrong type keeps the default AND warns --\n");
    {
        const Config& c = load(R"({ "debug": { "join_barrier": "yes" } })");
        check(c.net_join_barrier, "the string 'yes' is not a boolean");
        check(warned(),           "reported");
    }
    {
        const Config& c = load(R"({ "net": { "port": 70000 } })");
        check(c.net_port == 27600, "an out-of-range port is refused");
        check(warned(),            "reported");
    }
    {
        const Config& c = load(R"({ "ui": { "key": "Escape" } })");
        check(c.ui_key == 0x70, "an unknown key name keeps F1");
        check(warned(),         "reported");
    }
    {
        const Config& c = load(R"({ "net": { "role": "server" } })");
        // Kept verbatim: session_start only acts on host/client, so an unknown
        // role is dormant. The point is that it is not SILENTLY dormant.
        check(!c.net_control_auto == false, "control untouched");
        check(warned(),                     "an unknown role is reported");
    }

    printf("-- a file that will not parse loses everything, loudly --\n");
    {
        const Config& c = load("{ \"net\": { \"role\": \"host\", } oops");
        check(_stricmp(c.net_role, "off") == 0,
              "nothing from a malformed file is applied");
        check(warned(), "and it is reported rather than half-applied");
    }

    printf("-- booleans accept 0/1, because people type them --\n");
    {
        const Config& c = load(R"({ "debug": { "desync_halt": 0, "record": 1 } })");
        check(!c.net_desync_halt, "0 is false");
        check(c.record,           "1 is true");
    }

    printf("-- the capture is named after the log, so one name sets both --\n");
    {
        const Config& c = load(R"({ "log": "runF.log", "debug": { "record": true } })");
        check(wcsstr(c.log_path,    L"runF.log") != nullptr, "log path honoured");
        check(wcsstr(c.record_path, L"runF.mgr") != nullptr,
              "and the capture takes the same stem");
    }

    printf("-- hooks the file cannot name are still implied correctly --\n");
    {
        const Config& c = load(R"({ "net": { "role": "host" }, "ui": { "enabled": false } })");
        check(c.hook[T_FrameBegin],
              "a configured role forces the frame hook that pumps the socket");
        check(c.hook[T_SFStoreBlob] && c.hook[T_SFLoadBlob],
              "and the two blob accessors the inventory push needs");
        check(c.hook[T_EventChoice] && c.hook[T_EventUpdate],
              "and the choice screens");
        check(c.hook[T_SaveScumPenalty] == tune::kHookSaveScum,
              "while the game-modifying hooks follow mgmp_tuning.h alone");
    }
    {
        const Config& c = load(R"({ "ui": { "enabled": true } })");
        check(c.hook[T_FrameBegin],
              "the panel alone forces it too -- its connect buttons open a socket");
        check(!c.hook[T_RandInt],
              "but the RNG hooks stay off without debug.record");
    }
    {
        const Config& c = load(R"({ "debug": { "record": true } })");
        check(c.hook[T_RandInt] && c.hook[T_RandFloat] &&
              c.hook[T_Rand2]   && c.hook[T_RollChance],
              "debug.record implies all four RNG hooks");
    }

    printf(g_fail ? "\n%d FAILED\n" : "\nall good\n", g_fail);
    return g_fail ? 1 : 0;
}
