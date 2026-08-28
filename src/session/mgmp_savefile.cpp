// mgmp_savefile.cpp -- see mgmp_savefile.h.

#include "mgmp_savefile.h"
#include "mgmp_net.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_follow.h"   // follow_on_map
// A slot click starts a run, which makes the three per-run dedupe caches stale.
// See the block in savefile_on_slot_click.
#include "mgmp_catsync.h"
#include "mgmp_invsync.h"
#include "mgmp_runhist.h"

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mgmp {
namespace {

// SaveSelection+0x38 is a std::vector<std::string>: { begin@0x38, end@0x40,
// cap_end@0x48 }, stride 32. Read three ways that agree --
//   * SaveSelection::init assigns three elements into it via the vector
//     assign helper at 0x1400EC700 (rcx = this+0x38, r8d = 3), once for the
//     campaignNN.sav array and once for steamcampaignNN.sav;
//   * init then computes the element count as (end - begin) >> 5 at 0x1401B9A07;
//   * the ContinueFile transition lambda indexes it as
//     `(slot << 5) + [this+0x38]` and hands the result to MewDirector::init.
constexpr uintptr_t kSaveSel_NamesBegin = 0x38;
constexpr uintptr_t kSaveSel_NamesEnd   = 0x40;
constexpr size_t    kStdStringSize      = 32;

// More than the three the screen shows, so a build with more slots is read
// rather than silently truncated.
constexpr uint32_t kMaxSlots = 8;

struct Names {
    uint32_t count = 0;
    char     v[kMaxSlots][64] = {};
};


bool read_slot_names(const void* ss, Names& out) {
    out.count = 0;
    if (!ss) return false;
    const uint8_t* begin = nullptr;
    const uint8_t* end   = nullptr;
    if (!mem_read((const uint8_t*)ss + kSaveSel_NamesBegin, &begin, sizeof(begin))) return false;
    if (!mem_read((const uint8_t*)ss + kSaveSel_NamesEnd,   &end,   sizeof(end)))   return false;
    if (!begin || end < begin) return false;

    size_t bytes = (size_t)(end - begin);
    if (bytes % kStdStringSize) return false;              // not this vector
    size_t n = bytes / kStdStringSize;
    if (n == 0 || n > kMaxSlots) return false;

    for (size_t i = 0; i < n; ++i) {
        if (!mem_read_std_string(begin + i * kStdStringSize, out.v[i], sizeof(out.v[i])))
            return false;
        // Every shipped slot name ends in .sav. Checking it is what stops a
        // wrong offset from being mistaken for a valid roster of file names --
        // the same "gate on evidence, not on the offset being right" rule the
        // per-turn state hash follows.
        size_t len = strlen(out.v[i]);
        if (len < 5 || _stricmp(out.v[i] + len - 4, ".sav") != 0) return false;
    }
    out.count = (uint32_t)n;
    return true;
}

// --- the save directory ----------------------------------------------------
//
// <roaming appdata>\Glaiel Games\Mewgenics\<account id>\saves.
//
// TWO HALVES, RESOLVED TWO DIFFERENT WAYS, BOTH DELIBERATE.
//
// The root comes from SHGetFolderPathW(CSIDL_APPDATA) because that is the call
// the GAME makes: it reaches it through SDL_GetPrefPath, and the binary
// contains no "APPDATA" string anywhere -- so the environment variable is a
// proxy for the answer, not the answer. They agree on an ordinary desktop and
// diverge exactly where it would be hardest to debug (a launcher that overrides
// the variable, a roaming profile, a service context). Matching the game's own
// API means we cannot write a save into a directory the game will never read.
// The variable is kept only as a fallback for the case where the shell call
// fails outright.
//
// The account-id component is found by ENUMERATION rather than composed,
// because it is the Steam ID on a Steam install and we have no business
// guessing it. Exactly one such directory exists in practice; if several ever
// do, the most recently written one is the live account, and the choice is
// logged either way.
bool resolve_save_dir(wchar_t* out, size_t out_len) {
    out[0] = 0;
    wchar_t appdata[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr,
                                SHGFP_TYPE_CURRENT, appdata)) || !appdata[0]) {
        // Only if the shell call fails. Logged, because from here on we are no
        // longer guaranteed to be looking where the game looks.
        if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
            log_line("SAVEFILE", "!! neither SHGetFolderPath(CSIDL_APPDATA) nor "
                                 "%%APPDATA%% resolved -- cannot find the save directory");
            return false;
        }
        log_line("SAVEFILE", "!! SHGetFolderPath(CSIDL_APPDATA) failed; falling back "
                             "to %%APPDATA%%, which is not necessarily where the "
                             "game looks");
    }

    wchar_t root[MAX_PATH];
    _snwprintf_s(root, _TRUNCATE, L"%s\\Glaiel Games\\Mewgenics", appdata);

    wchar_t glob[MAX_PATH];
    _snwprintf_s(glob, _TRUNCATE, L"%s\\*", root);

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        log_line("SAVEFILE", "!! no Mewgenics app-data directory under %ls", root);
        return false;
    }

    wchar_t  best[MAX_PATH] = {};
    FILETIME best_time{};
    int      found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;

        wchar_t cand[MAX_PATH];
        _snwprintf_s(cand, _TRUNCATE, L"%s\\%s\\saves", root, fd.cFileName);
        DWORD attr = GetFileAttributesW(cand);
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;

        ++found;
        if (found == 1 || CompareFileTime(&fd.ftLastWriteTime, &best_time) > 0) {
            wcscpy_s(best, cand);
            best_time = fd.ftLastWriteTime;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (!found) {
        log_line("SAVEFILE", "!! found no <account>\\saves directory under %ls", root);
        return false;
    }
    if (found > 1)
        log_line("SAVEFILE", "%d account directories under %ls -- using the most "
                             "recently written one", found, root);
    wcscpy_s(out, out_len, best);
    return true;
}

