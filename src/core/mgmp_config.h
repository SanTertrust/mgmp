// mgmp_config.h -- mgmp.json, read from the directory the DLL lives in.
//
// Everything the mod can be told at RUNTIME is here, and it is deliberately
// short. The settings that used to be in the file and are not any more live in
// mgmp_tuning.h as constants; that header explains the test they failed.
#pragma once

#include <cstdint>

namespace mgmp {

struct Config {
    // log: path to the text trace, relative to the DLL directory or absolute.
    // Per-peer, because two instances in one directory would otherwise fight
    // over one file.
    wchar_t  log_path[512] = {};

    // --- net -----------------------------------------------------------------

    // net.role = off | host | client. Anything else leaves multiplayer dormant,
    // which is the default: an unconfigured mgmp.json must not open a socket.
    char     net_role[16]  = "off";
    char     net_addr[64]  = "127.0.0.1";   // client only
    uint16_t net_port      = 27600;

    // net.control = "auto", or an array of cat indices into the battle's own
    // character list. auto derives the split from the roster and is the only
    // usable setting for a whole run -- explicit indices cannot be known before
    // the battle exists and are stale the moment you fight anything else.
    //
    // An empty array is legal and means "this peer decides for nothing", a pure
    // observer. That is NOT the same as "auto".
    uint8_t  net_control[32]   = {};
    uint32_t net_control_count = 0;
    bool     net_control_auto  = true;

    // --- debug ---------------------------------------------------------------
    //
    // The four switches below are the ones tools/net_test.ps1 flips, which is
    // the whole reason they are still in the file rather than in mgmp_tuning.h.

    // debug.follow = false stops the client following the host through the map
    // and stops its own map clicks being swallowed -- how you drive the two
    // instances to the same battle by hand. (-NoFollow)
    bool     net_follow = true;

    // debug.follow_delay_ms makes the CLIENT sit on the host's node choice for
    // that long before following it, manufacturing the late-join gap on demand.
    // The only setting in the mod that exists purely to provoke a bug.
    // (-LateClient)
    uint32_t net_follow_delay_ms = 0;

    // debug.join_barrier = false lets each peer start deciding the moment it
    // has a roster instead of waiting for the other to reach the same battle.
    // A barrier that never lifts is a stall, so being able to take it out of
    // the picture is worth the line. (-NoBarrier)
    bool     net_join_barrier = true;

    // debug.desync_halt = false turns a per-turn hash mismatch from a halt into
    // a report. It answers the one question a halting run structurally cannot,
    // because halting destroys the evidence for it: is the divergence real or
    // transient? A "turn N AGREES again" line after a mismatch is that answer.
    //
    // Scoped to the hash compare only. Every other halt still halts, because
    // those are conditions where continuing means injecting an action into the
    // wrong cat rather than watching two numbers drift. (-NoHalt)
    bool     net_desync_halt = true;

    // --- the recorder --------------------------------------------------------

    // debug.record opens the binary event stream and implies the RNG hooks --
    // a recording without them is a stream of actions and no draws, which is
    // silently useless and only noticed after playing a whole battle.
    bool     record            = false;
    wchar_t  record_path[512]  = {};   // derived from the log name
    char     record_note[256]  = {};   // stored in EV_META

    // debug.replay = "<file>" injects that capture's decisions instead of
    // polling the brain. Empty means no replay. Meant to be used WITH record:
    // run B replays run A while recording its own stream, and the two captures
    // are diffed. Enabling only replay reproduces a battle and measures
    // nothing.
    wchar_t  replay_path[512]  = {};

    // --- the debug panel -----------------------------------------------------

    bool     ui         = true;    // ui.enabled
    bool     ui_visible = true;    // whether it starts open; ui.key toggles
    uint32_t ui_key     = 0x70;    // ui.key, "F1" or a raw VK code

    // --- derived, not parsed -------------------------------------------------

    // Which hooks are installed. No longer settable from the file: the defaults
    // plus the forcing rules below them are the only combinations that were
    // ever correct, and an ini that turned one off by hand produced failures
    // that looked like missing features (see the hook_sfstoreblob note in
    // mgmp_config.cpp).
    bool     hook[32] = {};

    // Anything the parser could not make sense of, printed in the startup
    // banner: a misconfigured mod must announce itself, not fail silently.
    char     parse_warnings[512] = {};
};

// dll_dir must be the directory containing mgmp.dll (no trailing slash).
void        config_load(const wchar_t* dll_dir);
const Config& config();

} // namespace mgmp
