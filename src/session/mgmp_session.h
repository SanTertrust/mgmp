// mgmp_session.h -- host/join and the connect handshake. Phase 4, layer 4.
//
// Drives the transport up to the point where lockstep can take over:
//
//     host                          client
//     ----                          ------
//     net_host(port)                net_join(addr, port)
//     <- HELLO                      HELLO ->
//     validate, WELCOME ->          <- validate
//     lockstep_init()               lockstep_init()
//
// Either side may answer REFUSE instead, and a refusal is final. Both peers
// need byte-identical game data before the first roll: RollChance @ 0x14094B550
// takes NO draw when p >= 1.0, so stream *position* depends on which procs were
// possible, not on which fired. Two peers that disagree about whether a chance
// was 0.9 or 1.0 desync even when the visible outcome matches. That has to be
// refused here rather than discovered on turn 30.
#pragma once

#include <cstdint>
#include "mgmp_proto.h"   // Hello, for session_on_hello

namespace mgmp {

// Reads net.role/net.addr/net.port from mgmp.json and starts hosting or joining.
// A net_role the parser does not recognise -- including the default "off" --
// leaves multiplayer dormant and returns false without opening a socket.
bool session_start();
void session_shutdown();

// Called every frame from the frame hook. Runs the handshake, then hands off to
// lockstep_pump once both sides are Ready.
void session_update();

// Accept, welcome and catch up one peer that sent HELLO.
//
// Exposed because session_update stops draining the queue once this peer is
// Ready -- from then on lockstep_pump owns it, and a late joiner's HELLO
// arrives there. Both drain sites route through this so the two can never
// disagree about what accepting a peer means.
void session_on_hello(uint8_t from, const Hello& h);

bool        session_ready();
const char* session_status();   // one line for the banner / diagnostics

// --- starting and stopping a session at RUNTIME ------------------------------
//
// mgmp.json decides what happens at launch; these decide what happens when
// somebody presses a button on the debug panel. The panel is the only caller
// today, and the reason it exists is the one open item with no live evidence
// behind it: reconnect and mid-fight join. Testing that currently means killing
// a process and relaunching, which resets everything the bug might live in.
//
// BOTH ARE DEFERRED, and that is the whole point of them being here rather than
// net_host/net_join called straight from the UI.
//
// The panel draws from the SDL_GL_SwapWindow takeover -- inside the game's
// present path, with the frame half-submitted. Tearing a session down there
// would run lockstep_shutdown, net_shutdown and six *_shutdown calls underneath
// a frame that is still being drawn. So a request is recorded and applied at
// the top of the next session_update(), which is the point the session already
// ticks at and the only place in the frame that is known to be between things.
//
// A request made while another is pending replaces it: the last thing the
// player clicked is what they meant.
void session_request_host(uint16_t port);
void session_request_join(const char* addr, uint16_t port);
void session_request_disconnect();

// Whether a request is waiting to be applied. The panel greys its buttons on
// it, so a second click during the one-frame gap cannot queue a second action.
bool session_request_pending();

// What the last runtime request did, for the panel to show. "" if none has been
// made in this process.
const char* session_last_action();

} // namespace mgmp