// --- whole-file I/O --------------------------------------------------------

uint8_t* read_whole_file(const wchar_t* path, uint32_t& size_out, uint64_t& mtime_out) {
    size_out = 0; mtime_out = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;

    LARGE_INTEGER sz{};
    FILETIME      ft{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)kMaxSaveBytes) {
        CloseHandle(h);
        return nullptr;
    }
    if (GetFileTime(h, nullptr, nullptr, &ft))
        mtime_out = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

    uint32_t n = (uint32_t)sz.QuadPart;
    uint8_t* buf = (uint8_t*)malloc(n);
    if (!buf) { CloseHandle(h); return nullptr; }

    DWORD got = 0;
    bool ok = ReadFile(h, buf, n, &got, nullptr) && got == n;
    CloseHandle(h);
    if (!ok) { free(buf); return nullptr; }
    size_out = n;
    return buf;
}

bool write_whole_file(const wchar_t* path, const uint8_t* data, uint32_t n) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data, n, &put, nullptr) && put == n;
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

// The co-op file from a previous session is about to be replaced by this one.
// It is only ever a mirror of the host's save, so losing it costs nothing --
// but a timestamped copy is free, and the one time it matters is the time you
// wanted it.
bool back_up(const wchar_t* dir, const wchar_t* name) {
    wchar_t src[MAX_PATH];
    _snwprintf_s(src, _TRUNCATE, L"%s\\%s", dir, name);
    if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) return true;   // nothing there

    wchar_t bdir[MAX_PATH];
    _snwprintf_s(bdir, _TRUNCATE, L"%s\\mgmp_backups", dir);
    CreateDirectoryW(bdir, nullptr);

    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t dst[MAX_PATH];
    _snwprintf_s(dst, _TRUNCATE, L"%s\\%s.%04u%02u%02u-%02u%02u%02u.bak",
                 bdir, name, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

    if (!CopyFileW(src, dst, FALSE)) {
        log_line("SAVEFILE", "!! could not back up %ls (win32 %lu) -- NOT overwriting it",
                 name, GetLastError());
        return false;
    }
    log_line("SAVEFILE", "backed up the local %ls to mgmp_backups", name);
    return true;
}

// The file the client's copy of the host's run lives in.
//
// Deliberately not one of the game's own names, and deliberately short: at 13
// characters it fits MSVC's small-string optimisation, so the std::string image
// we hand to MewDirector::init is entirely self-contained -- 16 bytes of
// inline text, no heap pointer, nothing for the game's allocator to own or
// free. A longer name would need a heap buffer, and a callee that move-assigned
// from it would then try to free our memory with its allocator.
const char kCoopSaveName[] = "mgmp_coop.sav";
static_assert(sizeof(kCoopSaveName) <= 16, "must fit std::string's SSO buffer");

// MSVC std::string, the 32-byte layout already confirmed as the stride of
// SaveSelection's name vector: { union { char buf[16]; char* ptr; }, size, cap }.
struct StdStringImage {
    char     buf[16];
    uint64_t size;
    uint64_t cap;
};

// ---------------------------------------------------------------------------

struct State {
    bool on        = false;
    bool is_client = false;
    bool inited    = false;   // ensure_state has run, NOT "the handshake finished"
    bool announced = false;   // savefile_init printed the banner line

    wchar_t dir[MAX_PATH] = {};
    bool    have_dir  = false;
    bool    dir_tried = false;

    // --- host ---
    bool     have_slot   = false;
    uint32_t slot        = 0;
    char     name[64]    = {};
    uint64_t click_mtime = 0;     // the file's mtime when the host committed
    bool     published   = false;
    uint32_t publish_tries = 0;
    // sub_1403B9CE0(MewDirector*), resolved once and prologue-checked. Null
    // means "do not call it", which is a degraded publish (a stale checkpoint,
    // announced as such) rather than a disabled feature.
    void   (*save_adventure)(void*) = nullptr;
    const void** director_slot = nullptr;
    bool     said_no_flush = false;
    bool     said_mid_node = false;

    // --- client ---
    uint8_t* blob      = nullptr;
    uint32_t blob_size = 0;
    uint64_t blob_hash = 0;
    uint32_t peer_slot = 0;
    char     peer_name[64] = {};
    bool     pending   = false;   // a save arrived and has not been applied
    bool     applied   = false;   // we clicked; do not click again
    uint32_t refused   = 0;       // saves declined because we are already in the run
    uint32_t suppressed = 0;
    bool     said_waiting = false;

    bool     printed_names = false;   // the one-shot roster line

    // Armed just before the injected slot click and disarmed by the first
    // MewDirector::init that follows it. Single-shot on purpose: a load the
    // client starts any other way must not be redirected.
    bool           redirect_armed = false;
    StdStringImage redirect_name{};

