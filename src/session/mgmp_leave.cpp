// mgmp_leave.cpp -- see mgmp_leave.h.

#include "mgmp_leave.h"
#include "mgmp_net.h"
#include "mgmp_lockstep.h"   // lockstep_in_battle -- a marker on the press, not a gate
#include "mgmp_savefile.h"   // savefile_adventure_is_loaded -- the second opinion
#include "mgmp_config.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"

#include <cstring>
#include <cstdio>

namespace mgmp {
namespace {

// A run has a handful of scenes live at once. The cap is generous and bounded:
// a five-digit count means the offsets moved, and this module must then report
// that rather than walk a megabyte of heap.
constexpr uint32_t kMaxScenes = 64;

// Frames between polls. The scene list changes when a person clicks something,
// so twice a second is already far finer than the event it watches, and it
// keeps the per-frame cost of this module at one increment and one compare.
constexpr uint32_t kPollFrames = 30;

// How many consecutive OUT polls before the host announces. See the header:
// the asymmetry means a transition gap can only suppress, never invent -- this
// is belt and braces on top of that, and it costs one second.
constexpr uint32_t kConfirmPolls = 2;

// The same shape as savefile's auto-Play, for the same reasons: the first press
// is immediate, and if the button is still ticking kRetryFrames later then the
// transition did not start.
constexpr uint32_t kRetryFrames = 120;   // ~2 s at 60 Hz
constexpr uint32_t kMaxPresses  = 5;

// WHERE THIS PEER IS, from two independent readings, because either can drift.
//
// The scene walk answers "which screen is up" and knows nothing about the run;
// savefile's cat-id test answers "is a run loaded" and knows nothing about the
// screen. They are not redundant, and a module that consults both still has an
// answer when one of them has stopped working -- which is the difference
// between telling a player their host left and saying nothing at all.
enum class Where { Unknown, InRun, OutOfRun };

struct SceneList {
    uint32_t count = 0;
    char     name[kMaxScenes][40] = {};
    bool     dying[kMaxScenes] = {};
};

struct State {
    bool on        = false;
    bool is_client = false;
    bool broken    = false;   // the scene walk did not read as a scene list
    bool said_broken = false;
    bool printed   = false;   // the one-shot roster of loaded scenes

    const void** director_slot = nullptr;
    void  (*button_click)(void*, bool) = nullptr;

    uint32_t tick = 0;

    // --- host ---
    bool     was_in_run = false;   // has this peer been inside a run at all
    uint32_t out_polls  = 0;
    bool     announced  = false;
    uint32_t sent       = 0;

    // --- client ---
    bool     pending      = false;  // told to leave, still in a run
    uint32_t presses      = 0;
    uint32_t cooldown     = 0;
    bool     said_gave_up = false;
    bool     said_no_click = false;
    bool     said_sidebar  = false;   // the pause sidebar was seen ticking
    uint32_t left         = 0;      // departures actually completed
    uint32_t received     = 0;

