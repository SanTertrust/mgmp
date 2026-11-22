#pragma once
// mgmp_draft -- Adventure cat ownership draft (host-authoritative).
//
// When the host clicks a node on the Adventure map and no ownership assignment
// exists yet for this run (with >= 2 players connected), the node entry is held
// while all peers see a native Mewgenics UI overlay (mgmp_gameui) showing the
// party's cats with their live CatParts visuals, names, and classes.
//
// Quotas follow split_for(cat_count, peer_count, pos).count:
//   2 players, 4 cats -> 2 / 2
//   3 players, 4 cats -> 2 / 1 / 1  (host gets 2, other two get 1 each)
//   4 players, 4 cats -> 1 / 1 / 1 / 1
//
// First player to claim a cat gets it (host-authoritative). Clicking a cat you
// already claimed unpicks it while the draft is still open. Once all assignable
// cats are claimed, the overlay closes automatically, the held node is entered,
// and the cat_uid -> peer_id mapping is remembered for the entire adventure run
// (used by battles, LevelUpScreen, and cat-targeted WorldEvents).

#include "mgmp_proto.h"
#include "mgmp_split.h"

#include <cstdint>

namespace mgmp {

// ---------------------------------------------------------------------------
// Pure-logic draft state machine (unit-tested in tests/test_draft.cpp).
// ---------------------------------------------------------------------------
struct DraftModel {
    uint32_t generation = 0;
    bool     active     = false;   // true while picking is in progress
    bool     decided    = false;   // true once a draft completed for this run
    uint8_t  count      = 0;       // 0..kMaxDraftCats
    uint8_t  peer_count = 0;       // 0..kMaxPeers
    uint64_t cat_uid[kMaxDraftCats] = {};
    uint8_t  owner[kMaxDraftCats]   = {};  // kNoPeer if unclaimed
    uint8_t  peer_ids[kMaxPeers]    = {};
    uint8_t  peer_quota[kMaxPeers]  = {};

    void clear() {
        active     = false;
        decided    = false;
        count      = 0;
        peer_count = 0;
        for (uint32_t i = 0; i < kMaxDraftCats; ++i) {
            cat_uid[i] = 0;
            owner[i]   = kNoPeer;
        }
        for (uint32_t i = 0; i < kMaxPeers; ++i) {
            peer_ids[i]   = kNoPeer;
            peer_quota[i] = 0;
        }
    }

    // Start a fresh draft with sorted peer_ids (host first) and party cat_uids.
    bool start(uint32_t gen, const uint64_t* uids, uint8_t cat_n,
               const uint8_t* peers, uint8_t peer_n) {
        clear();
        if (!uids || !peers || cat_n == 0 || peer_n < 2) return false;
        if (cat_n > kMaxDraftCats) cat_n = (uint8_t)kMaxDraftCats;
        if (peer_n > kMaxPeers)    peer_n = (uint8_t)kMaxPeers;

        generation = gen;
        active     = true;
        decided    = false;
        count      = cat_n;
        peer_count = peer_n;
        for (uint8_t i = 0; i < count; ++i) {
            cat_uid[i] = uids[i];
            owner[i]   = kNoPeer;
        }
        for (uint8_t p = 0; p < peer_count; ++p) {
            peer_ids[p]   = peers[p];
            peer_quota[p] = (uint8_t)split_for(count, peer_count, p).count;
        }
        return true;
    }

    int find_cat(uint64_t uid) const {
        for (uint8_t i = 0; i < count; ++i)
            if (cat_uid[i] == uid) return (int)i;
        return -1;
    }

    int find_peer(uint8_t peer) const {
        for (uint8_t i = 0; i < peer_count; ++i)
            if (peer_ids[i] == peer) return (int)i;
        return -1;
    }

    uint8_t quota_of(uint8_t peer) const {
        const int idx = find_peer(peer);
        return idx >= 0 ? peer_quota[idx] : 0;
    }

