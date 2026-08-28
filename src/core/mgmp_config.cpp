#include "mgmp_config.h"
#include "mgmp_addresses.h"
#include "mgmp_tuning.h"

#include "json.hpp"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <string>

namespace mgmp {
namespace {

using nlohmann::json;

Config g_cfg;

void warn(const char* fmt, ...) {
    char note[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(note, sizeof(note), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_cfg.parse_warnings[0]) strncat_s(g_cfg.parse_warnings, "; ", _TRUNCATE);
    strncat_s(g_cfg.parse_warnings, note, _TRUNCATE);
}

// --- typed readers ----------------------------------------------------------
//
// Every one of these takes the OBJECT and the KEY rather than a value, so that
// "the key is absent" and "the key holds the wrong type" are different paths.
// The first is silent (the default is the documented answer); the second warns
// and keeps the default, because a config value that is ignored without saying
// so is exactly the failure the old truthy() note was written about.

const json* member(const json& o, const char* key) {
    if (!o.is_object()) return nullptr;
    auto it = o.find(key);
    return it == o.end() || it->is_null() ? nullptr : &*it;
}

void read_bool(const json& o, const char* key, bool& out) {
    const json* v = member(o, key);
    if (!v) return;
    if (v->is_boolean())      out = v->get<bool>();
    else if (v->is_number())  out = v->get<double>() != 0.0;
    else warn("%s is not a boolean, kept %s", key, out ? "true" : "false");
}

void read_uint(const json& o, const char* key, uint32_t& out,
               uint32_t lo, uint32_t hi) {
    const json* v = member(o, key);
    if (!v) return;
    if (!v->is_number_integer() && !v->is_number_unsigned()) {
        warn("%s is not a whole number, kept %u", key, out);
        return;
    }
    const long long n = v->get<long long>();
    if (n < (long long)lo || n > (long long)hi) {
        warn("%s = %lld is outside %u..%u, kept %u", key, n, lo, hi, out);
        return;
    }
    out = (uint32_t)n;
}

void read_str(const json& o, const char* key, char* out, size_t cap) {
    const json* v = member(o, key);
    if (!v) return;
    if (!v->is_string()) { warn("%s is not a string, kept '%s'", key, out); return; }
    strncpy_s(out, cap, v->get<std::string>().c_str(), _TRUNCATE);
}

// A path is stored wide and resolved against the DLL directory unless it is
// already absolute -- "C:\..." or "\...".
void read_path(const json& o, const char* key, const wchar_t* dll_dir,
               wchar_t* out, size_t cap) {
    const json* v = member(o, key);
    if (!v) return;
    if (!v->is_string()) { warn("%s is not a string, kept the default", key); return; }
    const std::string s = v->get<std::string>();
    if (s.empty()) { out[0] = 0; return; }

    wchar_t w[512];
    size_t conv = 0;
    mbstowcs_s(&conv, w, s.c_str(), _TRUNCATE);
    if (w[0] && (w[1] == L':' || w[0] == L'\\')) wcsncpy_s(out, cap, w, _TRUNCATE);
    else                                         swprintf_s(out, cap, L"%s\\%s", dll_dir, w);
}

// ui.key accepts "F1".."F12" or a raw virtual-key code. The names exist because
// 0x70 is precisely the kind of value that made the old file unreadable.
void read_vkey(const json& o, const char* key, uint32_t& out) {
    const json* v = member(o, key);
    if (!v) return;
    if (v->is_number_integer() || v->is_number_unsigned()) {
        const long long n = v->get<long long>();
        if (n > 0 && n <= 0xFF) out = (uint32_t)n;
        else warn("%s = %lld is not a virtual-key code, kept 0x%02X", key, n, out);
        return;
    }
    if (!v->is_string()) { warn("%s is neither a name nor a number", key); return; }
    const std::string s = v->get<std::string>();
    if ((s[0] == 'F' || s[0] == 'f') && s.size() >= 2) {
        const int n = atoi(s.c_str() + 1);
        if (n >= 1 && n <= 12) { out = 0x70 + (n - 1); return; }   // VK_F1 = 0x70
    }
    warn("%s = '%s' is not F1..F12; use a number for anything else", key, s.c_str());
}

// net.control: "auto", or an array of cat indices.
void read_control(const json& net) {
    const json* v = member(net, "control");
    if (!v) return;

    if (v->is_string()) {
        const std::string s = v->get<std::string>();
        if (_stricmp(s.c_str(), "auto") == 0) {
            g_cfg.net_control_auto  = true;
            g_cfg.net_control_count = 0;
            return;
        }
        warn("net.control = '%s' is not understood; use \"auto\" or a list like "
             "[0, 2]. Kept auto", s.c_str());
        return;
    }
    if (!v->is_array()) {
        warn("net.control is neither \"auto\" nor a list, kept auto");
        return;
    }

    // An empty array is the observer case and is deliberately reachable.
    g_cfg.net_control_auto  = false;
    g_cfg.net_control_count = 0;
    for (const json& e : *v) {
        if (g_cfg.net_control_count >= 32) { warn("net.control has more than 32 entries"); break; }
        if (!e.is_number_integer() && !e.is_number_unsigned()) {
            warn("net.control holds a non-number, skipped");
            continue;
        }
        const long long n = e.get<long long>();
        if (n < 0 || n > 255) { warn("net.control index %lld is out of range", n); continue; }
        g_cfg.net_control[g_cfg.net_control_count++] = (uint8_t)n;
    }
}

// --- hook defaults ----------------------------------------------------------

// Config::hook is a fixed array because mgmp_config.h deliberately does not
// include mgmp_addresses.h -- the config layer should not depend on the hook
// table's contents. That leaves nothing checking the two agree, and adding a
// target is exactly the moment it would silently stop being true.
static_assert(T_COUNT <= (int)(sizeof(g_cfg.hook) / sizeof(g_cfg.hook[0])),
              "Config::hook is smaller than T_COUNT -- grow it in mgmp_config.h");

void hook_defaults() {
    for (int i = 0; i < T_COUNT; ++i) g_cfg.hook[i] = true;

    // Hot path, and its only remaining job is pumping the socket -- forced on
    // below by net.role and by the panel.
    g_cfg.hook[T_FrameBegin] = false;

    // The hottest things in the harness and pure overhead unless a recording is
    // being taken, so they follow debug.record rather than standing alone.
    g_cfg.hook[T_RandInt]    = false;
    g_cfg.hook[T_RandFloat]  = false;
    g_cfg.hook[T_Rand2]      = false;
    g_cfg.hook[T_RollChance] = false;

    // The two that MODIFY the game -- see mgmp_tuning.h.
    g_cfg.hook[T_SaveScumPenalty] = tune::kHookSaveScum;
    g_cfg.hook[T_TimeDelayTick]   = tune::kHookTimeDelay;

    // The two MewSaveFile blob accessors exist only to serve mgmp_invsync, and
    // every blob in the save file passes through them, so a single-player
    // session should not have a mod hook sitting on its save path at all.
    g_cfg.hook[T_SFStoreBlob] = false;
    g_cfg.hook[T_SFLoadBlob]  = false;

    // The combat-menu lock. Off unless a session exists, and for a sharper
    // reason than tidiness: T_ButtonUpdate is a detour on EVERY button in the
    // game, and a single-player run has nothing for it to decide. It is turned
    // on below by net.role, next to the rest of the co-op surface.
    g_cfg.hook[T_CombatMenuUpdate] = false;
    g_cfg.hook[T_ButtonUpdate]     = false;

    // The aim-preview facing freeze. Also off without a session, and here the
    // reason is that the preview is real feedback: a solo player watching their
    // cat turn toward the aim has nobody to diverge from.
    g_cfg.hook[T_UpdateDecision] = false;

    // The highlight suppressor. Solo, there is nobody to draw another player's
    // aim for, so the guard it reads can never be raised and the hook would be
    // a detour on a function the game calls constantly for no reason at all.
    g_cfg.hook[T_HighlightRefresh] = false;
}

// Hooks that other settings imply. This used to be interleaved with "did the
// ini mention this key", which no longer exists -- the file cannot name a hook,
// so every rule here is unconditional and the whole thing got shorter.
void hooks_implied_by(bool net_configured) {
    if (net_configured) {
        // Multiplayer is pumped from the frame hook. Without this, role = host
        // produces a game that listens, accepts a peer, and never completes the
        // handshake, with nothing in the log to say why.
        g_cfg.hook[T_FrameBegin] = true;

        // The inventory push has no other route to the peer: without these the
        // host's bucket serialize goes into its own save file and nothing is
        // captured, which shows up as three failed buckets per map node rather
        // than as a missing feature.
        if (tune::kInvSync) {
            g_cfg.hook[T_SFStoreBlob] = true;
            g_cfg.hook[T_SFLoadBlob]  = true;
        }

        // Without EVTCHOICE / LVLSELECT the client never suppresses its own
        // clicks, so both players answer the same event and the runs part
        // company with nothing in either log to say why. Without the two update
        // hooks a choice that arrives before its screen is up is held forever.
        if (tune::kChoice) {
            g_cfg.hook[T_EventChoice] = true;
            g_cfg.hook[T_EventUpdate] = true;
            g_cfg.hook[T_LevelSelect] = true;
            g_cfg.hook[T_LevelUpdate] = true;
        }

        // The node hash's second sample point is taken on WorldEvent::update --
        // the only tick that can see which event was chosen. Without the hook
        // the enter-node samples still work, so this would degrade to half the
        // check, silently, which is what the module exists to prevent.
        if (tune::kNodeHash) g_cfg.hook[T_EventUpdate] = true;

        // Both halves or neither. The Button hook alone does nothing (its scope
        // is only ever opened by the CombatMenu hook) and the CombatMenu hook
        // alone decides ownership every frame and has no way to act on it, which
        // would be the worst of the three states: the log would report greying
        // that never appeared on screen.
        if (tune::kCombatLock) {
            g_cfg.hook[T_CombatMenuUpdate] = true;
            g_cfg.hook[T_ButtonUpdate]     = true;
        }

        // Not optional and not a diagnostic: without it the aim preview writes
        // facing under a wall-clock gate and the backstab test reads it, so two
        // peers at different frame rates compute different damage. Implied by a
        // session for the same reason the lockstep hooks are.
        g_cfg.hook[T_UpdateDecision] = true;

        // Implied by the aim preview, and NOT independently switchable: without
        // it mgmp_aim would call the ability highlight with its apply_status
        // half live, on a cat this peer does not own. That is the exact
        // mutation that cost a run on 2026-08-26. If this hook is ever missing,
        // the highlight must not be called at all.
        g_cfg.hook[T_HighlightRefresh] = tune::kAimPreview;
    }

    // The panel needs the frame hook one step removed: its connect buttons can
    // start a session in a process launched with role = off, and a socket with
    // nothing pumping it is the same dead handshake as above.
    if (g_cfg.ui) g_cfg.hook[T_FrameBegin] = true;

    if (g_cfg.record) {
        g_cfg.hook[T_RandInt]    = true;
        g_cfg.hook[T_RandFloat]  = true;
        g_cfg.hook[T_Rand2]      = true;
        g_cfg.hook[T_RollChance] = true;
        if (tune::kRecordFrames) g_cfg.hook[T_FrameBegin] = true;
    }
}

} // namespace

void config_load(const wchar_t* dll_dir) {
    // Start from a fresh Config every time. The mod calls this once, so nothing
    // depended on it -- but "correct only if called exactly once" was an unstated
    // precondition, and the first thing that called it twice (tests/test_config)
    // saw the previous load's values survive keys the new file did not mention,
    // and its warnings accumulate on top.
    g_cfg = Config{};
    hook_defaults();
    swprintf_s(g_cfg.log_path, L"%s\\mgmp_trace.log", dll_dir);

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\mgmp.json", dll_dir);

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        // Defaults are a complete, working single-player configuration, so a
        // missing file is not an error -- but it IS worth saying, because the
        // other reading of a game that ignored every setting is a typo in the
        // filename.
        warn("no mgmp.json beside mgmp.dll -- running on defaults");
        hooks_implied_by(false);
        return;
    }

