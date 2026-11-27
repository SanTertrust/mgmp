// mgmp_draft.cpp -- Adventure cat ownership draft overlay (see mgmp_draft.h).

#include "mgmp_draft.h"
#include "mgmp_gameui.h"
#include "mgmp_savefile.h"
#include "mgmp_leave.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace mgmp {
namespace {

using fn_cat_by_id = void* (__fastcall*)(void* registry, uint64_t id);

// Player colors matching mgmp_overlay.cpp peer cursor palette.
GameUiColor peer_color(uint8_t peer_id) {
    switch (peer_id) {
        case 0:  return { 0.35f, 0.85f, 1.00f, 1.0f }; // Host: cyan
        case 1:  return { 1.00f, 0.55f, 0.30f, 1.0f }; // P2: coral/orange
        case 2:  return { 0.45f, 0.95f, 0.55f, 1.0f }; // P3: mint green
        case 3:  return { 1.00f, 0.85f, 0.30f, 1.0f }; // P4: gold
        default: return { 0.85f, 0.85f, 0.85f, 1.0f };
    }
}

struct CardUi {
    uint64_t        cat_uid = 0;
    double          cx      = 0.0;
    double          cy      = 0.0;
    double          scale   = 1.0;
    wchar_t         name[64] = L"Cat";
    GameUiClip      bg;
    GameUiCat       cat;
    GameUiCatKit    kit;
    GameUiCombatBar icons;
    GameUiLabel     name_lbl;
    GameUiLabel     class_lbl;
    GameUiLabel     owner_lbl;
    uint8_t         shown_owner = 0xFE; // force initial label refresh
    bool            hovered     = false;
};

struct State {
    DraftModel    model;
    uint32_t      next_gen = 1;

    // Host: held map node waiting for draft completion.
    bool          has_held_node   = false;
    uint32_t      held_node_index = 0;
    bool          node_ready      = false;

    // Native Game UI overlay state.
    GameUiOverlay   ov;
    GameUiClip      backdrop;
    GameUiLabel     title_lbl;
    GameUiLabel     status_lbl;
    void*           tooltip_comp = nullptr;
    CardUi          cards[kMaxDraftCats];
    uint8_t         card_count    = 0;
    uint32_t        ui_gen        = 0;
    bool            ui_dirty      = false;
    bool            prev_lmb      = false;