    // --- pressing Play (see savefile_on_button_update) ---
    void   (*button_click)(void*, bool) = nullptr;
    uint32_t play_presses  = 0;
    uint32_t play_cooldown = 0;
    bool     said_gave_up  = false;
    bool     said_no_click = false;
};

// How hard to try. The first press is immediate; if the Play button is still
// ticking kPlayRetryFrames later then the transition did not start, so press
// again -- up to kPlayMaxPresses, and then stop and say so.
//
// A cap rather than an endless retry because the two reasons a press can fail
// need opposite responses from the player, and neither is helped by a loop:
// either the button refused (Button::Click's own guards), or the click landed
// and something further down did not. Both are worth one loud line and a
// human, not a machine pressing a button forever.
constexpr uint32_t kPlayRetryFrames = 120;   // ~2 s at 60 Hz
constexpr uint32_t kPlayMaxPresses  = 5;

State g;

void ensure_dir() {
    if (g.dir_tried) return;
    g.dir_tried = true;                 // one attempt, one log line, whatever happens
    g.have_dir = resolve_save_dir(g.dir, MAX_PATH);
    if (g.have_dir) log_line("SAVEFILE", "save directory: %ls", g.dir);
}

// Role and enablement come from the CONFIG, not from the session, and that is
// the whole point of this function.
//
// savefile_init() runs at go_ready -- i.e. when a peer has connected. But the
// host picks its save file whenever it feels like it, which is routinely BEFORE
// anyone has joined, and a client must have its own picks swallowed from the
// first frame the screen is up rather than from whenever the socket comes
// alive. Waiting for the handshake to arm this module would therefore miss the
// single event it exists to observe. config().net_role is already parsed by the
// time any menu exists, so it is the right source here.
// RECOMPUTED ON EVERY CALL, NOT LATCHED. It used to run once and keep the
// answer, which was correct while the role could only come from the file --
// and wrong the moment the panel's connect buttons could set it. A process
// launched with role = off called this from the first save-selection frame,
// latched g.on = false, and stayed off for the rest of its life however many
// sessions it went on to join. Two _stricmp per call is not worth a latch.
void ensure_state() {
    g.inited = true;
    const Config& cfg = config();
    bool host   = _stricmp(cfg.net_role, "host")   == 0;
    bool client = _stricmp(cfg.net_role, "client") == 0;
    g.is_client = client;
    // Unconditional: see mgmp_config.h. A peer in a session always shares the
    // save, because that is how the client gets the run.
    g.on        = (host || client);
}

void wide(const char* s, wchar_t* out, size_t n) {
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)n);
}

// Host: read the named file and put it on the wire. Returns false when there is
// nothing to send YET, which is not an error -- a brand-new game has no file on
// disk until the game writes one.
// `to` is kNoPeer for the initial broadcast, or a specific peer when catching a
// late joiner up.
// Make the file on disk equal to the host's LIVE run before we read it.
//
// Without this, publish() ships whatever the game last checkpointed, which for
// a peer joining mid-adventure is the wrong run state entirely -- measured on
// 2026-08-25, where a reconnect at map node 108 re-sent hash f5e46cf8 for a
// session that had started on 90d7c35c. The module already detected that (the
// click_mtime warning below) and shipped it anyway.
//
// sub_1403B9CE0 is write-through to the .sav: MewSaveFile::Store reaches
// `INSERT OR REPLACE INTO <table> VALUES (:key, :data);` through
// SQLSaveFile::SQL. It is what the game itself calls on every ReturnToMap, so
// this is the game's own save path on the game's own schedule, just asked for
// a moment early.
//
// Returns false when it could not be done, which is a reason to ANNOUNCE a
// stale transfer, not to refuse one: a stale save still beats no save for a
// client sitting on a suppressed selection screen.
// A NON-NULL MewDirector IS NOT A LOADED ADVENTURE, and assuming it was
// overwrote a player's save file.
//
// MewDirector is a singleton that exists from startup, so `director != nullptr`
// is true while the player is still sitting on the SAVE SELECTION SCREEN. The
// initial publish runs from exactly there -- savefile_pump fires on the slot
// click, before MewDirector::init has loaded anything -- so the flush called
// save_adventure on an EMPTY director and wrote its blank chapter_map,
// adventure_state and on_adventure straight over the slot the player had just
// chosen. The run then had a map it could neither enter nor advance past.
//
// Measured 2026-08-26 (host log 022438): CATSYNC said "could not read the run's
// cat list" at seq 21 -- there was demonstrably no run -- and SAVEFILE still
// reported "flushed the host's live run to disk first" at seq 35, twelve lines
// later, against the same director.
//
// The flush exists for the RECONNECT path, where an adventure really is loaded
// and the last checkpoint really is stale. It has no business running on the
// selection screen, where the file on disk is already exactly what we want to
// send: the run as the player last left it.
//
// So the test is not "is there a director" but "is there a RUN" -- the same
// question catsync asks with run_cats(), using the same pinned offsets. A slot
// click has no cat id list; a live adventure has a few dozen.
bool adventure_is_loaded_impl() {
    if (!g.director_slot) return false;
    const void* director = nullptr;
    if (!mem_read(g.director_slot, &director, sizeof(director)) || !director)
        return false;

    const uint8_t* d = (const uint8_t*)director;
    const void* registry = nullptr;
    const void* ids      = nullptr;
    uint32_t    count    = 0;
    if (!mem_read(d + kDir_CatRegistry, &registry, sizeof(registry))) return false;
    if (!mem_read(d + kDir_CatIdCount,  &count,    sizeof(count)))    return false;
    if (!mem_read(d + kDir_CatIdData,   &ids,      sizeof(ids)))      return false;

    // Same implausibility bound as run_cats: a five-digit count means the
    // offsets moved, and that is exactly when not to call a function that
    // writes the player's save file.
    if (!registry || !ids) return false;
    return count > 0 && count <= 4096;
}