    // Parse WITHOUT exceptions: this runs from DllMain's thread during startup,
    // and a throw out of here would take the game down over a stray comma.
    // Comments are allowed, so the file can carry the two or three notes that
    // are genuinely worth having in it.
    json j = json::parse(in, nullptr, /*allow_exceptions=*/false,
                         /*ignore_comments=*/true);
    if (j.is_discarded() || !j.is_object()) {
        warn("mgmp.json is not valid JSON -- EVERY setting in it was ignored "
             "and the mod is running on defaults");
        hooks_implied_by(false);
        return;
    }

    read_path(j, "log", dll_dir, g_cfg.log_path, 512);

    if (const json* net = member(j, "net")) {
        read_str (*net, "role", g_cfg.net_role, sizeof(g_cfg.net_role));
        read_str (*net, "addr", g_cfg.net_addr, sizeof(g_cfg.net_addr));
        uint32_t port = g_cfg.net_port;
        read_uint(*net, "port", port, 1, 65535);
        g_cfg.net_port = (uint16_t)port;
        read_control(*net);

        if (_stricmp(g_cfg.net_role, "off")    != 0 &&
            _stricmp(g_cfg.net_role, "host")   != 0 &&
            _stricmp(g_cfg.net_role, "client") != 0)
            warn("net.role = '%s' is not off/host/client -- multiplayer stays "
                 "dormant", g_cfg.net_role);
    }