    // THE LAST POSITION THE PUMP READ, so nothing else has to walk the scene
    // list to find out where we are.
    //
    // leave_status runs from the panel's render, i.e. every frame, and it was
    // taking the full walk each time: a pointer chase and a std::string read
    // per loaded scene, sixty times a second, for a line of text. During a
    // scene teardown those reads land on freed Scene objects and fault -- caught,
    // but the faults were real and they were the ones filling the crash log on
    // 2026-08-28. Polling twice a second is already far finer than the event
    // being watched; reading it sixty times a second bought nothing.
    Where    where       = Where::Unknown;
    char     where_name[40] = {};
};

State g;

// Same de-latched shape as savefile's, and for the same reason: the role can be
// set by the panel's connect buttons long after the first call, so an answer
// computed once is an answer that stays wrong for the life of the process.
void ensure_state() {
    const Config& cfg = config();
    const bool host   = _stricmp(cfg.net_role, "host")   == 0;
    g.is_client       = _stricmp(cfg.net_role, "client") == 0;
    g.on              = (host || g.is_client);
}

// Walk the game's loaded-scene vector. Pure reads; see the kScene_* block in
// mgmp_addresses.h.
//
// GATED ON EVIDENCE, NOT ON PERFECTION, and the difference is the whole reason
// this reads the way it does now. The first version required EVERY entry to
// yield a non-empty printable name and returned false otherwise -- so a single
// scene with no name, or one name longer than the buffer, disabled the module
// outright and took the "the host left the run" announcement with it. That is
// the wrong shape for a heuristic: "at least one entry read as a scene name"
// already rules out an unrelated pair of pointers, and unreadable entries can
// simply be skipped, because the question being asked of this list is whether a
// PARTICULAR name is present.
//
// So an entry that does not read leaves an empty slot and the walk continues;
// the walk fails only if the vector itself is implausible or NOTHING in it read
// as a name.
bool read_scenes(SceneList& out) {
    out.count = 0;
    if (!g.director_slot) return false;

    const void* director_owner = nullptr;   // the MewDirector, itself a Component
    if (!mem_read(g.director_slot, &director_owner, sizeof(director_owner)) ||
        !director_owner)
        return false;

    const void* director = nullptr;
    if (!mem_read((const uint8_t*)director_owner + kDir_SceneDirector,
                  &director, sizeof(director)) || !director)
        return false;

    const uint8_t* begin = nullptr;
    const uint8_t* end   = nullptr;
    if (!mem_read((const uint8_t*)director + kDirector_ScenesBegin, &begin, sizeof(begin)))
        return false;
    if (!mem_read((const uint8_t*)director + kDirector_ScenesEnd,   &end,   sizeof(end)))
        return false;
    if (!begin || end < begin) return false;

    const size_t bytes = (size_t)(end - begin);
    if (bytes % sizeof(void*)) return false;
    const size_t n = bytes / sizeof(void*);
    if (n == 0 || n > kMaxScenes) return false;

    uint32_t named = 0;
    for (size_t i = 0; i < n; ++i) {
        char* dst = out.name[i];
        dst[0] = 0;
        out.dying[i] = false;

        const void* scene = nullptr;
        if (!mem_read(begin + i * sizeof(void*), &scene, sizeof(scene)) || !scene)
            continue;
        if (!mem_read_std_string((const uint8_t*)scene + kScene_Name, dst,
                                 sizeof(out.name[0]))) {
            dst[0] = 0;
            continue;
        }

        const size_t len = strlen(dst);
        bool printable = len > 0;
        for (size_t c = 0; c < len && printable; ++c)
            if (dst[c] < 0x20 || dst[c] > 0x7E) printable = false;
        if (!printable) { dst[0] = 0; continue; }

        uint8_t dying = 0;
        if (mem_read((const uint8_t*)scene + kScene_Destroying, &dying, 1))
            out.dying[i] = (dying != 0);
        ++named;
    }
    // Nothing read as a name: this is not a scene vector.
    if (!named) return false;
    out.count = (uint32_t)n;
    return true;
}

// Which of the three "not in a run" scenes is live, or nullptr.
const char* out_scene(const SceneList& s) {
    static const char* const kOut[] = { kScene_House, kScene_MainMenu, kScene_SaveSelect };
    for (uint32_t i = 0; i < s.count; ++i) {
        if (s.dying[i]) continue;
        for (const char* k : kOut)
            if (strcmp(s.name[i], k) == 0) return k;
    }
    return nullptr;
}

// One line naming everything the game has loaded, once. This is the cheapest
// possible confirmation that Director+0/+8 really is a vector of scenes and
// Scene+1208 really is its name -- three or four recognisable names in the log
// prove it, and a line of nonsense refutes it, without anyone reading the
// disassembly again.
void print_once(const SceneList& s) {
    if (g.printed) return;
    g.printed = true;
    char line[512];
    int  off = 0;
    for (uint32_t i = 0; i < s.count && off < (int)sizeof(line) - 48; ++i)
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "%s%s%s",
                           i ? ", " : "", s.name[i], s.dying[i] ? "(dying)" : "");
    log_line_lvl(LogLevel::Trace, "LEAVE", "scenes loaded: %s", line);
}

// Takes the reading. THE ONLY CALLERS ARE THE PUMP AND leave_on_message --
// everything else reads g.where, which this updates. See the State comment.
Where where_are_we(const char** out_name) {
    if (out_name) *out_name = nullptr;

    // THE POSITIVE HALF, AND IT IS NOT OPTIONAL. "In a run" used to mean only
    // that none of the three out-scenes was loaded -- an absence, which is a
    // reading nothing has to be true for.
    //
    // At startup nothing is true. The game boots into
    //   Shared, Base, Tutorial, Cutscene, PauseMenu, ToolTip, Transition, ...
    // with no MainMenu scene yet, so the absence test said IN A RUN, the host
    // recorded that it had been in one, and the instant MainMenu finished
    // loading it read OUT and announced a departure that never happened. The
    // client believed it and quit to the menu -- measured 2026-08-28, host seq
    // 35 then 37, client seq 34 then 35, all before a save had even been
    // picked. The asymmetry that stops a transition gap from manufacturing a
    // departure does not help here, because this gap is not transient: it lasts
    // the whole boot sequence.
    //
    // So a run has to be POSITIVELY established, and the cat-id list is the
    // reading that can do it -- it is populated by ContinueAdventure and empty
    // on every menu. Absence of an out-scene is now necessary and not
    // sufficient, and anything else is Unknown, which never announces.
    const bool run_loaded = savefile_adventure_is_loaded();

    SceneList s{};
    if (read_scenes(s)) {
        if (const char* out = out_scene(s)) {
            if (out_name) *out_name = out;
            return Where::OutOfRun;
        }
        return run_loaded ? Where::InRun : Where::Unknown;
    }

    // The scene list did not read. The cat-id test cannot say WHICH screen is
    // up, so the caller loses the scene name in its log line and nothing else --
    // but it can still say a run is loaded, and that is the half that gates the
    // announcement.
    return run_loaded ? Where::InRun : Where::Unknown;
}

// Take a reading and remember it. Returns the same thing where_are_we does.
Where refresh_where(const char** out_name) {
    const char* named = nullptr;
    g.where = where_are_we(&named);
    strncpy_s(g.where_name, named ? named : "", _TRUNCATE);
    if (out_name) *out_name = g.where_name[0] ? g.where_name : nullptr;
    return g.where;
}

void report_broken() {
    if (g.said_broken) return;
    g.said_broken = true;
    g.broken = true;
    log_line_lvl(LogLevel::Error, "LEAVE",
                 "!! the loaded-scene list did not read as one (Director+0/+8, "
                 "Scene+%u name, Scene+%u destroy flag) -- this peer cannot tell "
                 "whether the host is still in the run, so nobody will be told "
                 "when it leaves. Re-derive those offsets after a game update.",
                 (unsigned)kScene_Name, (unsigned)kScene_Destroying);
}

} // namespace