bool flush_live_run() {
    if (!g.save_adventure || !g.director_slot) return false;
    if (!adventure_is_loaded_impl())
        return false;   // on the menu: the file on disk is already the right one

    // AND ONLY BETWEEN NODES. This is the one that actually cost a run.
    //
    // save_adventure is a NODE-BOUNDARY function: the game calls it from
    // ReturnToMap and nowhere else, so every state it has ever written is a
    // state where the player is standing on the map with nothing in progress.
    // We were calling it at an arbitrary moment -- whenever a peer connected --
    // and on 2026-08-26 a client pressed `join` at turn 2 of a battle, which
    // ran savefile_catchup -> publish -> flush from inside the fight.
    //
    // What that persists is "inside node N, unresolved". Reload it and the run
    // comes up on the map standing at a node it cannot enter (the battle is
    // over as far as the map is concerned) and cannot move past (the node was
    // never completed). The adventure is intact; it is wedged.
    //
    // Freshness loses to safety here, and it is not a close call: a joiner that
    // gets the last checkpoint is still caught up by CATDATA, INVENTORY and the
    // replayed ACTIONs, whereas a host whose run is wedged has lost the run.
    if (!follow_on_map()) {
        if (!g.said_mid_node) {
            g.said_mid_node = true;
            log_line("SAVEFILE", "not flushing: the run is inside a node, and "
                                 "save_adventure is a node-boundary function -- "
                                 "writing here would persist an unresolved node "
                                 "and wedge the run. Sending the last checkpoint; "
                                 "the joiner is caught up by CATDATA + the action "
                                 "replay.");
        }
        return false;
    }

    const void* director = nullptr;
    if (!mem_read(g.director_slot, &director, sizeof(director)) || !director)
        return false;
    g.save_adventure((void*)director);
    return true;
}

