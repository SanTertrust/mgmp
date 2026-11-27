// mgmp_combatlock.cpp -- see mgmp_combatlock.h for why this is two hooks and
// why it reads the bar's subject rather than asking who is being polled.
#include "mgmp_combatlock.h"

#include "mgmp_ability.h"
#include "mgmp_addresses.h"
#include "mgmp_aim.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_overlay.h"
#include "mgmp_resolve.h"
#include "mgmp_tuning.h"

#include <windows.h>

namespace mgmp {
namespace {

struct Vec2D { double x, y; };
using fn_get_world_pos = Vec2D* (__fastcall*)(void* renderer, Vec2D* out, const Vec2D* local,
                                              void* disp_obj, double scale);

// The bar is one row of abilities plus end-turn. Eight or nine in practice; the
// cap exists so a garbage vector (a CombatMenu read through a stale pointer, a
// layout that moved in a future build) costs a bounded loop and a log line
// instead of walking the heap. Overflowing it disengages rather than truncating:
// half a greyed bar is a worse answer than none.
constexpr uint32_t kMaxButtons = 32;

struct State {
    bool     engaged     = false;        // this tick's verdict, for the Button hook
    void*    button[kMaxButtons] = {};
    uint32_t buttons     = 0;
    void*    aimed_btn   = nullptr;      // button currently aimed by the cat's owner (left un-dimmed)
    void*    hovered_btn = nullptr;      // button currently hovered by the cat's owner (left un-dimmed)

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