// ---------------------------------------------------------------------------

void leave_init() {
    ensure_state();
    g.was_in_run = false;
    g.out_polls  = 0;
    g.announced  = false;
    g.pending    = false;
    g.presses    = 0;
    g.cooldown   = 0;
    g.said_gave_up = false;
    g.tick       = 0;
    g.where      = Where::Unknown;
    g.where_name[0] = 0;
    log_line_lvl(LogLevel::Trace, "LEAVE",
                 g.is_client
                     ? "armed -- if the host leaves the run, this peer is taken "
                       "back to the main menu"
                     : "armed -- will tell the client when this peer leaves the run");
}

void leave_shutdown() {
    if (g.on) {
        // A client that was told to leave and never managed it is the failure
        // worth seeing: it is still sitting in a dead run. Everything else here
        // is bookkeeping.
        const bool failed = g.pending;
        log_line_lvl(failed ? LogLevel::Error : LogLevel::Trace, "LEAVE",
                 "done: %u departure(s) announced, %u received, %u acted on%s",
                 g.sent, g.received, g.left,
                 failed ? " -- AND THIS PEER IS STILL IN A RUN THE HOST HAS LEFT; "
                          "open the pause menu and quit to the menu"
                        : "");
    }
    g.on = false;
    g.pending = false;
}