bool publish(uint8_t to, bool fresh) {
    ensure_dir();
    if (!g.have_dir) return false;

    const bool flushed = flush_live_run();
    if (!flushed && !g.said_no_flush) {
        g.said_no_flush = true;
        // Not a warning on the normal path. Publishing from the save-selection
        // screen SHOULD skip the flush: there is no adventure loaded, and the
        // file on disk is already exactly the run the player chose. It is only
        // worth noticing when a peer is being caught up mid-run, and that case
        // says so with its own line at the call site.
        log_line("SAVEFILE", "no adventure loaded -- sending the file as it is on "
                             "disk (this is the normal path from the save screen; "
                             "the flush is for catching a peer up mid-run)");
    }

    wchar_t wname[128];
    wide(g.name, wname, 128);
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\%s", g.dir, wname);

    uint32_t size = 0;
    uint64_t mtime = 0;
    uint8_t* data = read_whole_file(path, size, mtime);
    if (!data) return false;

    SaveFileMsg m{};
    m.slot  = g.slot;
    m.size  = size;
    m.hash  = savefile_hash(data, size);
    m.fresh = fresh ? 1 : 0;
    strncpy_s(m.name, g.name, _TRUNCATE);
    m.data = data;

    bool ok = (to == kNoPeer) ? net_send_savefile(m) : net_send_savefile_to(to, m);
    free(data);
    if (!ok) {
        log_line("SAVEFILE", "!! failed to send %s (%u bytes)", g.name, size);
        return false;
    }

    if (to == kNoPeer)
        log_line("SAVEFILE", "-> sent slot %u '%s', %u bytes, hash %016llx (%s)",
                 g.slot, g.name, size, (unsigned long long)m.hash,
                 fresh ? "a fresh pick -- the client drops any run it has and "
                         "follows this one"
                       : "a catch-up copy");
    else
        log_line("SAVEFILE", "-> re-sent slot %u '%s', %u bytes, hash %016llx to peer %u "
                             "(joined after the first publish)",
                 g.slot, g.name, size, (unsigned long long)m.hash, (unsigned)to);

    // The transfer is exact only when the file has not moved since the host
    // committed to it. If it has, the host is already partway through a run and
    // the client is getting the last checkpoint, not the host's live state --
    // which is the gap RUNSTATE exists to close, and is worth saying out loud
    // rather than discovering as a desync three battles later.
    if (flushed) {
        // Said once per send rather than warned: it is the normal path now,
        // and the hash in the line above is the host's live position.
        log_line("SAVEFILE", "   (flushed the host's live run to disk first -- "
                             "this is the host's current position, not a "
                             "checkpoint)");
    } else if (g.click_mtime && mtime != g.click_mtime) {
        log_line("SAVEFILE", "!! the host has saved progress since choosing this "
                             "file and the live run could not be flushed -- the "
                             "client gets that checkpoint, not the host's live "
                             "run; start the session from the save screen if the "
                             "two must match exactly");
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

// A SECOND, INDEPENDENT ANSWER TO "IS THIS PEER IN A RUN", exported because
// mgmp_leave needs one that does not go through the loaded-scene list.
//
// The two disagree in useful ways rather than redundantly: this one reads the
// run's cat-id list off the MewDirector, so it is true from the moment
// ContinueAdventure has populated it and says nothing about which SCREEN is up;
// the scene walk says exactly which screen is up and nothing about the run. A
// module that has both can answer when either offset has drifted.
bool savefile_adventure_is_loaded() { return adventure_is_loaded_impl(); }


void savefile_init() {
    ensure_state();
    if (g.announced) return;            // the host may have picked before this
    g.announced = true;
    if (g.is_client)
        log_line_lvl(LogLevel::Trace, "SAVEFILE",
                             "armed -- waiting for the host's save; local save-file "
                             "input is suppressed and the run will load from '%s', "
                             "leaving this machine's own saves alone", kCoopSaveName);
    else
        log_line_lvl(LogLevel::Trace, "SAVEFILE",
                             "armed -- will publish this peer's save file to the client");
    if (!g.is_client && g.have_slot)
        log_line_lvl(LogLevel::Trace, "SAVEFILE",
                             "the host had already chosen slot %u '%s' before the "
                             "peer connected -- publishing it now", g.slot, g.name);
}

void savefile_shutdown() {
    if (g.on) {
        // THE ONE LINE IN THIS MODULE A PLAYER MUST NOT MISS. A client that
        // never received a save never joined the run -- it is the whole failure
        // reported from the wild on 2026-08-28, and it used to be Info, sitting
        // in a wall of identical-looking summaries. Same for a host that never
        // published: there is a peer waiting on a file that is not coming.
        //
        // TWO CASES THAT LOOK IDENTICAL IN THE COUNTERS AND ARE NOT FAILURES,
        // both excluded here rather than left to be re-diagnosed later:
        //
        //   * g.announced is set by savefile_init, which runs at go_ready --
        //     i.e. only once a peer has actually connected. Without this test a
        //     host who opened a socket and was never joined reports "published
        //     nothing" as an error, which is just an accurate description of
        //     having played alone.
        //   * a RECONNECTING client declines the save on purpose (it is already
        //     in the run and a save cannot be applied off the selection screen)
        //     and resynchronises from the per-node pushes instead. That is
        //     g.applied == false with g.refused > 0, and it is a success.
        const bool failed = g.announced &&
                            (g.is_client ? (!g.applied && !g.refused)
                                         : !g.published);
        log_line_lvl(failed ? LogLevel::Error : LogLevel::Trace, "SAVEFILE",
                 "done: %s, %u local pick(s) suppressed, %u save(s) "
                 "declined as already-in-run",
                 g.is_client ? (g.applied ? "loaded the host's save" : "never received a save")
                             : (g.published ? "published our save" : "published nothing"),
                 g.suppressed, g.refused);
    }
    g.on = false;
    if (g.blob) { free(g.blob); g.blob = nullptr; }
    g.blob_size = 0;
    g.pending = false;
}

// A peer that joined after the host already published would otherwise never see
// the save at all: savefile_pump publishes exactly once and then latches
// g.published forever, which was correct when a session was two peers that
// connected before anything happened and is wrong the moment a third arrives.
//
// Nothing happens if the host has not published yet -- that peer is early, not
// late, and the ordinary broadcast will include it.
void savefile_set_base(uintptr_t base) {
    ensure_state();
    g.save_adventure = nullptr;
    g.director_slot  = nullptr;

    // This is the target the second signature window existed for:
    // save_adventure and MewDirector::ContinueAdventure are byte-identical for
    // their first TWENTY-ONE bytes, and ContinueAdventure destroys the House
    // scene and frees the live cat registry. The signature resolver cannot make
    // that mistake by construction -- it refuses anything that matches more
    // than once, so a pattern that cannot tell the two apart never resolves at
    // all rather than resolving to the wrong one.
    const uintptr_t addr = addr_of_call(C_SaveAdventure);
    if (!addr) {
        // Non-fatal: without it the host publishes the last checkpoint, which
        // is what it did before this existed. Refusing to publish at all would
        // turn a degraded transfer into no session.
        log_line("SAVEFILE", "!! %s did not resolve by signature -- "
                             "publishing the last checkpoint instead of the live run",
                 kCalls[C_SaveAdventure].name);
        return;
    }
    g.save_adventure = (void(*)(void*))addr;
    g.director_slot  = (const void**)addr_of_data(D_MewDirectorPtr);

    // glaiel::Button::Click, for pressing Play on a client stuck on the menu.
    // Its own feature turns itself off by name if it does not resolve, which is
    // the graded-refusal rule: a client that cannot press Play is back to where
    // it was before this existed -- waiting for a human -- and the log has to
    // say that rather than let the player wonder.
    g.button_click = nullptr;
    const uintptr_t click = addr_of_call(C_ButtonClick);
    if (!click)
        log_line_lvl(LogLevel::Error, "SAVEFILE",
                     "!! %s did not resolve by signature -- this peer cannot press "
                     "Play for itself and a client will have to do it by hand",
                     kCalls[C_ButtonClick].name);
    else
        g.button_click = (void(*)(void*, bool))click;
}

void savefile_catchup(uint8_t peer) {
    ensure_state();
    if (!g.on || g.is_client) return;
    if (!g.published || !g.have_slot) return;
    if (!net_active()) return;
    // fresh = false: this peer is being caught up, not told to restart. A
    // client already in the run must keep it and resync from the per-node
    // pushes; only a slot click means "drop what you have".
    publish(peer, /*fresh=*/false);
}

void savefile_pump() {
    ensure_state();

    // COUNTED HERE, ABOVE THE HOST-ONLY RETURN, because it is a FRAME counter
    // and savefile_on_button_update runs once per BUTTON per frame. Decremented
    // there, a menu with ten live buttons drained a 120-frame cooldown in
    // twelve, so all five allowed presses landed inside a fifth of a second --
    // the same press five times, before the first could have done anything.
    // This function runs exactly once a frame on both roles, which is the unit
    // kPlayRetryFrames is written in.
    if (g.play_cooldown) --g.play_cooldown;

    if (!g.on || g.is_client) return;
    if (g.published) return;
    if (!net_active()) return;

    // THE SILENT STALL. g.have_slot is set by savefile_on_slot_click and by
    // nothing else, so a host that never passed through the save-selection
    // screen IN THIS PROCESS publishes nothing, forever, and used to do it
    // without a single line in either log -- the client sat on "waiting for the
    // host to choose a save file" and the host said nothing at all.
    //
    // It is reachable: `mgmp_loader.exe --attach <pid>` is documented in the
    // README, and a host who attached to a game that was already in a run has
    // no click for the hook to see. Every drop site must say what it dropped.
    if (!g.have_slot) {
        if (++g.publish_tries == 300) {          // ~5 s of a live session
            log_line_lvl(LogLevel::Error, "SAVEFILE",
                     "!! a peer is connected and this host has not chosen a save "
                     "slot in this process, so there is nothing to publish and the "
                     "client is waiting on a file that is not coming. This is what "
                     "attaching to an already-running game looks like: go out to "
                     "the main menu and pick your save again, or relaunch through "
                     "the loader.");
        }
        return;
    }

    // Retried rather than attempted once, because the file may not exist yet:
    // starting a NEW game means the click names a slot the game has not written
    // to disk. Once a second is often enough for a transfer that happens once.
    if (++g.publish_tries % 60 != 1) return;

    // fresh = true: the only way g.have_slot is set is savefile_on_slot_click,
    // so everything this path publishes is a slot the host just chose.
    if (publish(kNoPeer, /*fresh=*/true)) {
        g.published = true;
        return;
    }
    if (g.publish_tries == 1)
        log_line("SAVEFILE", "'%s' is not on disk yet -- will keep trying "
                             "(a new game has no save file until the game writes one)",
                 g.name);
}

bool savefile_on_slot_click(void* ss, int slot) {
    ensure_state();
    if (!g.on || slot < 0) return true;

    Names names{};
    bool  have_names = read_slot_names(ss, names);

    // --- the client: swallow it ------------------------------------------
    //
    // The host owns the run, so a local pick here would put the two peers on
    // different saves -- which no amount of battle-layer lockstep can recover
    // from, because the seed travels with the save. Swallowing rather than
    // disabling the buttons keeps the change to one function, and it holds even
    // while we are still waiting for the host's file.
    if (g.is_client) {
        ++g.suppressed;
        log_line("SAVEFILE", "suppressed local save pick: slot %d (%s) -- the host's "
                             "save is used%s",
                 slot,
                 (have_names && (uint32_t)slot < names.count) ? names.v[slot] : "?",
                 g.pending ? "" : " (not received yet)");
        return false;
    }

    // --- the host: remember it, publish when there is a peer --------------
    if (!have_names || (uint32_t)slot >= names.count) {
        log_line("SAVEFILE", "!! could not read the save-file names off SaveSelection "
                             "%p (slot %d) -- nothing to share", ss, slot);
        return true;
    }

    g.have_slot = true;
    g.slot      = (uint32_t)slot;
    strncpy_s(g.name, names.v[slot], _TRUNCATE);
    g.published     = false;
    g.publish_tries = 0;
    g.click_mtime   = 0;

    ensure_dir();
    if (g.have_dir) {
        wchar_t wname[128]; wide(g.name, wname, 128);
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, _TRUNCATE, L"%s\\%s", g.dir, wname);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
            g.click_mtime = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) |
                            fad.ftLastWriteTime.dwLowDateTime;
    }

    // A SLOT CLICK STARTS A RUN, SO THE PER-RUN DEDUPE CACHES ARE NOW LIES.
    //
    // catsync, invsync and runhist all skip a push whose bytes match the last
    // one they sent, and that cache is keyed on the RUN, not on the peer or the
    // session -- the same reason session.cpp forgets them before catching a
    // reconnecting peer up. A host that goes back to the menu and picks a
    // different slot would otherwise carry the previous run's hashes into the
    // new one and silently skip pushing every cat and item that happens to
    // match, leaving the client holding state from a run nobody is playing.
    //
    // Cheap and unconditional: the worst case is one redundant full push at the
    // first node, which is the same cost a joiner already pays.
    catsync_forget();
    invsync_forget();
    runhist_forget();

    log_line("SAVEFILE", "host chose slot %u '%s' -- publishing it and forgetting "
                         "the per-run cat/inventory/history caches, because this "
                         "starts a run", g.slot, g.name);
    return true;
}

