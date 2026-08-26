// mgmp_nodehash -- see mgmp_nodehash.h for why the meta layer needs its own
// hash and why it reports rather than halts.
#include "mgmp_nodehash.h"

#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_follow.h"     // follow_here_seed
#include "mgmp_hashring.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_runhist.h"
#include "mgmp_rng.h"        // rng_global_stream

#include <windows.h>
#include <cstring>

namespace mgmp {
namespace {

// The ring keys on the PAIR (battle_id, turn). Node hashes key on the pair
// (node seed, sample point), which is the same shape and the same guarantee --
// so the tested ring is reused verbatim rather than reimplemented, with the two
// names carrying node meanings. `turn` holds kNodePointEnter/kNodePointEvent.
struct Entry {
    uint64_t    battle_id = 0;   // the node seed
    uint32_t    turn      = 0;   // the sample point
    NodeHashMsg m;
};

struct State {
    bool on   = false;
    bool halt = false;

    const void** director_slot = nullptr;

    HashRing<Entry> mine;        // what this peer sent, awaiting a counterpart
    HashRing<Entry> theirs;      // what arrived before we reached that point

    // The screen last sampled, so one event is measured once rather than every
    // frame its update runs.
    void* sampled_screen = nullptr;

    // ...and the screen we are WAITING on, with how many ticks we have waited.
    // WorldEvent+0x1A10 is read from disassembly and has never been exercised
    // live, so "the name is not there" has two causes -- init has not written
    // it yet (normal, a frame or two) and the offset is wrong (silent, and it
    // would remove half this module's value without a word). Waiting is fine;
    // waiting forever has to be said out loud.
    void*    waiting_screen = nullptr;
    uint32_t waiting_ticks  = 0;
    bool     waiting_warned = false;

    uint32_t sent = 0, agreed = 0, mismatched = 0;

