#include "mgmp_catsync.h"

#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_bytestream.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_lockstep.h"   // lockstep_in_battle

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mgmp {
namespace {

// The ByteStream's layout now lives in mgmp_bytestream.h -- mgmp_runhist drives
// the same struct through a different serializer, and one copy of an offset
// table is the only number of copies worth having.

typedef void  (__fastcall* fn_serialize_cat)(void* cat, void* stream, bool flag);
typedef void  (__fastcall* fn_bs_dtor)(void* stream);
typedef void* (__fastcall* fn_ofstream_ctor)(void* self);
typedef void* (__fastcall* fn_cat_by_id)(void* registry, uint64_t id);

struct State {
    uintptr_t base = 0;
    bool      resolved = false;   // every call target verified

    fn_serialize_cat serialize = nullptr;
    fn_bs_dtor       bs_dtor   = nullptr;
    fn_ofstream_ctor of_ctor   = nullptr;
    fn_cat_by_id     by_id     = nullptr;
    const void**     director_slot = nullptr;

    bool on        = false;
    bool is_client = false;
    bool announced = false;
    // One line per battle, not one per cat: the host pushes the whole run on a
    // reconnect and this would otherwise be a dozen identical lines.
    bool declined_in_battle = false;
    bool held_in_battle     = false;   // same, for the deferring path

    // What we last sent per cat, so an unchanged cat costs nothing on the wire.
    // Flat and small: a run is a few dozen cats, and a linear scan of 64 u64s is
    // not worth a hash map on a path that runs once per map node.
    static constexpr uint32_t kMaxCats = 64;
    uint64_t sent_id[kMaxCats]   = {};
    uint64_t sent_hash[kMaxCats] = {};
    uint32_t sent_count = 0;

    // Cats that arrived at a moment this peer must not write them, held until
    // the map-follow tick. Coalesced by id -- a CATDATA is whole state, not a
    // delta, so a newer one for the same cat makes the older one worthless.
    uint64_t pend_id[kMaxCats]   = {};
    uint64_t pend_hash[kMaxCats] = {};
    uint8_t* pend_data[kMaxCats] = {};
    uint32_t pend_size[kMaxCats] = {};
    uint32_t pend_count = 0;

    uint32_t pushed = 0, applied = 0, skipped = 0, deferred = 0, coalesced = 0;
};

State g;

// --- SEH shims --------------------------------------------------------------
//
// Every one of these calls into the game with a pointer whose meaning is
// recovered rather than declared. The project's rule is that a wrong guess
// produces a bad log line and never a crash in the game process, so each raw
// call lives in its own function with no C++ objects in scope and a __try around
// it. They return false instead of propagating.

bool safe_ofstream_ctor(void* p) {
    __try { g.of_ctor(p); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_bs_dtor(void* p) {
    __try { g.bs_dtor(p); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_serialize(void* cat, void* stream) {
    // `true` at every observed call site (GlobalProgressionData, the two save
    // paths, the winning-teams writer); none passes false.
    __try { g.serialize(cat, stream, true); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_by_id(void* registry, uint64_t id, void** out) {
    __try { *out = g.by_id(registry, id); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// --- stream lifecycle -------------------------------------------------------

bool bs_construct(uint8_t* bs) {
    memset(bs, 0, kByteStreamSize);
    if (!safe_ofstream_ctor(bs + kBS_Ofstream)) return false;
    // Both endian words stay 0, so read() compares 0 != 0 and never byte-swaps.
    // This is the element-size cap that guards the swap it will not do.
    *(uint32_t*)(bs + kBS_SwapElem) = 8;
    return true;
}

// --- the run's cats ---------------------------------------------------------
//
// director+1432 is the id -> CatData* registry, +1468/+1472 the run's id list
// (count, data). All three read off sub_1403B2060, which is what the inventory
// screen itself walks. We read the ids directly rather than calling that
// function because it allocates a CustomVector we would only free again.
struct RunCats {
    const void* registry = nullptr;
    const uint64_t* ids  = nullptr;
    uint32_t count       = 0;
};

bool run_cats(RunCats& out) {
    if (!g.director_slot) return false;
    const void* dir = nullptr;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir) return false;

    const uint8_t* d = (const uint8_t*)dir;
    if (!mem_read(d + kDir_CatRegistry, &out.registry, sizeof(out.registry))) return false;
    if (!mem_read(d + kDir_CatIdCount,  &out.count,    sizeof(out.count)))    return false;
    if (!mem_read(d + kDir_CatIdData,   &out.ids,      sizeof(out.ids)))      return false;

    // Refuse anything implausible rather than walking it. A run has a few dozen
    // cats; a five-digit count means the offsets moved and we are reading some
    // other object, which is exactly when NOT to start calling game functions
    // with the results.
    if (!out.registry || !out.ids) return false;
    if (out.count == 0 || out.count > 4096) return false;
    return true;
}

uint64_t fnv1a(const void* p, uint32_t n) {
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

// Serializes one cat into a caller-owned buffer. Returns 0 on any failure.
// `out` must be freed with free() by the caller.
uint32_t serialize_cat(void* cat, uint8_t** out) {
    *out = nullptr;
    uint8_t bs[kByteStreamSize];
    if (!bs_construct(bs)) return 0;

    *(uint32_t*)(bs + kBS_Mode) = 1;             // write, growing the buffer
    uint32_t n = 0;
    if (safe_serialize(cat, bs)) {
        const void* buf = *(const void**)(bs + kBS_WriteBuf);
        uint32_t    len = *(uint32_t*)(bs + kBS_WriteLen);
        if (buf && len && len <= kMaxCatBytes) {
            uint8_t* copy = (uint8_t*)malloc(len);
            // Copy out before the destructor runs: the buffer belongs to the
            // GAME's heap and the destructor frees it there.
            if (copy && mem_read(buf, copy, len)) { *out = copy; n = len; }
            else if (copy) free(copy);
        }
    }
    safe_bs_dtor(bs);
    return n;
}

uint64_t* remembered(uint64_t id) {
    for (uint32_t i = 0; i < g.sent_count; ++i)
        if (g.sent_id[i] == id) return &g.sent_hash[i];
    if (g.sent_count >= State::kMaxCats) return nullptr;
    g.sent_id[g.sent_count] = id;
    g.sent_hash[g.sent_count] = 0;
    return &g.sent_hash[g.sent_count++];
}

// Read from the CONFIG rather than the live session, for the same reason
// mgmp_savefile does: the host publishes at its first map node, which can be
// before a peer has finished connecting.
//
// NOT LATCHED -- see the note on mgmp_savefile's ensure_state. The panel's
// connect buttons can set the role after this has already run once, and a
// latched "off" is a module that never arms for the rest of the process.
void ensure_state() {
    const Config& c = config();
    bool host   = _stricmp(c.net_role, "host")   == 0;
    bool client = _stricmp(c.net_role, "client") == 0;
    g.is_client = client;
    g.on = tune::kCatSync && (host || client) && g.resolved;

    static bool said = false;
    if (!g.on && tune::kCatSync && (host || client) && !said) {
        said = true;
        log_line("CATSYNC", "!! disabled: the game functions it calls did not "
                            "verify against this build");
    }
}

} // namespace

// Defined with the rest of the hold machinery, below the publish path; declared
// here because catsync_shutdown drains the hold and is written above it.
static void free_pending();

// ---------------------------------------------------------------------------

void catsync_set_base(uintptr_t base) {
    g.base = base;
    g.resolved = false;

    // Only the four this module calls. It used to walk all of C_COUNT, which
    // meant drift in an address belonging to some OTHER module silently turned
    // CAT SYNC off -- the wrong feature, named in the wrong log line. Same
    // convention mgmp_invsync already states.
    static const int kNeed[] = { C_SerializeCatData, C_ByteStreamDtor,
                                 C_OfstreamCtor,     C_CatDataById };
    for (int idx : kNeed) {
        if (!addr_of_call((Call)idx)) {
            log_line("CATSYNC", "!! %s did not resolve by signature "
                                "-- cat sync is OFF", kCalls[idx].name);
            return;
        }
    }

    g.serialize     = (fn_serialize_cat)addr_of_call(C_SerializeCatData);
    g.bs_dtor       = (fn_bs_dtor)      addr_of_call(C_ByteStreamDtor);
    g.of_ctor       = (fn_ofstream_ctor)addr_of_call(C_OfstreamCtor);
    g.by_id         = (fn_cat_by_id)    addr_of_call(C_CatDataById);
    g.director_slot = (const void**)    addr_of_data(D_MewDirectorPtr);
    if (!g.director_slot) {
        log_line("CATSYNC", "!! the MewDirector* global did not resolve -- cat sync is OFF");
        return;
    }
    g.resolved = true;
}

void catsync_init() {
    ensure_state();
    if (!g.on || g.announced) return;
    g.announced = true;
    log_line_lvl(LogLevel::Trace, "CATSYNC", "armed -- %s",
             g.is_client ? "cats will be overwritten with the host's"
                         : "this peer's cats will be published to the client");
}

void catsync_shutdown() {
    const uint32_t stranded = g.pend_count;
    free_pending();
    if (!g.announced) return;
    // `stranded` is the drop this module is allowed to make, so it says so:
    // a cat held for a map tick that never came is a cat this peer never got.
    // It is also the ONLY thing in this line that means something went wrong,
    // which is why it and not the line decides the severity.
    log_line_lvl(stranded ? LogLevel::Warn : LogLevel::Trace, "CATSYNC",
             "done: %u pushed, %u applied, %u unchanged, "
             "%u held (%u superseded), %u still held at exit",
             g.pushed, g.applied, g.skipped, g.deferred, g.coalesced, stranded);
}

// Drop the "already sent this" cache, so the next publish sends every cat
// whatever its bytes are. For a peer that RECONNECTS: the dedupe is per-run,
// not per-peer, so without this the next node's publish tells a freshly
// arrived peer about only the cats that happened to change since the last
// node -- and it has none of the others.
void catsync_forget() {
    ensure_state();
    if (!g.on || g.is_client) return;
    g.sent_count = 0;
    memset(g.sent_id,   0, sizeof(g.sent_id));
    memset(g.sent_hash, 0, sizeof(g.sent_hash));
}

void catsync_publish(const char* why) {
    ensure_state();
    if (!g.on || g.is_client || !net_active()) return;

    RunCats rc{};
    if (!run_cats(rc)) {
        log_line("CATSYNC", "!! could not read the run's cat list -- nothing "
                            "published (%s)", why);
        return;
    }

    uint32_t sent = 0, same = 0, failed = 0;
    for (uint32_t i = 0; i < rc.count; ++i) {
        uint64_t id = 0;
        if (!mem_read(&rc.ids[i], &id, sizeof(id)) || !id) continue;

        void* cat = nullptr;
        if (!safe_by_id((void*)rc.registry, id, &cat) || !cat) { ++failed; continue; }

        uint8_t* bytes = nullptr;
        uint32_t n = serialize_cat(cat, &bytes);
        if (!n) { ++failed; continue; }

        CatDataMsg m{};
        m.id   = id;
        m.size = n;
        m.hash = fnv1a(bytes, n);
        m.data = bytes;

        uint64_t* prev = remembered(id);
        if (prev && *prev == m.hash) { ++same; free(bytes); continue; }

        if (net_send_catdata(m)) {
            if (prev) *prev = m.hash;
            ++sent;
        } else {
            ++failed;
        }
        free(bytes);
    }

    g.pushed  += sent;
    g.skipped += same;
    if (sent || failed)
        log_line("CATSYNC", "-> %u cat(s) changed and sent, %u unchanged%s (%s)",
                 sent, same,
                 failed ? " -- SOME FAILED, see above" : "", why);
}

// --- applying, and holding until it is safe to apply ------------------------

static void apply_now(const CatDataMsg& m, const char* when) {
    if (fnv1a(m.data, m.size) != m.hash) {
        log_line("CATSYNC", "!! cat %016llx failed its hash -- not applied",
                 (unsigned long long)m.id);
        return;
    }

    RunCats rc{};
    if (!run_cats(rc)) {
        // Before a save is loaded there is no run to write into. Not an error:
        // the host may publish while this peer is still on the menu.
        return;
    }

    void* cat = nullptr;
    if (!safe_by_id((void*)rc.registry, m.id, &cat) || !cat) {
        log_line("CATSYNC", "!! cat %016llx is not in this peer's run -- not "
                            "applied. The two runs are not the same run.",
                 (unsigned long long)m.id);
        return;
    }

    uint8_t bs[kByteStreamSize];
    if (!bs_construct(bs)) return;

    *(uint32_t*)(bs + kBS_Mode)    = 0;              // read
    *(const void**)(bs + kBS_ReadBuf) = m.data;      // BORROWED, see kBS_ReadOwns
    *(uint8_t*)(bs + kBS_ReadOwns) = 0;              // ...so the game must not free it
    *(uint32_t*)(bs + kBS_ReadLen) = m.size;
    *(uint32_t*)(bs + kBS_ReadPos) = 0;

    bool ok = safe_serialize(cat, bs);
    safe_bs_dtor(bs);

    if (ok) {
        ++g.applied;
        log_line("CATSYNC", "<- applied cat %016llx (%u bytes, %s)",
                 (unsigned long long)m.id, m.size, when);
    } else {
        log_line("CATSYNC", "!! deserializing cat %016llx faulted -- this peer's "
                            "copy of that cat is now UNTRUSTWORTHY",
                 (unsigned long long)m.id);
    }
}

static void free_pending() {
    for (uint32_t i = 0; i < g.pend_count; ++i) {
        free(g.pend_data[i]);
        g.pend_data[i] = nullptr;
        g.pend_size[i] = 0;
    }
    g.pend_count = 0;
}

static bool stash_pending(const CatDataMsg& m) {
    uint8_t* copy = (uint8_t*)malloc(m.size);
    if (!copy) return false;
    memcpy(copy, m.data, m.size);

    for (uint32_t i = 0; i < g.pend_count; ++i) {
        if (g.pend_id[i] != m.id) continue;
        free(g.pend_data[i]);
        g.pend_data[i] = copy;
        g.pend_size[i] = m.size;
        g.pend_hash[i] = m.hash;
        ++g.coalesced;
        return true;
    }
    if (g.pend_count >= State::kMaxCats) { free(copy); return false; }

    g.pend_id[g.pend_count]   = m.id;
    g.pend_hash[g.pend_count] = m.hash;
    g.pend_data[g.pend_count] = copy;
    g.pend_size[g.pend_count] = m.size;
    ++g.pend_count;
    return true;
}

// Deferral is only correct while something is guaranteed to drain the queue,
// and that something is the client's map-follow tick -- the same rule and the
// same helper shape as mgmp_invsync.
static bool defer_applies() { return g.is_client && config().net_follow; }

void catsync_apply_pending(const char* why) {
    ensure_state();
    if (!g.on || !g.pend_count) return;

    const uint32_t n = g.pend_count;
    for (uint32_t i = 0; i < n; ++i) {
        CatDataMsg m{};
        m.id   = g.pend_id[i];
        m.hash = g.pend_hash[i];
        m.size = g.pend_size[i];
        m.data = g.pend_data[i];
        apply_now(m, why);
    }
    free_pending();
    g.held_in_battle = false;   // re-arms the line for the next battle
}

void catsync_on_message(const CatDataMsg& m) {
    ensure_state();
    if (!g.on) return;
    if (!g.is_client) {
        // The host is authoritative; a cat arriving here means both peers think
        // they own the run, which is a configuration error worth naming rather
        // than a state to merge.
        log_line("CATSYNC", "!! received a cat from the peer while hosting -- "
                            "ignored (both peers configured as host?)");
        return;
    }
    if (!m.data || !m.size) return;

    // NOT WHILE A BATTLE THIS PEER IS ALREADY IN IS RUNNING -- but HELD, never
    // dropped. The difference cost a whole run on 2026-08-26 and is the reason
    // this branch is written twice over.
    //
    // Measured 2026-08-26: a client that dropped its socket mid-fight and
    // pressed `join` had four cats pushed at it by the host's reconnect
    // catch-up and applied them on the spot -- and a cat visibly gained a
    // shield with no action behind it. The catch-up is right to send them (a
    // peer whose process restarted has no run at all and needs every one), but
    // applying them into a live fight writes cat state that the battle's
    // Character objects derive from, outside the deterministic stream the
    // per-turn hash covers. It is the "is CatData written during a battle"
    // question this project wrote down as the first thing to check -- and the
    // answer turned out to be that WE write it.
    //
    // A peer that only lost its socket already has the right cats: it never
    // left the run. Declining is the same rule mgmp_savefile already applies
    // for the same reason -- see savefile_on_message refusing once `applied` is
    // set. A genuinely fresh joiner has no snapshot, so this does not fire for
    // the mid-fight join case that needs the data.
    //
    // What the original version got wrong is what happens NEXT. It returned,
    // and the host dedupes against the last hash it sent, so the push was gone
    // for the rest of the run. Worse, lockstep_in_battle() is "a roster has
    // been snapshotted and not yet replaced", which stays true on the map
    // screen BETWEEN battles -- so from the first fight onwards this peer
    // refused every ordinary per-node cat push. Measured 2026-08-26: 0 cats
    // applied over 22 turns and three map nodes, the two runs forked, and the
    // next battle opened with a turn-0 state-only mismatch.
    //
    // So: hold, and apply at the map-follow tick, exactly where the inventory
    // already lands. That is still before EnterNode, so it is in time for the
    // battle or shop the node opens, and it is off the battle screen, so it
    // never writes into a live fight.
    if (!lockstep_in_battle()) {
        g.declined_in_battle = false;   // re-arms the line for the next battle
    } else if (defer_applies()) {
        if (!stash_pending(m)) {
            log_line("CATSYNC", "!! could not hold cat %016llx for the map tick "
                                "(%u already held) -- applying it here instead, "
                                "into a live battle",
                     (unsigned long long)m.id, g.pend_count);
            apply_now(m, "on arrival, deferral failed");
            return;
        }
        ++g.deferred;
        if (!g.held_in_battle) {
            g.held_in_battle = true;
            log_line("CATSYNC", "holding cat pushes while a battle is in progress "
                                "-- they will be applied on the map, just before "
                                "this peer enters the next node");
        }
        return;
    } else {
        // net_follow off: nothing would ever drain the queue, so the old
        // behaviour is all that is available. This peer is driving its own map.
        if (!g.declined_in_battle) {
            g.declined_in_battle = true;
            log_line("CATSYNC", "declining cat pushes while a battle is in progress "
                                "-- net_follow is off, so nothing would ever apply "
                                "a held one. This peer's cats will DRIFT.");
        }
        return;
    }

    apply_now(m, "on arrival");
}

} // namespace mgmp