void savefile_on_message(const SaveFileMsg& m) {
    ensure_state();
    if (!g.on) return;
    if (!g.is_client) {
        // Symmetric message, asymmetric authority -- the same failure mode
        // mgmp_follow reports when both peers think they own the run.
        log_line("SAVEFILE", "!! received a save file while hosting -- both peers "
                             "believe they own the run");
        return;
    }
    if (!m.data || !m.size) return;

    uint64_t h = savefile_hash(m.data, m.size);
    if (h != m.hash) {
        log_line("SAVEFILE", "!! save file arrived corrupt (hash %016llx, expected "
                             "%016llx) -- ignoring it",
                 (unsigned long long)h, (unsigned long long)m.hash);
        return;
    }

    // Already in the host's run: this is a reconnect, not a cold join.
    //
    // Applying it would mean loading a save from underneath a live adventure,
    // and there is no path to do that -- savefile_autoselect only runs from
    // the SaveSelection::update hook, so off that screen the blob would sit in
    // g.blob forever with nothing to apply it and NOTHING SAYING SO. That
    // silence is the bug this branch exists to remove: the log used to show
    // `<- host's save: ...` and then behave as though the save had been taken.
    //
    // The run is kept and the host's per-node CATDATA/INVENTORY/ENTERNODE
    // pushes resynchronise it, which is what a reconnect actually needs.
    if (g.applied && !m.fresh) {
        ++g.refused;
        if (g.refused == 1)
            log_line("SAVEFILE", "<- host's save (slot %u '%s', hash %016llx) "
                                 "DECLINED -- this peer is already in the run and "
                                 "a save cannot be applied off the selection "
                                 "screen; staying put and following the host's "
                                 "per-node pushes instead",
                     m.slot, m.name, (unsigned long long)h);
        return;
    }

    // A FRESH PICK OVERRIDES THE LATCH. The host went back to the menu and
    // chose a slot, so it is no longer in the run this peer is holding -- and
    // g.applied, which is set once per process, was the whole reason the second
    // save was declined. Clear it and re-arm the auto-Play so the client can
    // follow the host back through the save screen.
    if (g.applied && m.fresh) {
        g.applied      = false;
        g.play_presses = 0;
        g.play_cooldown = 0;
        g.said_gave_up = false;
        log_line_lvl(LogLevel::Warn, "SAVEFILE",
                 "the host picked a save again -- it has left the run this peer "
                 "was in, so that run is being dropped and reloaded from the "
                 "host's new pick");

        // The residual limit, stated rather than hidden. autoselect only runs
        // from SaveSelection::update and the auto-Play only from the main menu's
        // Button::update, so a client standing inside an adventure has no screen
        // either of them can act on. The blob is kept and applies the moment the
        // player gets back to the menu -- but nothing can drag them there, so
        // this has to be said out loud.
        if (adventure_is_loaded_impl())
            log_line_lvl(LogLevel::Error, "SAVEFILE",
                     "!! this peer is INSIDE a run and a save can only be applied "
                     "from the save-selection screen -- quit to the main menu and "
                     "the host's new run will load on its own");
    }

    if (g.blob) free(g.blob);
    g.blob = (uint8_t*)malloc(m.size);
    if (!g.blob) { g.blob_size = 0; return; }
    memcpy(g.blob, m.data, m.size);
    g.blob_size = m.size;
    g.blob_hash = h;
    g.peer_slot = m.slot;
    strncpy_s(g.peer_name, m.name, _TRUNCATE);
    g.pending = true;
    g.applied = false;

    log_line("SAVEFILE", "<- host's save: slot %u '%s', %u bytes, hash %016llx",
             m.slot, m.name, m.size, (unsigned long long)h);
}

