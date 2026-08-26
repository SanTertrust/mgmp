// mgmp_combatlock.cpp -- see mgmp_combatlock.h for why this is two hooks and
// why it reads the bar's subject rather than asking who is being polled.
#include "mgmp_combatlock.h"

#include "mgmp_addresses.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_tuning.h"

namespace mgmp {
namespace {

// The bar is one row of abilities plus end-turn. Eight or nine in practice; the
// cap exists so a garbage vector (a CombatMenu read through a stale pointer, a
// layout that moved in a future build) costs a bounded loop and a log line
// instead of walking the heap. Overflowing it disengages rather than truncating:
// half a greyed bar is a worse answer than none.
constexpr uint32_t kMaxButtons = 32;

struct State {
    bool     engaged   = false;          // this tick's verdict, for the Button hook
    void*    button[kMaxButtons] = {};
    uint32_t buttons   = 0;

    // What `engaged` was at the END of the last combat-menu tick. `engaged`
    // itself is cleared on the way out so a stray Button::update outside the
    // scope can never act on a stale snapshot, which makes it useless to a panel
    // reading it from somewhere else in the frame. This is the reportable copy;
    // out of a battle it holds whatever the last bar said, which is what a debug
    // row wants anyway.
    bool     held      = false;

    // Last verdict announced, so the log carries the transitions rather than a
    // line per frame. -1 = nothing said yet.
    int      said      = -1;
    void*    said_for  = nullptr;
};

State g;

// CombatMenu+280 is a Ref<Character>: sub_1400A1B30 stores the pointer and the
// generation that was at ptr-8 when it was taken, and every read in
// CombatMenu::update re-checks it before dereferencing. A mismatch is the
// game's own "this cat is gone" signal, and it is the state in which it hides
// the bar -- so a mismatch here means there is nothing to decide about.
void* subject_of(const void* menu) {
    void*    ch  = nullptr;
    uint64_t gen = 0, live = 0;
    if (!mem_read((const uint8_t*)menu + kCM_Subject, &ch, sizeof(ch)) || !ch)
        return nullptr;
    if (!mem_read((const uint8_t*)menu + kCM_SubjectGen, &gen, sizeof(gen)))
        return nullptr;
    if (!mem_read((const uint8_t*)ch - kRefGenOffset, &live, sizeof(live)))
        return nullptr;
    return (live == gen) ? ch : nullptr;
}

// Snapshot the button vector for the duration of one tick. Cached rather than
// re-read per button because the Button hook fires for every button in the
// process and must not pay a game-memory read to find out it has nothing to do.
bool snapshot_buttons(const void* menu) {
    g.buttons = 0;

    uint8_t* begin = nullptr;
    uint8_t* end   = nullptr;
    if (!mem_read((const uint8_t*)menu + kCM_ButtonsBegin, &begin, sizeof(begin))) return false;
    if (!mem_read((const uint8_t*)menu + kCM_ButtonsEnd,   &end,   sizeof(end)))   return false;
    if (!begin || end < begin) return false;

    const size_t bytes = (size_t)(end - begin);
    if (bytes % sizeof(void*)) return false;
    const size_t n = bytes / sizeof(void*);
    if (n == 0 || n > kMaxButtons) return false;

    if (!mem_read(begin, g.button, n * sizeof(void*))) return false;
    g.buttons = (uint32_t)n;
    return true;
}

bool is_ours(const void* button) {
    for (uint32_t i = 0; i < g.buttons; ++i)
        if (g.button[i] == button) return true;
    return false;
}

} // namespace

void combatlock_enter(void* combat_menu) {
    g.engaged = false;
    g.held    = false;
    g.buttons = 0;
    if (!tune::kCombatLock || !combat_menu) return;

    void* subject = subject_of(combat_menu);
    if (!subject) return;

    const bool peer_owns = lockstep_peer_owns_character(subject);

    // Say it once per change of answer, and once per change of subject, so a log
    // read after the fact shows which cat each stretch of greying was for. A
    // menu ticks every frame; a line every frame would be the loudest thing in
    // the file and the least informative.
    if (g.said != (int)peer_owns || g.said_for != subject) {
        log_line("CMLOCK", "combat menu for cat %p: %s",
                 subject, peer_owns ? "a PEER owns it -- greying the bar"
                                    : "ours -- bar left alone");
        g.said     = (int)peer_owns;
        g.said_for = subject;
    }

    if (!peer_owns) return;
    if (!snapshot_buttons(combat_menu)) {
        // The verdict was reachable but the buttons were not. Refusing rather
        // than greying what we did manage to read: a bar half of which responds
        // is a worse lie than a bar that is simply not helping.
        log_line("CMLOCK", "button vector unreadable on %p -- bar left alone", combat_menu);
        return;
    }
    g.engaged = true;
    g.held    = true;
}

void combatlock_leave() {
    g.engaged = false;
}

void combatlock_on_button(void* button) {
    if (!g.engaged || !button) return;
    if (!is_ours(button)) return;      // some other button updating re-entrantly

    const int32_t disabled = kBtnState_Disabled;
    mem_write((uint8_t*)button + kBtn_State, &disabled, sizeof(disabled));
}

bool     combatlock_engaged() { return g.held; }
uint32_t combatlock_buttons() { return g.buttons; }

} // namespace mgmp