void leave_set_base(uintptr_t /*base*/) {
    ensure_state();
    g.director_slot = (const void**)addr_of_data(D_MewDirectorPtr);
    g.broken = false;
    g.said_broken = false;

    // Shared with savefile's auto-Play, and it turns off the same way: without
    // it this module can still SAY the host left, which is most of the value --
    // the client just has to press the button itself.
    g.button_click = nullptr;
    const uintptr_t click = addr_of_call(C_ButtonClick);
    if (!click)
        log_line_lvl(LogLevel::Error, "LEAVE",
                     "!! %s did not resolve by signature -- this peer can be told "
                     "the host left the run but cannot press Quit To Menu for you",
                     kCalls[C_ButtonClick].name);
    else
        g.button_click = (void(*)(void*, bool))click;
}

// The cached reading -- see the State comment. Half a second stale at worst,
// which is finer than the thing it describes changes.
bool leave_in_run() { return g.where == Where::InRun; }

void leave_pump() {
    ensure_state();

    // THE COOLDOWN IS SERVICED BEFORE EVERY EARLY RETURN, and that ordering is
    // the whole bug this function shipped with.
    //
    // It is counted here rather than in the button callback because that
    // callback runs once per BUTTON per frame: decremented there, a screen with
    // ten live buttons drains a 120-frame cooldown in twelve, so all five
    // allowed presses land inside a fifth of a second -- the same press five
    // times before anything could respond to the first.
    //
    // But it sat BELOW `if (!g.on || !net_active()) return;`, so on any frame
    // that guard fired the cooldown was never counted down. One press parked it
    // at 120 forever and the button callback returned at `if (g.cooldown)` from
    // then on. THE FEATURE WORKED EXACTLY ONCE PER PROCESS and then went silent
    // -- reported as "worked in the battle, then did not work on the adventure
    // or in a battle either". A counter that gates the only action a module
    // takes must be counted unconditionally.
    if (g.cooldown) --g.cooldown;

    // Nothing armed and no role: there is nothing to watch for. `g.pending` is
    // in the test because leave_request_local arms a peer that may have no
    // session at all, and the "did we get out" check below has to run for it.
    if (!g.on && !g.pending) return;

    if (++g.tick % kPollFrames) return;

    // Printed once when it works, and the failure is reported once too --
    // without this the module's whole input was invisible, which is exactly
    // what made the first report of "quit to menu did nothing" unanswerable.
    SceneList s{};
    if (read_scenes(s)) print_once(s);
    else                report_broken();

    const char* out = nullptr;
    const Where here = refresh_where(&out);
    const char* where_name = out ? out : "somewhere this peer cannot name";

    // --- we were leaving, and we got out ----------------------------------
    //
    // Before the role split, because leave_request_local can arm a host and a
    // departure that completes has to be reported whoever asked for it.
    if (g.pending && here == Where::OutOfRun) {
        g.pending = false;
        ++g.left;
        log_line("LEAVE", "out of the run and on '%s' -- if the host picks a save "
                          "again this peer will follow it back in on its own",
                 where_name);
    }
    // A FRESH RUN IS A FRESH CHANCE. The press budget is per departure, not per
    // process: without this, a peer that spent its five presses once could
    // never be taken out of a later run, and the only symptom would be the
    // gave-up line from the previous one.
    if (here == Where::InRun && (g.presses || g.said_gave_up)) {
        g.presses      = 0;
        g.said_gave_up = false;
    }

    // Everything below is the ANNOUNCING half, and only that half needs a live
    // session -- the arming and the click above do not.
    if (!g.on || !net_active()) return;
    if (g.is_client) return;

    // --- the host: announce leaving, exactly once per run ------------------
    //
    // ONLY A POSITIVE "IN A RUN" ARMS THE ANNOUNCEMENT. Unknown suppresses --
    // it neither arms nor announces -- which is the same asymmetry as before
    // read the other way round: a reading we could not take must never become
    // half of "was in a run, now is not". That pair is the whole claim.
    if (here == Where::InRun) {
        if (!g.was_in_run)
            log_line("LEAVE", "the host is in a run -- the client will be told if "
                              "it leaves");
        g.was_in_run = true;
        g.out_polls  = 0;
        g.announced  = false;      // re-arm: a NEW run may be left later
        return;
    }
    if (here == Where::Unknown) {
        g.out_polls = 0;           // a boot screen or a transition, not a departure
        return;
    }
    // Never been in a run this session -- sitting on the menu at startup is not
    // a departure, and announcing it would take a client out of a run it had
    // legitimately been caught up into.
    if (!g.was_in_run || g.announced) return;
    if (++g.out_polls < kConfirmPolls) return;

    g.announced  = true;
    g.was_in_run = false;
    ++g.sent;

    HostLeftMsg m{};
    strncpy_s(m.scene, out ? out : "", _TRUNCATE);
    if (net_send_hostleft(m))
        log_line("LEAVE", "-> the host has left the run and is on '%s' -- telling "
                          "the client, which is otherwise unable to tell this "
                          "apart from a long turn", where_name);
    else
        log_line_lvl(LogLevel::Error, "LEAVE",
                 "!! the host left the run and the message could not be sent -- "
                 "the client is still in a run nobody is playing");
}