    CRITICAL_SECTION cs;
    bool cs_ready = false;
};

State g;

struct Guard {
    Guard()  { if (g.cs_ready) EnterCriticalSection(&g.cs); }
    ~Guard() { if (g.cs_ready) LeaveCriticalSection(&g.cs); }
};

uint64_t fnv1a(const void* p, uint32_t n, uint64_t h = 0xCBF29CE484222325ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (uint32_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

const char* point_name(uint32_t p) {
    return p == kNodePointEvent ? "the event screen" : "node entry";
}

// --- the three things we hash besides the stream ----------------------------

const void* director() {
    if (!g.director_slot) return nullptr;
    const void* dir = nullptr;
    if (!mem_read(g.director_slot, &dir, sizeof(dir))) return nullptr;
    return dir;
}

// The run's cat ids, IN ORDER. Order is not incidental here: sub_1400AACD0
// takes one xoshiro step and uses it to index this list, so two peers whose
// lists are permutations of each other pick different cats from the same draw.
uint64_t cats_hash(uint32_t& count_out) {
    count_out = 0;
    const void* dir = director();
    if (!dir) return 0;
    const uint8_t* d = (const uint8_t*)dir;

    uint32_t count = 0;
    const uint64_t* ids = nullptr;
    if (!mem_read(d + kDir_CatIdCount, &count, sizeof(count))) return 0;
    if (!mem_read(d + kDir_CatIdData,  &ids,   sizeof(ids)))   return 0;
    // Refuse anything implausible rather than walking it -- a five-digit count
    // means we are reading some other object, and a hash of the wrong bytes is
    // a guaranteed false mismatch, which is strictly worse than no hash.
    if (!ids || count == 0 || count > 4096) return 0;

    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t id = 0;
        if (!mem_read(&ids[i], &id, sizeof(id))) return 0;
        h = fnv1a(&id, sizeof(id), h);
    }
    count_out = count;
    return h;
}

// Coins, food, boxes and the three bucket counts. No serializer and no blob:
// these are plain ints on the Inventory object, and the counts are the cheap
// half of what mgmp_invsync already pushes whole.
uint64_t inv_hash() {
    const void* dir = director();
    if (!dir) return 0;
    const void* inv = nullptr;
    if (!mem_read((const uint8_t*)dir + kDir_Inventory, &inv, sizeof(inv)) || !inv)
        return 0;
    const uint8_t* p = (const uint8_t*)inv;

    static const uintptr_t kScalars[] = { kInv_Coins, kInv_Food, kInv_Boxes };
    static const uintptr_t kBuckets[] = { kInv_Backpack, kInv_Storage, kInv_Trash };

    uint64_t h = 0xCBF29CE484222325ull;
    for (uintptr_t off : kScalars) {
        int32_t v = 0;
        if (!mem_read(p + off, &v, sizeof(v))) return 0;
        h = fnv1a(&v, sizeof(v), h);
    }
    for (uintptr_t bucket : kBuckets) {
        uint32_t n = 0;
        if (!mem_read(p + bucket + kBucket_Count, &n, sizeof(n))) return 0;
        h = fnv1a(&n, sizeof(n), h);
    }
    return h;
}

// MSVC's std::string: characters inline while capacity <= 15, behind a pointer
// otherwise. Same reader mgmp_choice uses, and defensive for the same reason --
// we are reading an offset recovered from disassembly.
bool read_game_string(const void* str, char* out, uint32_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!str) return false;
    uint64_t len = 0, capacity = 0;
    if (!mem_read((const uint8_t*)str + 16, &len, sizeof(len))) return false;
    if (!mem_read((const uint8_t*)str + 24, &capacity, sizeof(capacity))) return false;
    if (len > 4096) return false;
    const uint8_t* chars = (const uint8_t*)str;
    if (capacity > 15) {
        void* p = nullptr;
        if (!mem_read((const uint8_t*)str, &p, sizeof(p)) || !p) return false;
        chars = (const uint8_t*)p;
    }
    uint32_t n = (uint32_t)len;
    if (n > cap - 1) n = cap - 1;
    if (n && !mem_read(chars, out, n)) { out[0] = '\0'; return false; }
    out[n] = '\0';
    return true;
}

// --- comparison -------------------------------------------------------------

void report(uint8_t from, const NodeHashMsg& a, const NodeHashMsg& b) {
    const bool rng_same  = memcmp(a.rng, b.rng, sizeof(a.rng)) == 0;
    const bool hist_same = a.hist_hash == b.hist_hash;
    const bool cats_same = a.cats_hash == b.cats_hash && a.cat_count == b.cat_count;
    const bool inv_same  = a.inv_hash  == b.inv_hash;
    const bool evt_same  = strcmp(a.event, b.event) == 0;

    if (rng_same && hist_same && cats_same && inv_same && evt_same) {
        ++g.agreed;
        // NAME WHAT WAS NOT MEASURED. A component that could not be read is 0
        // on both peers and therefore "agrees" -- a vacuous pass, and exactly
        // the trap the battle layer's `0 desync(s)` with no AGREES behind it
        // fell into. An agreement over three fields and a blank is worth
        // knowing about, so the reader is told which fields carried a value.
        char missing[96] = {};
        if (!a.hist_hash) strcat_s(missing, " history");
        if (!a.cats_hash) strcat_s(missing, " cats");
        if (!a.inv_hash)  strcat_s(missing, " inventory");
        log_line("NODEHASH", "node %016llx at %s AGREES with peer %u "
                             "(rng+history+cats+inventory)%s%s",
                 (unsigned long long)a.node_seed, point_name(a.point),
                 (unsigned)from,
                 missing[0] ? " -- BUT NOT MEASURED:" : "", missing);
        return;
    }

    ++g.mismatched;
    log_line("NODEHASH", "!! MISMATCH at %s of node %u (%016llx) against peer %u",
             point_name(a.point), a.node_index,
             (unsigned long long)a.node_seed, (unsigned)from);

    // One line per differing component, with both values. The whole reason this
    // message carries five separate hashes instead of one is that "they differ"
    // is not a diagnosis -- which of them differs is.
    if (!rng_same)
        log_line("NODEHASH", "!!   rng       %016llx vs %016llx (first word)",
                 (unsigned long long)a.rng[0], (unsigned long long)b.rng[0]);
    if (!hist_same)
        log_line("NODEHASH", "!!   history   %016llx vs %016llx -- the used-event "
                             "list differs, so the peers will roll DIFFERENT EVENTS",
                 (unsigned long long)a.hist_hash, (unsigned long long)b.hist_hash);
    if (!cats_same)
        log_line("NODEHASH", "!!   cats      %016llx (%u) vs %016llx (%u) -- the "
                             "roster the event's subject cat is drawn from",
                 (unsigned long long)a.cats_hash, a.cat_count,
                 (unsigned long long)b.cats_hash, b.cat_count);
    if (!inv_same)
        log_line("NODEHASH", "!!   inventory %016llx vs %016llx",
                 (unsigned long long)a.inv_hash, (unsigned long long)b.inv_hash);
    if (!evt_same)
        log_line("NODEHASH", "!!   EVENT     '%s' here but '%s' on peer %u -- the two "
                             "peers are looking at different events",
                 a.event, b.event, (unsigned)from);

    if (g.halt)
        log_line("NODEHASH", "!! net_nodehash_halt is set -- treat this run as over; "
                             "the meta layer has diverged");
}

void match(uint8_t from, const NodeHashMsg& theirs) {
    Entry mine{};
    if (g.mine.take(theirs.node_seed, theirs.point, mine)) {
        report(from, mine.m, theirs);
        return;
    }
    // Their sample for a point we have not reached. Hold it -- the peers walk
    // the same nodes but not at the same moment, exactly as with turns.
    Entry e{};
    e.battle_id = theirs.node_seed;
    e.turn      = theirs.point;
    e.m         = theirs;
    if (!g.theirs.push_refusing(e))
        log_line("NODEHASH", "!! holding %u node hashes from peer %u already and "
                             "refusing more -- those nodes go unchecked",
                 kHashRing, (unsigned)from);
}

void publish(const NodeHashMsg& m) {
    Entry e{};
    e.battle_id = m.node_seed;
    e.turn      = m.point;
    e.m         = m;

    // Did the peer already send its counterpart? Then compare now rather than
    // storing something nothing will ever come for.
    Entry theirs{};
    if (g.theirs.take(m.node_seed, m.point, theirs)) {
        report(kNoPeer, m, theirs.m);
    } else {
        g.mine.push_evicting(e);
    }

    if (net_send_nodehash(m)) ++g.sent;
}

// Fills everything except node_seed/node_index/point/event.
void fill(NodeHashMsg& m) {
    if (const uint64_t* s = rng_global_stream())
        for (int i = 0; i < 4; ++i) m.rng[i] = s[i];
    m.hist_hash = runhist_hash();
    m.cats_hash = cats_hash(m.cat_count);
    m.inv_hash  = inv_hash();
}

} // namespace

// ---------------------------------------------------------------------------

void nodehash_set_base(uintptr_t base) {
    g.director_slot = (const void**)addr_of_data(D_MewDirectorPtr);
}

void nodehash_init() {
    if (!g.cs_ready) { InitializeCriticalSection(&g.cs); g.cs_ready = true; }
    const Config& c = config();
    g.on   = tune::kNodeHash;
    g.halt = tune::kNodeHashHalt;
    g.mine.clear();
    g.theirs.clear();
    g.sampled_screen = nullptr;
    if (!g.on) { log_line("NODEHASH", "meta-layer hashing disabled by net_nodehash = 0"); return; }
    log_line("NODEHASH", "armed -- hashing rng, run history, cat roster and "
                         "inventory at every map node%s",
             g.halt ? "; a mismatch will be treated as fatal" : "");
}

void nodehash_shutdown() {
    if (!g.on) return;
    // Say when nothing was compared. `0 mismatch(es)` on its own reads as a
    // pass and would not be one -- the same trap the battle layer's
    // `0 desync(s)` fell into after a reconnect, where the number was empty
    // rather than clean.
    if (!g.agreed && !g.mismatched)
        log_line("NODEHASH", "done: %u sent and NOTHING WAS EVER COMPARED -- no peer "
                             "reached the same node, so this run says nothing about "
                             "whether the meta layer stayed in sync", g.sent);
    else
        log_line("NODEHASH", "done: %u sent, %u agreed, %u mismatched",
                 g.sent, g.agreed, g.mismatched);
}

void nodehash_on_node(uint64_t node_seed, uint32_t node_index) {
    if (!g.on || !net_active() || !node_seed) return;
    Guard guard;

    // A new node means the previous node's event screen is gone.
    g.sampled_screen = nullptr;
    g.waiting_screen = nullptr;

    NodeHashMsg m{};
    m.node_seed  = node_seed;
    m.node_index = node_index;
    m.point      = kNodePointEnter;
    fill(m);
    publish(m);
}

void nodehash_on_event_screen(void* world_event) {
    if (!g.on || !net_active() || !world_event) return;
    Guard guard;
    if (g.sampled_screen == world_event) return;      // once per screen

    // The name is the whole point of this sample, so do not take the sample
    // until it can be read: WorldEvent::init writes it, and update can tick
    // before that. An empty name is "not ready yet", not "no event".
    char name[48] = {};
    if (!read_game_string((const uint8_t*)world_event + kEvt_EventName,
                          name, sizeof(name)) || !name[0]) {
        if (g.waiting_screen != world_event) {
            g.waiting_screen = world_event;
            g.waiting_ticks  = 0;
            g.waiting_warned = false;
        }
        // Roughly two seconds at 60 fps. An event screen that has ticked this
        // long without a readable name is not "not ready yet".
        if (++g.waiting_ticks > 120 && !g.waiting_warned) {
            g.waiting_warned = true;
            log_line("NODEHASH", "!! an event screen has ticked %u times and "
                                 "WorldEvent+0x%X still holds no readable name -- "
                                 "the event sample point is NOT WORKING on this "
                                 "build and only the node-entry samples are being "
                                 "taken. The offset is the first suspect.",
                     g.waiting_ticks, (unsigned)kEvt_EventName);
        }
        return;
    }

    g.waiting_screen = nullptr;
    g.sampled_screen = world_event;

    NodeHashMsg m{};
    // follow_here_seed() is deliberately lock-free, so taking it while holding
    // our own lock cannot invert against mgmp_follow. See its definition.
    m.node_seed  = follow_here_seed();
    m.node_index = 0;
    m.point      = kNodePointEvent;
    memcpy(m.event, name, sizeof(m.event));
    m.event[sizeof(m.event) - 1] = '\0';
    if (!m.node_seed) return;                         // not in a known node
    fill(m);

    // Say which event this peer is looking at, unconditionally and before any
    // comparison. The AGREES/MISMATCH line only appears once the peer's
    // counterpart arrives, and the whole reported bug is two peers looking at
    // different events -- so the name has to be in BOTH logs whether or not the
    // two halves ever meet.
    log_line("NODEHASH", "event on node %016llx is '%s'",
             (unsigned long long)m.node_seed, m.event);

    publish(m);
}

void nodehash_on_message(uint8_t from, const NodeHashMsg& m) {
    if (!g.on) return;
    Guard guard;
    match(from, m);
}

} // namespace mgmp