    fn_cat_by_id  cat_by_id  = nullptr;
};

State g;

bool window_is_focused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// The draft overlay must only be shown once this peer is actually on the
// Adventure Map with the run loaded (never over MainMenu, SaveSelection,
// House/Prep, mid-save-reload, or while the Map is suspended).
bool can_show_draft_ui() {
    if (!savefile_adventure_is_loaded()) return false;
    if (savefile_has_pending()) return false;
    bool map_suspended = false;
    if (!leave_find_scene("Map", nullptr, &map_suspended) || map_suspended)
        return false;
    return true;
}

void* resolve_cat_data(uint64_t uid) {
    if (!g.cat_by_id)
        g.cat_by_id = (fn_cat_by_id)addr_of_call(C_CatDataById);
    if (!g.cat_by_id) return nullptr;

    const uintptr_t slot = addr_of_data(D_MewDirectorPtr);
    if (!slot) return nullptr;
    void* dir = nullptr;
    if (!mem_read((const void*)slot, &dir, sizeof(dir)) || !dir) return nullptr;
    void* reg = nullptr;
    if (!mem_read((const uint8_t*)dir + kDir_CatRegistry, &reg, sizeof(reg)) || !reg)
        return nullptr;

    __try {
        return g.cat_by_id(reg, uid);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

uint8_t read_party_cat_uids(uint64_t* out, uint8_t cap) {
    const uintptr_t slot = addr_of_data(D_MewDirectorPtr);
    if (!slot) return 0;
    void* dir = nullptr;
    if (!mem_read((const void*)slot, &dir, sizeof(dir)) || !dir) return 0;

    uint32_t count = 0;
    const uint64_t* data = nullptr;
    if (!mem_read((const uint8_t*)dir + kDir_CatIdCount, &count, sizeof(count))) return 0;
    if (!mem_read((const uint8_t*)dir + kDir_CatIdData,  &data,  sizeof(data)))  return 0;
    if (!data || count == 0 || count > 32) return 0;

    uint8_t n = 0;
    for (uint32_t i = 0; i < count && n < cap; ++i) {
        uint64_t uid = 0;
        if (!mem_read(data + i, &uid, sizeof(uid)) || uid == 0) continue;
        // Verify CatData exists in registry.
        if (!resolve_cat_data(uid)) continue;
        out[n++] = uid;
    }
    return n;
}

bool peer_is_connected(uint8_t peer) {
    uint8_t ids[kMaxPeers] = {};
    const uint8_t n = net_peer_count();
    if (n == 0 || !net_peer_ids(ids, (uint8_t)kMaxPeers)) return false;
    for (uint8_t i = 0; i < n && i < kMaxPeers; ++i)
        if (ids[i] == peer) return true;
    return false;
}

void format_peer_label(uint8_t owner, uint8_t self_id, bool hovered, bool can_pick,
                       wchar_t* out, size_t cap, GameUiColor& color) {
    if (owner == kNoPeer) {
        if (hovered && can_pick) {
            swprintf_s(out, cap, L"[ PICK ]");
            color = { 1.0f, 0.95f, 0.45f, 1.0f };
        } else {
            swprintf_s(out, cap, can_pick ? L"Click to pick" : L"Unclaimed");
            color = { 0.80f, 0.80f, 0.80f, 1.0f };
        }
        return;
    }

    color = peer_color(owner);
    if (owner == self_id) {
        swprintf_s(out, cap, hovered ? L"YOU (unpick)" : L"YOU");
    } else if (owner == 0) {
        swprintf_s(out, cap, L"HOST");
    } else {
        swprintf_s(out, cap, L"PLAYER %u", (unsigned)(owner + 1));
    }
}

void close_ui() {
    if (g.ov.open)
        gameui_close(g.ov);
    g.tooltip_comp = nullptr;
    g.card_count   = 0;
    g.ui_gen       = 0;
    g.ui_dirty     = false;
    g.prev_lmb     = false;
}

void refresh_ui_labels() {
    if (!gameui_is_alive(g.ov)) return;

    const uint8_t self_id = net_self();
    const uint8_t quota   = g.model.quota_of(self_id);
    const uint8_t picked  = g.model.picked_of(self_id);
    const uint8_t left    = (quota > picked) ? (uint8_t)(quota - picked) : 0;
    const bool    can_pick = (left > 0);

    wchar_t status[128] = {};
    if (left > 0) {
        swprintf_s(status, 128, L"Pick %u %s  (%u/%u total)",
                   (unsigned)left,
                   left == 1 ? L"cat" : L"cats",
                   (unsigned)g.model.total_picked(),
                   (unsigned)g.model.total_quota());
        gameui_set_label_color(g.status_lbl, { 1.0f, 0.95f, 0.65f, 1.0f });
    } else {
        swprintf_s(status, 128, L"Ready! Waiting for others (%u/%u)  - click yours to unpick",
                   (unsigned)g.model.total_picked(),
                   (unsigned)g.model.total_quota());
        gameui_set_label_color(g.status_lbl, { 0.65f, 1.0f, 0.72f, 1.0f });
    }
    gameui_set_label_text(g.status_lbl, status);

    for (uint8_t i = 0; i < g.card_count && i < g.model.count; ++i) {
        CardUi& c = g.cards[i];
        const uint8_t own = g.model.owner[i];
        const bool clickable = (own == self_id) || (own == kNoPeer && can_pick);

        wchar_t badge[64] = {};
        GameUiColor col{};
        format_peer_label(own, self_id, c.hovered, can_pick, badge, 64, col);
        gameui_set_label_text(c.owner_lbl, badge);
        gameui_set_label_color(c.owner_lbl, col);

        // Card scale & alpha feedback.
        const double hover_mul = (c.hovered && clickable) ? 1.06 : 1.0;
        gameui_set_scale(c.bg.transform, 1.50 * c.scale * hover_mul, 2.00 * c.scale * hover_mul);
        const double alpha = (own != kNoPeer && own != self_id) ? 0.55 : 1.0;
        gameui_set_alpha(c.bg.renderer, alpha);
        if (c.cat.renderer) gameui_set_alpha(c.cat.renderer, alpha);
        if (c.icons.renderer) gameui_set_alpha(c.icons.renderer, alpha);

        c.shown_owner = own;
    }
    g.ui_dirty = false;
}

bool build_ui() {
    close_ui();
    if (!g.model.active || g.model.count == 0) return false;
    if (!gameui_open(g.ov, "MGMP_CatDraft", 999990.0)) return false;

    // Native modal backdrop ("ConfirmationBox" with yes/no/prompt hidden),
    // framing the cards and their surrounding ability/passive/gear icons.
    if (gameui_add_clip(g.ov, "ConfirmationBox", 640.0, 360.0, 1.58, 1.32, 10, 0.96, g.backdrop)) {
        gameui_set_child_visible(g.backdrop.renderer, "yes", false);
        gameui_set_child_visible(g.backdrop.renderer, "no",  false);
        gameui_set_child_text(g.backdrop.renderer, "prompt", L"");
    }

    // Header & status text in native game font.
    // Note: HouseNametag renders its text ~40*scale px above its transform y.
    gameui_add_label(g.ov, L"CHOOSE YOUR CATS", 640.0, 74.0, 1.85, 20,
                     { 1.0f, 1.0f, 1.0f, 1.0f }, g.title_lbl);
    gameui_add_label(g.ov, L"", 640.0, 120.0, 1.30, 20,
                     { 1.0f, 0.95f, 0.65f, 1.0f }, g.status_lbl);

    // One shared AbilityTooltip drives hover descriptions for every icon on
    // every cat card in the overlay.
    g.tooltip_comp = gameui_add_ability_tooltip(g.ov);

    // Lay out 1..N cat cards evenly with space on left/right/bottom for each
    // cat's own abilities, passives, and equipment icons.
    const uint8_t n = g.model.count;
    const double base_slot_w = 296.0;
    const double fit = (n * base_slot_w > 1200.0) ? (1200.0 / (n * base_slot_w)) : 1.0;
    const double slot_w  = base_slot_w * fit;
    const double total_w = n * slot_w;
    const double start_x = 640.0 - total_w * 0.5 + slot_w * 0.5;
    const double card_y  = 312.0;

    g.card_count = 0;
    for (uint8_t i = 0; i < n; ++i) {
        CardUi& c = g.cards[i];
        c = {};
        c.cat_uid = g.model.cat_uid[i];
        c.cx      = start_x + i * slot_w;
        c.cy      = card_y;
        c.scale   = fit;

        gameui_add_clip(g.ov, "InventoryGridBGBox", c.cx, c.cy,
                        1.50 * fit, 2.00 * fit, 15, 1.0, c.bg);

        void* cd = resolve_cat_data(c.cat_uid);
        wchar_t name_buf[64] = L"Cat";
        wchar_t cls_buf[64]  = L"";
        if (cd) {
            gameui_add_cat(g.ov, cd, c.cx, c.cy + 36.0 * fit, 1.05 * fit, 22, c.cat);

            wchar_t raw_name[48] = {};
            if (gameui_read_wstring((const uint8_t*)cd + kCatData_Name, raw_name, 48) && raw_name[0])
                wcsncpy_s(name_buf, raw_name, _TRUNCATE);

            char cls_a[32] = {};
            int32_t lvl = 1;
            gameui_read_string((const uint8_t*)cd + kCatData_Class, cls_a, sizeof(cls_a));
            mem_read((const uint8_t*)cd + kCatData_Level, &lvl, sizeof(lvl));
            if (cls_a[0]) {
                // Switch cat pose to its class-specific front idle if available
                // (e.g. "FighterIdleF", "MageIdleF"); otherwise keep "idleF".
                char anim_label[48] = {};
                snprintf(anim_label, sizeof(anim_label), "%sIdleF", cls_a);
                gameui_goto_label(c.cat.renderer, nullptr, anim_label);

                wchar_t cls_w[32] = {};
                for (int k = 0; k < 31 && cls_a[k]; ++k) cls_w[k] = (wchar_t)(unsigned char)cls_a[k];
                swprintf_s(cls_buf, 64, L"%ls  Lv.%d", cls_w, (int)lvl);
            } else {
                swprintf_s(cls_buf, 64, L"Lv.%d", (int)lvl);
            }

            // Surround this card with its own abilities (left), passives
            // (right), and equipped gear (bottom).
            gameui_add_cat_kit_icons(g.ov, cd, c.cx, c.cy, fit, c.icons, c.kit);
        }
        wcsncpy_s(c.name, name_buf, _TRUNCATE);

        gameui_add_label(g.ov, name_buf, c.cx, c.cy - 88.0 * fit, 1.45 * fit, 25,
                         { 1.0f, 1.0f, 1.0f, 1.0f }, c.name_lbl);
        gameui_add_label(g.ov, cls_buf, c.cx, c.cy - 36.0 * fit, 1.12 * fit, 25,
                         { 0.90f, 0.90f, 0.90f, 1.0f }, c.class_lbl);
        gameui_add_label(g.ov, L"", c.cx, c.cy + 120.0 * fit, 1.30 * fit, 25,
                         { 0.85f, 0.85f, 0.85f, 1.0f }, c.owner_lbl);

        ++g.card_count;
    }

    g.ui_gen   = g.model.generation;
    g.prev_lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    refresh_ui_labels();
    return true;
}

void log_final_assignment() {
    char buf[256] = {};
    int pos = 0;
    for (uint8_t i = 0; i < g.model.count; ++i) {
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s%llu->P%u",
                        i ? ", " : "",
                        (unsigned long long)g.model.cat_uid[i],
                        (unsigned)(g.model.owner[i] + 1));
    }
    log_line("DRAFT", "cat ownership finalized (gen=%u): %s", g.model.generation, buf);
}

void broadcast_state() {
    DraftStateMsg m = g.model.to_msg();
    net_send_draft_state(m);
}

} // namespace

// ---------------------------------------------------------------------------

void draft_init() {
    draft_reset();
    g.cat_by_id = (fn_cat_by_id)addr_of_call(C_CatDataById);
}

void draft_shutdown() {
    close_ui();
    g.model.clear();
    g.has_held_node = false;
    g.node_ready    = false;
}

void draft_reset() {
    close_ui();
    g.model.clear();
    g.has_held_node = false;
    g.node_ready    = false;
}

bool draft_needs_selection() {
    if (!net_active()) return false;
    if (net_role() != NetRole::Host) return false;
    if (net_peer_count() < 2) return false;
    if (g.model.decided || g.model.active) return false;
    if (!gameui_available()) return false;
    return true;
}

bool draft_is_active() {
    return g.model.active;
}

bool draft_begin_host(uint32_t node_index) {
    if (net_role() != NetRole::Host || !net_active()) return false;
    if (!gameui_available()) {
        log_line("DRAFT", "native UI unavailable -- falling back to automatic split");
        return false;
    }

    uint8_t peers[kMaxPeers] = {};
    const uint8_t peer_n = net_peer_count();
    if (peer_n < 2 || !net_peer_ids(peers, (uint8_t)kMaxPeers)) return false;

    uint64_t uids[kMaxDraftCats] = {};
    const uint8_t cat_n = read_party_cat_uids(uids, (uint8_t)kMaxDraftCats);
    if (cat_n < 2) {
        log_line("DRAFT", "party has %u cat(s) -- skipping draft", (unsigned)cat_n);
        return false;
    }

    const uint32_t gen = g.next_gen++;
    if (!g.model.start(gen, uids, cat_n, peers, (uint8_t)peer_n))
        return false;

    g.has_held_node   = true;
    g.held_node_index = node_index;
    g.node_ready      = false;

    log_line("DRAFT", "starting cat ownership draft gen=%u (%u cats, %u players) before node %u",
             gen, (unsigned)cat_n, peer_n, node_index);

    broadcast_state();
    if (can_show_draft_ui())
        build_ui();
    return true;
}

bool draft_take_ready_node(uint32_t* out_node_index) {
    if (!g.node_ready || !g.has_held_node) return false;
    if (out_node_index) *out_node_index = g.held_node_index;
    g.node_ready    = false;
    g.has_held_node = false;
    return true;
}

void draft_tick() {
    if (!g.model.active) {
        if (g.ov.open) close_ui();
        return;
    }

    if (!net_active()) {
        draft_reset();
        return;
    }

    if (!can_show_draft_ui()) {
        if (g.ov.open) close_ui();
        return;
    }

    // Open or rebuild UI if needed.
    if (!gameui_is_alive(g.ov) || g.ui_gen != g.model.generation) {
        if (!build_ui()) return;
    }

    double mx = -1000.0, my = -1000.0;
    const bool have_mouse = gameui_get_mouse(g.ov, mx, my);
    const bool focused    = window_is_focused();
    const bool lmb_now    = focused && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    const bool lmb_click  = lmb_now && !g.prev_lmb;
    g.prev_lmb = lmb_now;

    int clicked_idx = -1;
    bool hover_changed = false;
    for (uint8_t i = 0; i < g.card_count; ++i) {
        const double hit_w = 192.0 * g.cards[i].scale;
        const double hit_h = 228.0 * g.cards[i].scale;
        const bool h = have_mouse && gameui_hit_rect(mx, my, g.cards[i].cx, g.cards[i].cy, hit_w, hit_h);
        if (h != g.cards[i].hovered) {
            g.cards[i].hovered = h;
            hover_changed = true;
        }
        if (h && lmb_click) clicked_idx = (int)i;
    }

    if (hover_changed || g.ui_dirty)
        refresh_ui_labels();

    if (clicked_idx >= 0 && clicked_idx < (int)g.model.count) {
        const uint64_t uid = g.model.cat_uid[clicked_idx];
        if (net_role() == NetRole::Host) {
            if (g.model.toggle_pick(net_self(), uid)) {
                broadcast_state();
                if (!g.model.active) {
                    log_final_assignment();
                    close_ui();
                    if (g.has_held_node) g.node_ready = true;
                } else {
                    g.ui_dirty = true;
                    refresh_ui_labels();
                }
            }
        } else {
            DraftPickMsg m{};
            m.generation = g.model.generation;
            m.cat_uid    = uid;
            net_send_draft_pick(m);
        }
    }
}

void draft_on_pick(uint8_t from_peer, const DraftPickMsg& m) {
    if (net_role() != NetRole::Host) return;
    if (!g.model.active || m.generation != g.model.generation) return;

    if (g.model.toggle_pick(from_peer, m.cat_uid)) {
        broadcast_state();
        if (!g.model.active) {
            log_final_assignment();
            close_ui();
            if (g.has_held_node) g.node_ready = true;
        } else {
            g.ui_dirty = true;
        }
    }
}

void draft_on_state(const DraftStateMsg& m) {
    if (net_role() == NetRole::Host) return;

    const bool was_active = g.model.active;
    g.model.apply_msg(m);

    if (g.model.active) {
        if (!was_active) {
            log_line("DRAFT", "<- host started cat ownership draft gen=%u (%u cats, %u players)",
                     m.generation, (unsigned)m.count, (unsigned)m.peer_count);
        }
        g.ui_dirty = true;
    } else {
        if (was_active) {
            close_ui();
            if (g.model.decided) log_final_assignment();
            else log_line("DRAFT", "<- host cancelled cat ownership draft");
        }
    }
}

void draft_catchup(uint8_t peer) {
    if (net_role() != NetRole::Host || !net_active()) return;
    if (!g.model.active && !g.model.decided) return;
    DraftStateMsg m = g.model.to_msg();
    net_send_draft_state_to(peer, m);
    log_line("DRAFT", "-> sent draft state (gen=%u active=%u) to peer %u",
             m.generation, (unsigned)m.active, (unsigned)peer);
}

void draft_on_peer_disconnected(uint8_t peer) {
    if (net_role() != NetRole::Host) return;
    if (!g.model.active) return;

    if (g.model.on_peer_left(peer)) {
        broadcast_state();
        if (!g.model.active) {
            close_ui();
            if (g.model.decided) log_final_assignment();
            else log_line("DRAFT", "draft cancelled after peer %u disconnected", (unsigned)peer);
            if (g.has_held_node) g.node_ready = true;
        } else {
            g.ui_dirty = true;
        }
    }
}

bool draft_lookup_owner(uint64_t cat_uid, uint8_t* out_peer) {
    uint8_t owner = kNoPeer;
    if (!g.model.lookup_owner(cat_uid, &owner)) return false;
    if (!peer_is_connected(owner)) return false;
    if (out_peer) *out_peer = owner;
    return true;
}

} // namespace mgmp