void leave_on_message(const HostLeftMsg& m) {
    ensure_state();
    if (!g.on) return;
    ++g.received;

    if (!g.is_client) {
        // Symmetric message, asymmetric authority -- the same failure mode
        // savefile and follow both report when both peers think they own the
        // run.
        log_line("LEAVE", "!! received a 'host left the run' while hosting -- both "
                          "peers believe they own the run");
        return;
    }

    const Where here = refresh_where(nullptr);

    // ONLY "definitely out" declines. Unknown ARMS, and that is the correction:
    // the first version bailed with a warning whenever it could not read its own
    // position, which is a drop site that does nothing -- the rule this project
    // keeps relearning. Arming on Unknown costs nothing, because the only thing
    // it can do is click a Quit To Menu button, and that button does not exist
    // unless the player is in a run or the house and has opened the pause menu.
    if (here == Where::OutOfRun) {
        // The common case when the host is merely re-picking a slot: it went out
        // to the menu, we were already there. Not a problem, and not worth
        // interrupting anyone for.
        log_line_lvl(LogLevel::Trace, "LEAVE",
                 "the host has left the run (it is on '%s'); this peer is not in a "
                 "run either, so there is nothing to do", m.scene);
        return;
    }

    g.pending      = true;
    g.presses      = 0;
    g.cooldown     = 0;
    g.said_gave_up = false;

    g.said_sidebar = false;

    // THE LINE A PLAYER MUST NOT MISS, and the reason it names a key. There is
    // no persistent PauseMenu to reach into -- see the header -- so the one
    // thing this peer cannot do for itself is open the menu.
    log_line_lvl(LogLevel::Warn, "LEAVE",
             "the host has LEFT THE RUN (it is on '%s')%s. Press Escape: the mod "
             "will press Quit To Menu for you, and if the host starts a run again "
             "you will be taken back in automatically.",
             m.scene,
             here == Where::InRun
                 ? " and this peer is still in it"
                 : " and this peer cannot tell whether it is still in one, so it "
                   "is arming anyway");
}

void leave_status(char* out, size_t out_size) {
    if (!out || !out_size) return;
    out[0] = 0;

    // The CACHED reading. This runs once per rendered frame and must not walk
    // the game's scene list to do it -- see the State comment.
    const char* place = "?";
    switch (g.where) {
        case Where::InRun:    place = "in a run";                             break;
        case Where::OutOfRun: place = g.where_name[0] ? g.where_name : "out"; break;
        default:              place = "cannot tell";                          break;
    }

    if (!g.pending) {
        _snprintf_s(out, out_size, _TRUNCATE, "idle -- %s%s", place,
                    g.button_click ? "" : "  [Button::Click UNRESOLVED]");
        return;
    }
    _snprintf_s(out, out_size, _TRUNCATE,
                "ARMED -- %s, %u press(es) left%s%s", place,
                g.presses < kMaxPresses ? kMaxPresses - g.presses : 0,
                g.cooldown ? "  (waiting out the last press)" : "",
                g.said_sidebar ? "  [pause menu seen]"
                               : "  [pause menu NOT seen yet]");
}