    uint8_t picked_of(uint8_t peer) const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < count; ++i)
            if (owner[i] == peer) ++n;
        return n;
    }

    uint8_t total_quota() const {
        uint8_t sum = 0;
        for (uint8_t i = 0; i < peer_count; ++i) sum = (uint8_t)(sum + peer_quota[i]);
        return sum < count ? sum : count;
    }

    uint8_t total_picked() const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < count; ++i)
            if (owner[i] != kNoPeer) ++n;
        return n;
    }

    bool is_complete() const {
        if (!active) return decided;
        return total_picked() >= total_quota();
    }

    // Host-authoritative toggle:
    // - Clicking your own claimed cat unpicks it while draft is active.
    // - Clicking an unclaimed cat claims it if you still have quota left.
    // - Clicking another player's cat is rejected (first-come, first-served).
    // Returns true if state changed. Automatically finalizes when complete.
    bool toggle_pick(uint8_t peer, uint64_t uid) {
        if (!active || peer == kNoPeer) return false;
        const int ci = find_cat(uid);
        if (ci < 0) return false;
        const int pi = find_peer(peer);
        if (pi < 0) return false;

        if (owner[ci] == peer) {
            owner[ci] = kNoPeer;
            return true;
        }
        if (owner[ci] != kNoPeer) {
            return false; // already taken by someone else
        }
        if (picked_of(peer) >= peer_quota[pi]) {
            return false; // peer's quota is full
        }

        owner[ci] = peer;
        if (total_picked() >= total_quota()) {
            active  = false;
            decided = true;
        }
        return true;
    }

    // Handle a peer disconnecting mid-draft: release their picks and recompute
    // quotas for the remaining peers. If fewer than 2 peers remain, cancel
    // draft cleanly so the host is never blocked.
    bool on_peer_left(uint8_t peer) {
        if (!active) return false;
        const int pi = find_peer(peer);
        if (pi < 0) return false;

        for (uint8_t i = 0; i < count; ++i)
            if (owner[i] == peer) owner[i] = kNoPeer;

        for (uint8_t i = (uint8_t)pi; i + 1 < peer_count; ++i)
            peer_ids[i] = peer_ids[i + 1];
        --peer_count;

        if (peer_count < 2) {
            active  = false;
            decided = false;
            return true;
        }

        for (uint8_t p = 0; p < peer_count; ++p)
            peer_quota[p] = (uint8_t)split_for(count, peer_count, p).count;

        // If any remaining peer now holds more than their new quota, trim tail.
        for (uint8_t p = 0; p < peer_count; ++p) {
            uint8_t kept = 0;
            for (uint8_t i = 0; i < count; ++i) {
                if (owner[i] == peer_ids[p]) {
                    if (kept < peer_quota[p]) ++kept;
                    else owner[i] = kNoPeer;
                }
            }
        }

        if (total_picked() >= total_quota()) {
            active  = false;
            decided = true;
        }
        return true;
    }

    bool lookup_owner(uint64_t uid, uint8_t* out_peer) const {
        if (!decided) return false;
        const int ci = find_cat(uid);
        if (ci < 0 || owner[ci] == kNoPeer) return false;
        if (out_peer) *out_peer = owner[ci];
        return true;
    }

    DraftStateMsg to_msg() const {
        DraftStateMsg m{};
        m.generation = generation;
        m.active     = active ? 1 : 0;
        m.count      = count;
        m.peer_count = peer_count;
        for (uint8_t i = 0; i < count; ++i) {
            m.cat_uid[i] = cat_uid[i];
            m.owner[i]   = owner[i];
        }
        for (uint8_t i = 0; i < peer_count; ++i) {
            m.peer_ids[i]   = peer_ids[i];
            m.peer_quota[i] = peer_quota[i];
        }
        return m;
    }

    void apply_msg(const DraftStateMsg& m) {
        if (m.count > kMaxDraftCats || m.peer_count > kMaxPeers) return;
        generation = m.generation;
        active     = (m.active != 0);
        count      = m.count;
        peer_count = m.peer_count;
        for (uint8_t i = 0; i < count; ++i) {
            cat_uid[i] = m.cat_uid[i];
            owner[i]   = m.owner[i];
        }
        for (uint8_t i = 0; i < peer_count; ++i) {
            peer_ids[i]   = m.peer_ids[i];
            peer_quota[i] = m.peer_quota[i];
        }
        decided = (!active && count > 0 && total_picked() > 0);
    }
};

// ---------------------------------------------------------------------------
// Session lifecycle & runtime API (game thread).
// ---------------------------------------------------------------------------

void draft_init();
void draft_shutdown();

// Clear remembered ownership & close any open draft overlay (e.g. on
// ReturnToHouse, new adventure, or session reset).
void draft_reset();

// Host: true when a node click should pause and open the cat ownership draft
// instead of entering the node immediately.
bool draft_needs_selection();

// True while the draft overlay is currently active.
bool draft_is_active();

// Host: begin the cat ownership draft and hold `node_index` until complete.
// Returns true if the draft started (caller should hold node entry); false if
// unable to start (e.g. <2 peers, <2 cats, or native UI unavailable), in which
// case the caller should proceed into the node normally.
bool draft_begin_host(uint32_t node_index);

// Host: if a previously held node is now ready to enter (draft just finished
// or was cancelled by disconnect), consumes it and writes *out_node_index.
bool draft_take_ready_node(uint32_t* out_node_index);

// Called from MapScreen::update (and safe game-thread ticks) to drive the
// native UI overlay, hover feedback, and click input.
void draft_tick();

// Incoming network handlers (dispatched from lockstep_pump).
void draft_on_pick(uint8_t from_peer, const DraftPickMsg& m);
void draft_on_state(const DraftStateMsg& m);

// Host: send current draft/assignment state to a newly joined peer.
void draft_catchup(uint8_t peer);

// Host: handle a peer disconnecting.
void draft_on_peer_disconnected(uint8_t peer);

// Query remembered cat ownership for this adventure run.
// Returns true and writes *out_peer ONLY if a draft completed, `cat_uid` has
// an owner, AND that owner is still currently connected in the session.
bool draft_lookup_owner(uint64_t cat_uid, uint8_t* out_peer);

} // namespace mgmp