    if (const json* ui = member(j, "ui")) {
        read_bool(*ui, "enabled", g_cfg.ui);
        read_bool(*ui, "visible", g_cfg.ui_visible);
        read_vkey(*ui, "key",     g_cfg.ui_key);
    }

    if (const json* d = member(j, "debug")) {
        read_bool(*d, "follow",          g_cfg.net_follow);
        read_bool(*d, "join_barrier",    g_cfg.net_join_barrier);
        read_bool(*d, "desync_halt",     g_cfg.net_desync_halt);
        read_bool(*d, "record",          g_cfg.record);
        read_str (*d, "record_note",     g_cfg.record_note, sizeof(g_cfg.record_note));
        read_path(*d, "replay", dll_dir, g_cfg.replay_path, 512);
        // Capped rather than trusted: a stray extra digit here would look
        // exactly like the mod having hung.
        read_uint(*d, "follow_delay_ms", g_cfg.net_follow_delay_ms, 0, 600000);
    }

    // The capture is named after the log rather than separately. The two belong
    // to one run, and two independent names is how run A's text output was lost
    // to run B -- both defaulted to a fixed filename and the second clobbered
    // the first.
    wcsncpy_s(g_cfg.record_path, g_cfg.log_path, _TRUNCATE);
    if (wchar_t* dot = wcsrchr(g_cfg.record_path, L'.'))
        if (!wcschr(dot, L'\\')) *dot = 0;
    wcsncat_s(g_cfg.record_path, L".mgr", _TRUNCATE);

    const bool net_configured = _stricmp(g_cfg.net_role, "host")   == 0 ||
                                _stricmp(g_cfg.net_role, "client") == 0;
    hooks_implied_by(net_configured);
}

const Config& config() { return g_cfg; }

// See the note in mgmp_config.h. Deliberately re-runs hooks_implied_by rather
// than only writing the string: a process launched with role = off installed
// none of the co-op hooks, and the five modules that gate on this string are
// not the whole damage -- T_SFStoreBlob, T_SFLoadBlob, T_UpdateDecision,
// T_HighlightRefresh, T_CombatMenuUpdate and T_ButtonUpdate were never created.
// Flipping the flags here is what lets hooks_install_late() put them in.
bool config_set_role(bool host) {
    const char* want = host ? "host" : "client";
    if (_stricmp(g_cfg.net_role, want) == 0) return false;
    strncpy_s(g_cfg.net_role, want, _TRUNCATE);
    hooks_implied_by(true);
    return true;
}

} // namespace mgmp