int savefile_autoselect(void* ss) {
    ensure_state();
    if (!g.on || !ss) return -1;

    // Print the slot names once, the first time the screen exists, exactly as
    // the battle layer prints its roster -- and for the same reason. Every
    // offset in this module hangs off SaveSelection+0x38 being a
    // std::vector<std::string> of file names, which was read statically; three
    // plausible ".sav" names in the log is the cheapest possible confirmation
    // that it is, and a missing line is the cheapest possible refutation.
    if (!g.printed_names) {
        g.printed_names = true;
        Names n{};
        if (read_slot_names(ss, n)) {
            for (uint32_t i = 0; i < n.count; ++i)
                log_line("SAVEFILE", "slot %u = %s", i, n.v[i]);
        } else {
            log_line("SAVEFILE", "!! could not read the save-file names off "
                                 "SaveSelection %p -- this module cannot work "
                                 "until that offset is re-derived", ss);
        }
    }

    if (!g.is_client || g.applied) return -1;

    if (!g.pending) {
        // Say it once. The screen sits there doing nothing until the host picks
        // a file, and a silent stall is exactly the failure the map layer's
        // late-join gap taught us to narrate.
        if (!g.said_waiting) {
            g.said_waiting = true;
            log_line("SAVEFILE", "waiting for the host to choose a save file; "
                                 "this screen will advance on its own");
        }
        return -1;
    }

    Names names{};
    if (!read_slot_names(ss, names)) return -1;   // screen not built yet

    // Which card gets clicked. This is now cosmetic rather than load-bearing --
    // the redirect decides what is opened -- but matching the host's choice
    // keeps "the host is on file 2" true on both screens. Resolve by NAME
    // first and fall back to the slot index: SaveSelection::init builds either
    // campaignNN.sav or steamcampaignNN.sav depending on the build, so a Steam
    // host and a non-Steam client agree on the slot and not on the filename.
    int slot = -1;
    for (uint32_t i = 0; i < names.count; ++i)
        if (_stricmp(names.v[i], g.peer_name) == 0) { slot = (int)i; break; }

    if (slot < 0) {
        if (g.peer_slot < names.count) {
            slot = (int)g.peer_slot;
            log_line("SAVEFILE", "the host's '%s' is not one of our slot names -- "
                                 "falling back to slot %u ('%s')",
                     g.peer_name, g.peer_slot, names.v[slot]);
        } else {
            // Any card would do, since the redirect decides the file -- so a
            // roster we cannot match is a reason to fall back, not to give up.
            slot = 0;
            log_line("SAVEFILE", "the host's slot %u is out of range here (%u slots) "
                                 "and '%s' matches none of them -- clicking slot 0; "
                                 "the redirect makes the card irrelevant",
                     g.peer_slot, names.count, g.peer_name);
        }
    }

    ensure_dir();
    if (!g.have_dir) { g.pending = false; return -1; }

    // The host's run goes into OUR file, not into the slot the host was
    // playing. names[slot] is only used to pick which card on the screen gets
    // clicked; what actually gets opened is decided by the redirect below.
    wchar_t wcoop[64];
    wide(kCoopSaveName, wcoop, 64);
    back_up(g.dir, wcoop);              // best effort; it is only ever a mirror

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\%s", g.dir, wcoop);
    if (!write_whole_file(path, g.blob, g.blob_size)) {
        DWORD e = GetLastError();
        if (e == ERROR_SHARING_VIOLATION)
            log_line("SAVEFILE", "!! %ls is open in another process -- another "
                                 "client on this machine is already using it", path);
        else
            log_line("SAVEFILE", "!! could not write %ls (win32 %lu)", path, e);
        g.pending = false;
        return -1;
    }

    // Arm the substitution. Without it this click would load the client's own
    // names[slot], which is the player's real save and has nothing to do with
    // the host's run.
    memset(&g.redirect_name, 0, sizeof(g.redirect_name));
    memcpy(g.redirect_name.buf, kCoopSaveName, sizeof(kCoopSaveName));
    g.redirect_name.size = sizeof(kCoopSaveName) - 1;
    g.redirect_name.cap  = 15;                 // SSO: the text is inline, no heap
    g.redirect_armed     = true;

    log_line("SAVEFILE", "wrote the host's save to %ls (%u bytes) -- clicking slot "
                         "%d ('%s') and redirecting the load to '%s'; the local "
                         "save files are not touched",
             path, g.blob_size, slot, names.v[slot], kCoopSaveName);

    g.pending = false;
    g.applied = true;
    return slot;
}