    fn_get_world_pos get_world_pos = nullptr;
    bool             resolved_gwp  = false;
};

State g;

bool safe_get_world_pos(void* renderer, Vec2D local, void* disp_obj, Vec2D& out) {
    if (!g.resolved_gwp) {
        g.get_world_pos = (fn_get_world_pos)addr_of_call(C_RendererGetWorldPos);
        g.resolved_gwp  = true;
    }
    if (!g.get_world_pos || !renderer || !disp_obj) return false;
    __try {
        g.get_world_pos(renderer, &out, &local, disp_obj, 1.0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool button_contains_norm(void* button, float nx, float ny) {
    if (!button) return false;
    void* renderer = nullptr;
    uint64_t gen = 0, live = 0;
    if (!mem_read((const uint8_t*)button + kBtn_Renderer, &renderer, sizeof(renderer)) || !renderer)
        return false;
    if (!mem_read((const uint8_t*)button + kBtn_Renderer + 8, &gen, sizeof(gen))) return false;
    if (!mem_read((const uint8_t*)renderer - kRefGenOffset, &live, sizeof(live)) || live != gen)
        return false;

    void* clip = nullptr;
    if (!mem_read((const uint8_t*)button + kBtn_Clip, &clip, sizeof(clip)) || !clip) return false;

    float b[4] = {}; // xMin, xMax, yMin, yMax
    if (!mem_read((const uint8_t*)button + kBtn_Bounds, b, sizeof(b))) return false;
    if (b[1] <= b[0] || b[3] <= b[2]) return false;

    Vec2D p0{}, p1{};
    if (!safe_get_world_pos(renderer, Vec2D{ b[0], b[2] }, clip, p0)) return false;
    if (!safe_get_world_pos(renderer, Vec2D{ b[1], b[3] }, clip, p1)) return false;

    float x0 = (float)(p0.x / 1280.0);
    float y0 = (float)(p0.y / 720.0);
    float x1 = (float)(p1.x / 1280.0);
    float y1 = (float)(p1.y / 720.0);
    if (x0 > x1) { float t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { float t = y0; y0 = y1; y1 = t; }
    if (x1 - x0 < 0.005f || y1 - y0 < 0.005f) return false;
    if (x0 < -0.2f || x1 > 1.2f || y0 < -0.2f || y1 > 1.2f) return false;

    return nx >= x0 && nx <= x1 && ny >= y0 && ny <= y1;
}

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
    g.engaged     = false;
    g.held        = false;
    g.buttons     = 0;
    g.aimed_btn   = nullptr;
    g.hovered_btn = nullptr;
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

    // If the owning peer is actively aiming an ability, un-dim its button on
    // the bar so teammates can see which ability is being aimed.
    // CombatMenu::buttons order (CombatMenu::init):
    //   0: bMove, 1: bAttack, 2..5: bSpell0..3, 6: bBonusSpell, 7..11: bItem0..4
    uint8_t sk = SLOT_NONE, si = 0;
    if (aim_active_slot_for(subject, &sk, &si)) {
        uint32_t bidx = UINT32_MAX;
        if (sk == SLOT_MOVE)        bidx = 0;
        else if (sk == SLOT_ATTACK) bidx = 1;
        else if (sk == SLOT_BONUS)  bidx = 6;
        else if (sk == SLOT_SPELL) {
            if (si < 4) {
                bidx = 2u + si;
            } else if (void* ab = ability_from_slot(subject, AbilitySlot{ sk, si })) {
                uint8_t is_weapon = 0, is_trinket = 0;
                mem_read((const uint8_t*)ab + 0x8A1, &is_weapon, 1);
                mem_read((const uint8_t*)ab + 0x8A2, &is_trinket, 1);
                if (is_weapon)       bidx = 10; // bItem3 (weapon)
                else if (is_trinket) bidx = 11; // bItem4 (trinket)
            }
        }
        if (bidx < g.buttons) g.aimed_btn = g.button[bidx];
    }

    // If the owning peer's screen-space cursor is hovering over a button on the
    // combat bar, un-dim that button so teammates see them hover it in real time.
    float ox = 0.0f, oy = 0.0f;
    if (overlay_owner_pointer(&ox, &oy)) {
        for (uint32_t i = 0; i < g.buttons; ++i) {
            if (button_contains_norm(g.button[i], ox, oy)) {
                g.hovered_btn = g.button[i];
                break;
            }
        }
    }

    g.engaged = true;
    g.held    = true;
}

void combatlock_leave() {
    g.engaged     = false;
    g.aimed_btn   = nullptr;
    g.hovered_btn = nullptr;
}

void combatlock_on_button(void* button) {
    if (!g.engaged || !button) return;
    if (!is_ours(button)) return; // some other button updating re-entrantly
    if (button == g.aimed_btn || button == g.hovered_btn) return; // keep owner's hovered/aimed ability bright

    const int32_t disabled = kBtnState_Disabled;
    mem_write((uint8_t*)button + kBtn_State, &disabled, sizeof(disabled));
}

void combatlock_on_turn_portrait(void* portrait) {
    if (!tune::kCombatLock || !portrait) return;

    // Leave portraits that are fading out (-1) or round dividers (kind == 8) alone.
    int32_t turn_idx = 0;
    if (!mem_read((const uint8_t*)portrait + kTOP_TurnIdx, &turn_idx, sizeof(turn_idx)) ||
        turn_idx == -1)
        return;

    int32_t kind = 0;
    if (!mem_read((const uint8_t*)portrait + kTOP_Kind, &kind, sizeof(kind)) ||
        kind == 8)
        return;

    void* renderer = nullptr;
    if (!mem_read((const uint8_t*)portrait + kTOP_Renderer, &renderer, sizeof(renderer)) ||
        !renderer)
        return;

    void* ch = nullptr;
    uint64_t gen = 0, live = 0;
    if (!mem_read((const uint8_t*)portrait + kTOP_Subject, &ch, sizeof(ch)) || !ch)
        return;
    if (!mem_read((const uint8_t*)portrait + kTOP_SubjGen, &gen, sizeof(gen)))
        return;
    if (!mem_read((const uint8_t*)ch - kRefGenOffset, &live, sizeof(live)) || live != gen)
        return;

    const double alpha = lockstep_peer_owns_character(ch) ? 0.45 : 1.0;
    mem_write((uint8_t*)renderer + kRenderer_Alpha, &alpha, sizeof(alpha));
}

bool     combatlock_engaged() { return g.held; }
uint32_t combatlock_buttons() { return g.buttons; }

} // namespace mgmp
