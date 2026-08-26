// mgmp_net.h -- TCP transport. Phase 4, layer 2.
//
// One connection, two roles, behind an interface. SteamNetworkingMessages002 is
// already linked into the game if this ever needs NAT punch or relay; the cost
// of TCP is losing those, so a first milestone is direct-IP or port-forward.
//
// THE ONE HARD RULE: nothing here may block the game's frame thread.
//
// A dedicated receive thread owns the socket and drains it into a queue the
// game thread pops under a lock. That is affordable because of a property the
// game already has -- Brain::GetChoice is a *poll*, not a decision point. It
// returns type=1 ("nothing decided yet") on every frame while waiting on a
// human, and did so for 1695 of 1711 calls in one tutorial battle. So a remote
// decision that has not arrived yet needs no special handling at all: return
// type=1 and the game waits exactly the way it already waits for a person.
//
// That is why this transport has no timeouts, no frame budget and no blocking
// receive on the game thread. Waiting is free.
#pragma once

#include <cstdint>
#include "mgmp_proto.h"

namespace mgmp {

enum class NetRole { None, Host, Client };

enum class NetState {
    Idle,          // not started
    Listening,     // host, waiting for a peer
    Connecting,    // client, dialling
    Connected,     // socket up, HELLO not yet exchanged
    Ready,         // HELLO/WELCOME done, battle may proceed
    Failed,        // gave up; see net_error()
    Closed,        // peer hung up or we did
};

// One decoded inbound message. The union is flat rather than tagged-pointer so
// the receive thread can copy it into the queue without allocating -- an
// allocation on the receive path is a stall waiting to happen.
struct NetMsg {
    uint8_t   type = 0;
    // Which peer AUTHORED this, from the frame envelope -- not which socket it
    // arrived on. The two differ for anything the host relays between clients,
    // and the difference is the whole point: with four players, "an action
    // arrived" is not enough, you need to know whose.
    uint8_t   from = kNoPeer;
    Hello     hello;
    PeersMsg  peers;
    Welcome   welcome;
    ActionMsg action;
    HashMsg   hash;
    ControlMsg control;
    CursorMsg cursor;
    AimMsg    aim;
    EnterNodeMsg enter_node;
    ChoiceMsg    choice;
    // The two messages that do not fit the "flat, no allocation" rule above.
    // Their `data` is heap-allocated by the decoder and owned by whoever pops
    // the frame, so EVERY net_poll caller must pass the frame to
    // net_msg_release when it is done with it -- including the ones that
    // ignored it.
    SaveFileMsg savefile;
    CatDataMsg  catdata;
    InventoryMsg inventory;   // owns up to kInvBuckets buffers
    RunHistMsg   runhist;     // owns its buffer, same contract as catdata
    StateDumpMsg statedump;   // owns its buffer, same contract as catdata
    NodeHashMsg  nodehash;
    HaltMsg   halt;
    char      refuse[192] = {};
};

// Frees anything the frame owns and nulls it, so calling it twice is safe and
// calling it on a frame that owns nothing costs a branch.
void net_msg_release(NetMsg& m);

// --- lifecycle (game thread) ------------------------------------------------

// port is the TCP port; host binds it, client dials <addr>:<port>.
bool net_host(uint16_t port);
bool net_join(const char* addr, uint16_t port);
void net_shutdown();

NetState    net_state();
NetRole     net_role();
const char* net_error();          // "" unless state is Failed
bool        net_active();         // role != None && state not Failed/Closed

// --- traffic ----------------------------------------------------------------

// Send is thread-safe and never blocks the caller on the peer: the socket is
// left in blocking mode for writes, but a turn is a few hundred bytes and the
// kernel send buffer swallows it whole. If that ever stops being true the fix
// is a send queue, not a timeout.
// Send to every connected peer. On a client that is the host and nothing else;
// on the host it is every live client, which is what turns each existing
// host-authored push into a broadcast without touching its call site.
// True if it reached at least one peer.
bool net_send(const uint8_t* payload, uint32_t len);

// Send to one peer. Used where a message is genuinely point to point: WELCOME
// and REFUSE, and the per-peer PEERS copies.
bool net_send_peer(uint8_t peer, const uint8_t* payload, uint32_t len);

// This peer's own id, kHostPeer on the host and kNoPeer on a client until the
// first PEERS arrives. Peer 0 is always the host.
uint8_t net_self();

// Members of the session, host included. 0 before the first PEERS.
uint8_t net_peer_count();

// This peer's INDEX in the sorted membership list. Differs from its id once
// anyone has disconnected, and it is the index -- never the id -- that the
// control split is computed from.
uint8_t net_peer_pos();

// Copy the sorted member ids out. False before the first PEERS.
bool net_peer_ids(uint8_t* out, uint8_t cap);

bool net_send_hello(const Hello& h);
bool net_send_welcome(const Welcome& w);
bool net_send_action(const ActionMsg& a);
bool net_send_hash(const HashMsg& h);
bool net_send_control(const ControlMsg& c);
bool net_send_enter_node(const EnterNodeMsg& m);
bool net_send_choice(const ChoiceMsg& m);
// Cosmetic and high-frequency relative to everything else here, so it is the
// one message a caller is expected to throttle. See mgmp_cursor.h.
bool net_send_cursor(const CursorMsg& c);
// Same contract as net_send_cursor: outside the lockstep contract entirely, so
// a drop can only make a preview flicker.
bool net_send_aim(const AimMsg& m);
// Copies m.data into a temporary frame buffer; the caller keeps ownership of
// it. This is the only send that can block the game thread for a measurable
// time -- ~45 KB is more than a default socket send buffer, so it may wait for
// the peer to drain. It happens once per session, off the battle path.
bool net_send_savefile(const SaveFileMsg& m);
// Same, to one peer -- for a player who joined after the host published.
bool net_send_savefile_to(uint8_t peer, const SaveFileMsg& m);
// Same contract as net_send_savefile, but small: a serialized cat is well under
// a kilobyte, so this one is cheap enough to send several of in a row.
bool net_send_catdata(const CatDataMsg& m);
// The run inventory, whole. Same contract again; sent alongside the cats at
// each map node, and skipped entirely when nothing in it changed.
bool net_send_inventory(const InventoryMsg& m);
// Same contract as net_send_catdata: `data` is borrowed for the duration.
bool net_send_runhist(const RunHistMsg& m);
bool net_send_nodehash(const NodeHashMsg& m);

// The desync dump. Sent at most once per divergence, so it has no throughput
// budget to respect and no dedupe to do -- by the time it goes out the run is
// already over.
bool net_send_statedump(const StateDumpMsg& m);
bool net_send_halt(const HaltMsg& h);
bool net_send_refuse(const char* reason);
// Point-to-point variants, for the handshake: with several clients, a WELCOME
// broadcast would re-welcome everyone already playing.
bool net_send_hello_to(uint8_t peer, const Hello& h);
// One peer only -- replaying a battle in progress to a joiner. See the note on
// the definition for why this must not be a broadcast.
bool net_send_action_to(uint8_t peer, const ActionMsg& a);
bool net_send_enter_node_to(uint8_t peer, const EnterNodeMsg& m);
bool net_send_welcome_to(uint8_t peer, const Welcome& w);
bool net_send_refuse_to(uint8_t peer, const char* reason);

// Pops one decoded message, or returns false if the queue is empty. Call it
// from the game thread; drain in a loop until it returns false.
bool net_poll(NetMsg& out);

// Diagnostics for the banner and the desync dump.
struct NetStats {
    uint32_t sent = 0, received = 0, dropped = 0;
    uint64_t bytes_sent = 0, bytes_received = 0;
};
NetStats net_stats();

} // namespace mgmp