const void* savefile_redirect_load() {
    if (!g.on || !g.redirect_armed) return nullptr;
    g.redirect_armed = false;
    log_line("SAVEFILE", "redirecting MewDirector::init to '%s'", kCoopSaveName);
    return &g.redirect_name;
}

void savefile_on_button_update(void* button) {
    // THE CHEAP TESTS FIRST, AND THE ORDER IS THE POINT. This runs for every
    // button in the game on every frame, so everything above the name read is
    // a load and a branch, and g.pending is false for all of a normal session.
    if (!g.on || !g.is_client) return;
    if (!g.pending || g.applied) return;      // nothing waiting, or already in
    if (!button) return;

    if (!g.button_click) {
        // Said once. savefile_set_base already reported the resolve failure;
        // this is the moment it actually costs something, and the player is
        // looking at the menu right now.
        if (!g.said_no_click) {
            g.said_no_click = true;
            log_line_lvl(LogLevel::Error, "SAVEFILE",
                         "the host's save is here but Button::Click did not "
                         "resolve -- press Play yourself to load into the run");
        }
        return;
    }

    if (g.play_presses >= kPlayMaxPresses) {
        if (!g.said_gave_up) {
            g.said_gave_up = true;
            log_line_lvl(LogLevel::Error, "SAVEFILE",
                         "pressed Play %u times and this peer is still on the main "
                         "menu -- press it yourself; the host's save is loaded and "
                         "waiting, and the save screen will advance on its own once "
                         "it is up", g.play_presses);
        }
        return;
    }

    if (g.play_cooldown) return;             // counted down in savefile_pump

    // Identify by NAME, never by pointer or by position in the panel -- the
    // fourth time this project has settled on that. MainMenu::init assigns
    // "MainMenu_Button_Play" to Button+504 and Button::Click reads its length
    // from Button+520 to build the click sound, so the offset has two readings.
    char name[64];
    if (!mem_read_std_string((const uint8_t*)button + kBtn_Name, name, sizeof(name)))
        return;                                // not a named button, or not one
    if (strcmp(name, kBtnName_MainMenuPlay) != 0) return;

    // From here we know: we are a client, on the main menu, with the host's run
    // already written to disk and nothing to do but open it.
    ++g.play_presses;
    g.play_cooldown = kPlayRetryFrames;
    log_line("SAVEFILE", "the host's save is in -- pressing '%s' for you%s",
             name,
             g.play_presses > 1 ? " (again; the first press did not take)" : "");

    // force = 0, exactly as Button::update calls it. See the C_ButtonClick note
    // in mgmp_addresses.h: forcing would bypass a guard the game set for a
    // reason and would hide a refusal we want to hear about.
    g.button_click(button, false);
}

} // namespace mgmp