void leave_request_local() {
    ensure_state();
    g.pending      = true;
    g.presses      = 0;
    g.cooldown     = 0;
    g.said_gave_up = false;
    g.said_sidebar = false;
    // g.on is NOT required here and that is the point: the click path does not
    // depend on a session, so neither should the test of it. What it does need
    // is Button::Click, which is resolved in leave_set_base at load time.
    g.on = true;
    log_line("LEAVE", "armed by hand from the panel%s -- open the pause menu and "
                      "Quit To Menu will be pressed for you",
             g.button_click ? "" : ", BUT Button::Click did not resolve, so "
                                   "nothing can be pressed");
}

void leave_on_button_update(void* button) {
    // The cheap tests first, in this order deliberately: this runs for every
    // button in the game on every frame, and g.pending is false for the whole
    // of a normal session.
    // NOT role-gated. g.pending is only ever set by a client receiving HOSTLEFT
    // or by leave_request_local, so the role has already been decided by
    // whoever set it -- and gating here would make the panel's test button do
    // nothing on the machine a developer is most likely sitting at.
    if (!g.pending || !button) return;

    if (!g.button_click) {
        if (!g.said_no_click) {
            g.said_no_click = true;
            log_line_lvl(LogLevel::Error, "LEAVE",
                         "the pause menu is open but Button::Click did not resolve "
                         "-- press Quit To Menu yourself");
        }
        return;
    }

    if (g.presses >= kMaxPresses) {
        if (!g.said_gave_up) {
            g.said_gave_up = true;
            log_line_lvl(LogLevel::Error, "LEAVE",
                         "pressed Quit To Menu %u times and this peer is still in "
                         "the run -- press it yourself", g.presses);
        }
        return;
    }

    if (g.cooldown) return;                  // counted down in leave_pump

    char name[64];
    if (!mem_read_std_string((const uint8_t*)button + kBtn_Name, name, sizeof(name)))
        return;                              // not a named button, or not one

    // SAY WHETHER THE SIDEBAR WAS EVER SEEN, once. This is the line that makes
    // the next "I pressed Escape and nothing happened" answerable in one log
    // instead of a session: either this appears -- so the hook does see the
    // pause menu and the quit button simply is not in it under that name -- or
    // it does not, and the problem is upstream of this function entirely.
    if (!g.said_sidebar && strncmp(name, "Button_PauseMenu_", 17) == 0) {
        g.said_sidebar = true;
        log_line("LEAVE", "the pause menu is open (saw '%s') -- waiting for '%s'",
                 name, kBtnName_PauseQuitToMenu);
    }

    if (strcmp(name, kBtnName_PauseQuitToMenu) != 0) return;

    ++g.presses;
    g.cooldown = kRetryFrames;
    // SAY WHETHER A BATTLE IS BEING TORN DOWN. This is a marker, not a gate:
    // quitting mid-battle is something the game supports and a player does, so
    // refusing it here would be inventing a rule. But it is also the state in
    // which this mod is holding the most pointers into what is about to be
    // destroyed, so if a press is ever followed by a fault, the first question
    // is whether it was this press or a quiet one from the map -- and that
    // question has to be answerable from the log rather than from memory.
    log_line("LEAVE", "the host left the run -- pressing '%s' for you%s%s", name,
             g.presses > 1 ? " (again; the first press did not take)" : "",
             lockstep_in_battle() ? "  [a battle is live -- this tears it down]" : "");

    // force = 0, exactly as Button::update calls it. Forcing would bypass a
    // guard the game set for a reason and would hide a refusal worth hearing
    // about -- the same argument as the Play button.
    g.button_click(button, false);
}

} // namespace mgmp
