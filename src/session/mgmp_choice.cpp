// mgmp_choice -- replicate the player's choice on WorldEvent and LevelUpScreen,
// guard non-owner Shop/Adventure-Inventory clicks, grey out non-owner choice
// buttons while keeping them clickable, and broadcast multi-peer teammate votes.
#include "mgmp_choice.h"
#include "mgmp_draft.h"
#include "mgmp_follow.h"
#include "mgmp_savefile.h"
#include "mgmp_net.h"
#include "mgmp_tuning.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_addresses.h"
#include "mgmp_hooks.h"
#include "mgmp_resolve.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <deque>

namespace mgmp {
namespace {

// Struct offsets for WorldEvent, LevelUpScreen, MenuPanel, Button, DisplayObject,
// Shop, and Transition live in mgmp_addresses.h.
constexpr uint32_t kMaxOptions     = 64;
constexpr uint32_t kMaxVoteButtons = 16;

struct Vec2D { double x, y; };
struct SwfColorTransform {
    float mul_r, mul_g, mul_b, mul_a;
    float add_r, add_g, add_b, add_a;
};

using fn_cap           = void   (__fastcall*)(void*);
using fn_get_world_pos = Vec2D* (__fastcall*)(void* renderer, Vec2D* out, const Vec2D* local,
                                              void* disp_obj, double scale);
using fn_cat_by_id = void* (__fastcall*)(void*, uint64_t);

struct ShopExitCapture {
    void* vtable;
    void* shop;
};

// std::function capture used by Shop::ObtainItem's candy transition.
// The callback reads the PodBuffer<8> at +0x10 and never frees it itself.
struct ShopCandyCapture {
    void* vtable;
    void* shop;
    uint32_t capacity;
    uint32_t count;
    void** cats;
};

struct VoteEntry {
    bool     active    = false;
    uint8_t  kind      = 0;      // kChoiceEventVote or kChoiceLevelUpVote
    uint32_t index     = 0;
    uint64_t node_seed = 0;
    uint64_t cat_uid   = 0;
};

struct BtnRect {
    bool  valid = false;
    float nx0 = 0.0f, ny0 = 0.0f, nx1 = 0.0f, ny1 = 0.0f;
};

struct State {
    bool on        = false;
    bool is_client = false;

    uintptr_t        base          = 0;
    bool             resolved      = false;
    fn_cap           evt_commit    = nullptr;   // WorldEvent option commit lambda
    fn_cap           lvl_click     = nullptr;   // LevelUpScreen button click lambda
    fn_cap           lvl_reroll    = nullptr;   // LevelUpScreen::Reroll
    fn_get_world_pos get_world_pos = nullptr;   // Renderer::get_world_position
    fn_cat_by_id     cat_by_id     = nullptr;
    const void*     transition_slot = nullptr;

    void* world_event  = nullptr;   // latest WorldEvent*, from its update

    // Pending WorldEvent choice (at most one event screen per node).
    bool     pending_evt        = false;
    uint64_t pending_evt_seed   = 0;
    uint32_t pending_evt_index  = 0;
    uint32_t pending_evt_count  = 0;
    char     pending_evt_name[48] = {};
    bool     pending_evt_warned = false;

    // Pending LevelUpScreen actions (select + reroll), queued per (node_seed, cat_uid)
    // in arrival order so back-to-back level-ups and rerolls never overwrite each other.
    static constexpr uint32_t kMaxPendingLvl = 32;
    struct PendingLvl {
        ChoiceMsg msg;
        bool      warned = false;
    };
    PendingLvl lvl_q[kMaxPendingLvl];
    uint32_t   lvl_q_count = 0;

    std::deque<ChoiceMsg> shop_actions;
    std::deque<ChoiceMsg> ac_actions;
    void*                 last_lvl_scene   = nullptr;
    uint64_t              last_lvl_cat_uid = 0;
    ChoiceMsg candy_purchase{};
    void* candy_shop = nullptr;
    bool candy_level_opening = false;

    uint64_t here_seed = 0;

    static constexpr uint32_t kSkippedRing = 16;
    uint64_t skipped[kSkippedRing] = {};
    uint32_t skipped_next          = 0;

    // Per-peer active votes on the current Event or LevelUpScreen, plus
    // cached screen-space button rects updated every tick of the active screen.
    VoteEntry votes[kMaxPeers];
    BtnRect   btn_rects[kMaxVoteButtons];
    uint8_t   btn_vote_kind = 0;
    uint64_t  btn_node_seed = 0;
    uint64_t  btn_cat_uid   = 0;
    uint64_t  btn_rects_ms  = 0;

    bool injecting = false;

    uint32_t sent      = 0;
    uint32_t applied   = 0;
    uint32_t swallowed = 0;

    CRITICAL_SECTION cs;
    bool cs_ready = false;
};

State g;

struct Guard {
    Guard()  { if (g.cs_ready) EnterCriticalSection(&g.cs); }
    ~Guard() { if (g.cs_ready) LeaveCriticalSection(&g.cs); }
};

const char* kind_name(uint8_t k) {
    if (k == kChoiceLevelUp)       return "level-up";
    if (k == kChoiceLevelUpReroll) return "level-up reroll";
    if (k == kChoiceEventVote)     return "event vote";
    if (k == kChoiceLevelUpVote)   return "level-up vote";
    if (k == kChoiceShopPurchase)  return "shop purchase";
    if (k == kChoiceShopExit)      return "shop exit";
    if (k == kChoiceAbilityReplace) return "ability replace";
    return "event";
}

void clear_votes() {
    for (auto& v : g.votes) v = VoteEntry{};
}

uint64_t read_screen_cat_uid(const void* screen, uintptr_t cat_ptr_off) {
    if (!screen) return 0;
    void* cd = nullptr;
    uint64_t uid = 0;
    if (!mem_read((const uint8_t*)screen + cat_ptr_off, &cd, sizeof(cd)) || !cd) return 0;
    if (!mem_read((const uint8_t*)cd + kCatData_Uid, &uid, sizeof(uid))) return 0;
    return uid;
}

void lvl_q_erase(uint32_t idx) {
    if (idx >= g.lvl_q_count) return;
    for (uint32_t i = idx; i + 1 < g.lvl_q_count; ++i)
        g.lvl_q[i] = g.lvl_q[i + 1];
    --g.lvl_q_count;
}

void remember_skipped(uint64_t seed) {
    if (!seed) return;
    for (uint32_t i = 0; i < State::kSkippedRing; ++i)
        if (g.skipped[i] == seed) return;
    g.skipped[g.skipped_next] = seed;
    g.skipped_next = (g.skipped_next + 1) % State::kSkippedRing;
}

bool was_skipped(uint64_t seed) {
    if (!seed) return false;
    for (uint32_t i = 0; i < State::kSkippedRing; ++i)
        if (g.skipped[i] == seed) return true;
    return false;
}

// --- reading a std::string out of the game ---------------------------------
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

template <typename T>
bool read_field(const void* object, uintptr_t offset, T& value) {
    return object && mem_read(static_cast<const uint8_t*>(object) + offset, &value, sizeof(value));
}

bool sync_shop(void* shop) {
    uint8_t treasure = 0, house = 0;
    return g.on && net_active() && g.here_seed && g.transition_slot && g.cat_by_id &&
           hooks_is_live(T_ShopBuyItem) && hooks_is_live(T_ShopUpdate) &&
           hooks_is_live(T_ShopExit) && hooks_is_live(T_ShopCandyLevel) &&
           read_field(shop, kShop_IsTreasure, treasure) && !treasure &&
           read_field(shop, kShop_IsHouse, house) && !house;
}

bool shop_transition_is_idle() {
    const void* transition = nullptr;
    uint8_t active = 0;
    return mem_read(g.transition_slot, &transition, sizeof(transition)) &&
           read_field(transition, kTransition_Active, active) && !active;
}

bool read_shop_items(void* shop, uintptr_t& begin, uint32_t& count) {
    uintptr_t end = 0;
    if (!read_field(shop, kShop_Items, begin) ||
        !read_field(shop, kShop_Items + sizeof(begin), end) ||
        !begin || end < begin || (end - begin) % kShop_ItemSize != 0)
        return false;
    const uintptr_t size = (end - begin) / kShop_ItemSize;
    if (size > 256) return false;
    count = static_cast<uint32_t>(size);
    return true;
}

bool read_shop_item(void* item, ChoiceMsg& purchase, int32_t& stock) {
    int32_t cost = 0;
    if (!read_field(item, kShopItem_Type, purchase.aux) ||
        !read_field(item, kShopItem_Cost, cost) || cost < 0 ||
        !read_field(item, kShopItem_Stock, stock) ||
        !read_game_string(static_cast<const uint8_t*>(item) + kShopItem_Data,
                          purchase.name, sizeof(purchase.name)))
        return false;
    purchase.count = static_cast<uint32_t>(cost);
    return true;
}

void* shop_cat_by_uid(void* shop, uint64_t uid) {
    const void* director = nullptr;
    void* registry = nullptr;
    if (!uid || !read_field(shop, kShop_Director, director) ||
        !read_field(director, kDir_CatRegistry, registry) || !registry)
        return nullptr;
    return g.cat_by_id(registry, uid);
}

void send_shop_action(const ChoiceMsg& action) {
    if (!net_send_choice(action)) {
        log_line("CHOICE", "!! failed to publish shop action %u", action.kind);
        return;
    }
    ++g.sent;
    if (action.kind == kChoiceShopPurchase)
        log_line("CHOICE", "-> shop purchase slot %u, kind %u, cat=%016llx",
                 action.index, action.aux, (unsigned long long)action.cat_uid);
    else
        log_line("CHOICE", "-> shop exit");
}

void publish_candy_level(void* level_screen) {
    if (!g.candy_level_opening) return;
    const uint64_t uid = read_screen_cat_uid(level_screen, kLvl_Cat);
    if (!uid) return;
    g.candy_level_opening = false;
    g.candy_shop = nullptr;
    g.candy_purchase.cat_uid = uid;
    send_shop_action(g.candy_purchase);
}

void discard_shop_actions(uint64_t node_seed) {
    const auto first = std::remove_if(g.shop_actions.begin(), g.shop_actions.end(),
        [node_seed](const ChoiceMsg& m) { return m.node_seed == node_seed; });
    g.shop_actions.erase(first, g.shop_actions.end());
}

// --- button greying & screen-rect caching for multi-peer votes -------------

bool safe_get_world_pos(void* renderer, Vec2D local, void* disp_obj, Vec2D& out) {
    if (!g.get_world_pos || !renderer || !disp_obj) return false;
    __try {
        g.get_world_pos(renderer, &out, &local, disp_obj, 1.0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void tint_button_clip(void* button, bool grey_out) {
    if (!button) return;
    void* clip = nullptr;
    if (!mem_read((const uint8_t*)button + kBtn_Clip, &clip, sizeof(clip)) || !clip)
        return;
    const SwfColorTransform ct = grey_out
        ? SwfColorTransform{ 0.42f, 0.42f, 0.42f, 0.78f, 0.12f, 0.12f, 0.12f, 0.0f }
        : SwfColorTransform{ 1.00f, 1.00f, 1.00f, 1.00f, 0.00f, 0.00f, 0.00f, 0.0f };
    mem_write((uint8_t*)clip + kDispObj_CT, &ct, sizeof(ct));
}

bool read_button_rect(void* button, BtnRect& out) {
    out = BtnRect{};
    if (!button || !g.get_world_pos) return false;
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

    out.valid = true;
    out.nx0 = x0; out.ny0 = y0;
    out.nx1 = x1; out.ny1 = y1;
    return true;
}

// Walk MenuPanel::buttons (MSVC std::map<std::string, Button*>) read-only:
// 1. Grey out option buttons (and reroll) when !local_owns while keeping them clickable.
// 2. Cache screen-space rects of numbered option buttons for multi-peer vote markers.
void sync_choice_panel(void* screen, uintptr_t panel_off, const char* prefix,
                       bool local_owns, uint8_t vote_kind,
                       uint64_t node_seed, uint64_t cat_uid) {
    if (!screen) return;
    void* panel = nullptr;
    if (!mem_read((const uint8_t*)screen + panel_off, &panel, sizeof(panel)) || !panel)
        return;

    void* head = nullptr;
    uint64_t count = 0;
    if (!mem_read((const uint8_t*)panel + kPanel_Buttons, &head, sizeof(head)) || !head)
        return;
    if (!mem_read((const uint8_t*)panel + kPanel_Buttons + 8, &count, sizeof(count)) ||
        count == 0 || count > 128)
        return;

    for (auto& r : g.btn_rects) r = BtnRect{};
    g.btn_vote_kind = vote_kind;
    g.btn_node_seed = node_seed;
    g.btn_cat_uid   = cat_uid;

    void* root = nullptr;
    if (!mem_read((const uint8_t*)head + 8, &root, sizeof(root)) || !root) return;

    void* stack[64];
    int sp = 0;
    stack[sp++] = root;
    uint32_t visited = 0;
    const size_t plen = strlen(prefix);
    bool any_rect = false;

    while (sp > 0 && visited < 128) {
        void* node = stack[--sp];
        if (!node || node == head) continue;
        uint8_t isnil = 1;
        if (!mem_read((const uint8_t*)node + 25, &isnil, 1) || isnil) continue;
        ++visited;

        void* left = nullptr; void* right = nullptr;
        mem_read((const uint8_t*)node + 0,  &left,  sizeof(left));
        mem_read((const uint8_t*)node + 16, &right, sizeof(right));
        if (right && right != head && sp < 64) stack[sp++] = right;
        if (left  && left  != head && sp < 64) stack[sp++] = left;

        char key[48] = {};
        if (!read_game_string((const uint8_t*)node + 32, key, sizeof(key))) continue;
        void* btn = nullptr;
        if (!mem_read((const uint8_t*)node + 64, &btn, sizeof(btn)) || !btn) continue;

        if (strncmp(key, prefix, plen) == 0 && key[plen] >= '0' && key[plen] <= '9') {
            uint32_t idx = 0;
            for (const char* p = key + plen; *p >= '0' && *p <= '9'; ++p)
                idx = idx * 10u + (uint32_t)(*p - '0');
            tint_button_clip(btn, !local_owns);
            if (idx < kMaxVoteButtons) {
                if (read_button_rect(btn, g.btn_rects[idx])) any_rect = true;
            }
        } else if (vote_kind == kChoiceLevelUpVote && strcmp(key, "reroll") == 0) {
            tint_button_clip(btn, !local_owns);
        }
    }

    if (any_rect) g.btn_rects_ms = GetTickCount64();
}

void cast_local_vote(uint8_t vote_kind, uint32_t index, uint64_t node_seed, uint64_t cat_uid) {
    const uint8_t self = net_self();
    if (self >= kMaxPeers) return;

    VoteEntry& mine = g.votes[self];
    const bool same = mine.active && mine.kind == vote_kind &&
                      mine.index == index && mine.node_seed == node_seed &&
                      mine.cat_uid == cat_uid;
    if (same) {
        mine = VoteEntry{};
    } else {
        mine.active    = true;
        mine.kind      = vote_kind;
        mine.index     = index;
        mine.node_seed = node_seed;
        mine.cat_uid   = cat_uid;
    }

    ChoiceMsg vm{};
    vm.kind      = vote_kind;
    vm.index     = index;
    vm.aux       = mine.active ? 1u : 0u;
    vm.node_seed = node_seed;
    vm.cat_uid   = cat_uid;
    net_send_choice(vm);
    log_line("CHOICE", "-> %s option %u (%s)", kind_name(vote_kind), index,
             mine.active ? "voted" : "cleared");
}

// --- the option arrays -----------------------------------------------------

bool read_options(const void* screen, uintptr_t begin_off, uintptr_t end_off,
                  uint8_t*& begin, uint32_t& count) {
    begin = nullptr; count = 0;
    if (!screen) return false;
    uint8_t* b = nullptr; uint8_t* e = nullptr;
    if (!mem_read((const uint8_t*)screen + begin_off, &b, sizeof(b))) return false;
    if (!mem_read((const uint8_t*)screen + end_off,   &e, sizeof(e))) return false;
    if (!b || e < b) return false;
    const uintptr_t bytes = (uintptr_t)(e - b);
    if (bytes % kOptStride) return false;
    const uintptr_t n = bytes / kOptStride;
    if (n == 0 || n > kMaxOptions) return false;
    begin = b; count = (uint32_t)n;
    return true;
}

void option_name(uint8_t kind, const uint8_t* entry, char* out, uint32_t cap) {
    const uintptr_t off = (kind == kChoiceLevelUp) ? kLvlOpt_Name : kEvtOpt_Stat;
    if (!read_game_string(entry + off, out, cap)) snprintf(out, cap, "?");
}

uint32_t option_type(const uint8_t* entry) {
    uint32_t t = 0;
    mem_read(entry + kLvlOpt_Type, &t, sizeof(t));
    return t;
}

// --- injecting -------------------------------------------------------------

bool inject_event(uint32_t index) {
    uint8_t* begin = nullptr; uint32_t count = 0;
    if (!read_options(g.world_event, kEvt_OptBegin, kEvt_OptEnd, begin, count)) return false;
    if (index >= count) return false;

    void* cap[3] = { nullptr, g.world_event, begin + (uintptr_t)index * kOptStride };
    if (!g.evt_commit) return false;
    g.injecting = true;
    g.evt_commit(cap);
    g.injecting = false;
    return true;
}

bool inject_level(void* screen, uint32_t index) {
    uint8_t* begin = nullptr; uint32_t count = 0;
    if (!read_options(screen, kLvl_OptBegin, kLvl_OptEnd, begin, count)) return false;
    if (index >= count) return false;

    struct Cap { void* vt; void* screen; int32_t index; int32_t pad; };
    Cap cap = { nullptr, screen, (int32_t)index, 0 };
    if (!g.lvl_click) return false;
    g.injecting = true;
    g.lvl_click(&cap);
    g.injecting = false;
    return true;
}

void apply_pending_event(void* screen) {
    if (!g.pending_evt || !screen) return;

    if (g.pending_evt_seed && g.here_seed && g.pending_evt_seed != g.here_seed) {
        if (!g.pending_evt_warned) {
            g.pending_evt_warned = true;
            log_line("CHOICE", "holding the host's event choice %u -- it was made on node"
                               " %016llx and this peer is still in %016llx",
                     g.pending_evt_index,
                     (unsigned long long)g.pending_evt_seed,
                     (unsigned long long)g.here_seed);
        }
        return;
    }

    uint8_t* begin = nullptr; uint32_t count = 0;
    if (!read_options(screen, kEvt_OptBegin, kEvt_OptEnd, begin, count)) {
        if (!g.pending_evt_warned) {
            g.pending_evt_warned = true;
            log_line("CHOICE", "holding the host's event choice %u -- this peer's option"
                               " list is not built yet", g.pending_evt_index);
        }
        return;
    }

    const uint32_t want  = g.pending_evt_index;
    const uint32_t their = g.pending_evt_count;
    if (their && their != count) {
        log_line("CHOICE", "!! host offered %u event option(s), this peer built %u --"
                           " refusing to choose; the two screens are not the same screen",
                 their, count);
        g.pending_evt = false;
        return;
    }
    if (want >= count) {
        log_line("CHOICE", "!! host chose event option %u but this peer has only %u --"
                           " refusing", want, count);
        g.pending_evt = false;
        return;
    }

    char mine[48] = {};
    option_name(kChoiceEvent, begin + (uintptr_t)want * kOptStride, mine, sizeof(mine));
    if (g.pending_evt_name[0] && strcmp(mine, g.pending_evt_name) != 0)
        log_line("CHOICE", "!! event option %u is '%s' here but '%s' on the host --"
                           " taking it anyway, the lists are the same length",
                 want, mine, g.pending_evt_name);

    const bool ok = inject_event(want);
    g.pending_evt = false;
    g.pending_evt_warned = false;
    clear_votes();
    if (ok) {
        ++g.applied;
        log_line("CHOICE", "applied the host's event choice: option %u/%u ('%s')",
                 want, count, mine);
    } else {
        log_line("CHOICE", "!! failed to apply the host's event choice %u", want);
    }
}

void apply_pending_level(void* screen) {
    if (!g.lvl_q_count || !screen) return;

    const uint64_t screen_uid = read_screen_cat_uid(screen, kLvl_Cat);

    for (;;) {
        uint32_t match_idx = UINT32_MAX;
        for (uint32_t i = 0; i < g.lvl_q_count; ++i) {
            const ChoiceMsg& m = g.lvl_q[i].msg;
            if (m.node_seed && g.here_seed && m.node_seed != g.here_seed) continue;
            if (m.cat_uid && screen_uid && m.cat_uid != screen_uid) continue;
            match_idx = i;
            break;
        }
        if (match_idx == UINT32_MAX) return;

        uint8_t committed = 0;
        if (mem_read((const uint8_t*)screen + kLvl_Committed, &committed, 1) && committed)
            return;

        const ChoiceMsg m = g.lvl_q[match_idx].msg;
        if (m.kind == kChoiceLevelUpReroll) {
            lvl_q_erase(match_idx);
            if (!g.lvl_reroll) {
                log_line("CHOICE", "!! LevelUpScreen::Reroll not resolved -- cannot replay reroll");
                continue;
            }
            clear_votes();
            g.injecting = true;
            g.lvl_reroll(screen);
            g.injecting = false;
            ++g.applied;
            log_line("CHOICE", "applied peer's level-up reroll (cat=%016llx)",
                     (unsigned long long)m.cat_uid);
            continue;
        }

        uint8_t* begin = nullptr; uint32_t count = 0;
        if (!read_options(screen, kLvl_OptBegin, kLvl_OptEnd, begin, count)) {
            if (!g.lvl_q[match_idx].warned) {
                g.lvl_q[match_idx].warned = true;
                log_line("CHOICE", "holding peer's level-up choice %u -- option list not built yet",
                         m.index);
            }
            return;
        }

        if (m.count && m.count != count) {
            log_line("CHOICE", "!! peer offered %u level-up option(s), this peer built %u --"
                               " refusing to choose", m.count, count);
            lvl_q_erase(match_idx);
            return;
        }
        if (m.index >= count) {
            log_line("CHOICE", "!! peer chose level-up option %u but this peer has only %u --"
                               " refusing", m.index, count);
            lvl_q_erase(match_idx);
            return;
        }

        char mine[48] = {};
        option_name(kChoiceLevelUp, begin + (uintptr_t)m.index * kOptStride, mine, sizeof(mine));
        if (m.name[0] && strcmp(mine, m.name) != 0)
            log_line("CHOICE", "!! level-up option %u is '%s' here but '%s' on the peer --"
                               " taking it anyway, the lists are the same length",
                     m.index, mine, m.name);

        const uint32_t mine_type = option_type(begin + (uintptr_t)m.index * kOptStride);
        if (m.aux && mine_type != m.aux)
            log_line("CHOICE", "!! level-up option %u is type %u here but %u on the"
                               " peer -- taking it anyway", m.index, mine_type, m.aux);

        const bool ok = inject_level(screen, m.index);
        lvl_q_erase(match_idx);
        clear_votes();
        if (ok) {
            ++g.applied;
            log_line("CHOICE", "applied peer's level-up choice: option %u/%u ('%s', cat=%016llx)",
                     m.index, count, mine, (unsigned long long)m.cat_uid);
        } else {
            log_line("CHOICE", "!! failed to apply peer's level-up choice %u", m.index);
        }
        return;
    }
}

// Helper: read CatData* at screen+offset -> cat_uid -> draft owner.
bool screen_cat_owner(const void* screen, uintptr_t cat_ptr_off, uint8_t* out_owner) {
    if (!screen) return false;
    void* cd = nullptr;
    uint64_t uid = 0;
    if (!mem_read((const uint8_t*)screen + cat_ptr_off, &cd, sizeof(cd)) || !cd) return false;
    if (!mem_read((const uint8_t*)cd + kCatData_Uid, &uid, sizeof(uid)) || !uid)  return false;
    return draft_lookup_owner(uid, out_owner);
}

} // namespace

// ---------------------------------------------------------------------------

void choice_set_base(uintptr_t base) {
    g.base = base;
    g.resolved = false;
    g.evt_commit    = nullptr;
    g.lvl_click     = nullptr;
    g.lvl_reroll    = nullptr;
    g.get_world_pos = nullptr;
    g.cat_by_id = reinterpret_cast<fn_cat_by_id>(addr_of_call(C_CatDataById));
    g.transition_slot = reinterpret_cast<const void*>(addr_of_data(D_TransitionScenePtr));

    const uintptr_t evt = addr_of(T_EventChoice);
    const uintptr_t lvl = addr_of_call(C_LevelUpClick);
    const uintptr_t rer = addr_of(T_LevelReroll);
    if (!evt || !lvl) {
        log_line("CHOICE", "!! %s did not resolve by signature"
                           " -- choice replication is OFF",
                 !evt ? "WorldEvent option commit" : "LevelUpScreen button click");
        return;
    }
    g.evt_commit    = (fn_cap)evt;
    g.lvl_click     = (fn_cap)lvl;
    g.lvl_reroll    = (fn_cap)rer;
    g.get_world_pos = (fn_get_world_pos)addr_of_call(C_RendererGetWorldPos);
    g.resolved = true;
}

void choice_init() {
    if (!g.cs_ready) { InitializeCriticalSection(&g.cs); g.cs_ready = true; }
    g.on          = tune::kChoice;
    g.is_client   = (net_role() == NetRole::Client);
    g.world_event = nullptr;
    g.pending_evt = false;
    g.pending_evt_warned = false;
    g.lvl_q_count = 0;
    g.shop_actions.clear();
    g.ac_actions.clear();
    g.last_lvl_scene   = nullptr;
    g.last_lvl_cat_uid = 0;
    g.candy_shop = nullptr;
    g.candy_level_opening = false;
    g.here_seed   = 0;
    memset(g.skipped, 0, sizeof(g.skipped));
    g.skipped_next = 0;
    clear_votes();
    g.btn_rects_ms = 0;
    g.injecting = false;
    if (!g.on) { log_line("CHOICE", "choice replication disabled by net_choice = 0"); return; }
    if (g.is_client && !g.resolved) {
        g.on = false;
        log_line("CHOICE", "!! call targets unresolved -- choice replication is OFF."
                           " This peer keeps its own event and level-up clicks; expect"
                           " the two runs to diverge on the first event.");
        return;
    }
    log_line_lvl(LogLevel::Trace, "CHOICE", "armed -- %s",
             g.is_client ? "taking the host's event choices (local event/shop/inv clicks guarded);"
                           " level-up choices & rerolls are shared by cat owner"
                         : "publishing event choices; level-up choices & rerolls are shared by cat owner");
}

void choice_shutdown() {
    if (!g.on) return;
    log_line_lvl(LogLevel::Trace, "CHOICE",
             "done: %u sent, %u applied, %u local click(s) suppressed",
             g.sent, g.applied, g.swallowed);
    g.on = false;
    g.world_event  = nullptr;
    g.pending_evt  = false;
    g.lvl_q_count  = 0;
    g.shop_actions.clear();
    g.ac_actions.clear();
    g.last_lvl_scene   = nullptr;
    g.last_lvl_cat_uid = 0;
    g.candy_shop = nullptr;
    g.candy_level_opening = false;
    g.here_seed    = 0;
    memset(g.skipped, 0, sizeof(g.skipped));
    g.skipped_next = 0;
    clear_votes();
    g.btn_rects_ms = 0;
    if (g.cs_ready) { DeleteCriticalSection(&g.cs); g.cs_ready = false; }
}

// --- the world event -------------------------------------------------------

bool choice_on_event_commit(void* cap) {
    if (!g.on || !cap) return true;
    if (g.injecting) return true;

    Guard guard;

    void* we    = nullptr;
    void* entry = nullptr;
    if (!mem_read((const uint8_t*)cap + 8,  &we,    sizeof(we)))    return true;
    if (!mem_read((const uint8_t*)cap + 16, &entry, sizeof(entry))) return true;
    g.world_event = we;

    uint8_t* begin = nullptr; uint32_t count = 0;
    const bool have = read_options(we, kEvt_OptBegin, kEvt_OptEnd, begin, count);

    uint8_t cat_owner = kNoPeer;
    const bool has_cat_owner = screen_cat_owner(we, kEvt_CatChoice, &cat_owner);
    const bool local_owns_event = has_cat_owner ? (cat_owner == net_self()) : !g.is_client;

    if (!local_owns_event) {
        ++g.swallowed;
        if (have) {
            const uintptr_t delta = (uintptr_t)((uint8_t*)entry - begin);
            if (delta % kOptStride == 0 && delta / kOptStride < count) {
                const uint32_t idx = (uint32_t)(delta / kOptStride);
                cast_local_vote(kChoiceEventVote, idx, follow_here_seed(), 0);
            }
        }
        return false;
    }

    clear_votes();

    if (!have) {
        log_line("CHOICE", "!! could not read this event's option array -- the peer will"
                           " not be told which option was picked");
        return true;
    }
    const uintptr_t delta = (uintptr_t)((uint8_t*)entry - begin);
    if (delta % kOptStride || delta / kOptStride >= count) {
        log_line("CHOICE", "!! the chosen option is not an element of this event's array"
                           " -- not publishing");
        return true;
    }

    ChoiceMsg m{};
    m.kind  = kChoiceEvent;
    m.index = (uint32_t)(delta / kOptStride);
    m.count = count;
    m.node_seed = follow_here_seed();
    option_name(kChoiceEvent, (const uint8_t*)entry, m.name, sizeof(m.name));
    if (net_send_choice(m)) {
        ++g.sent;
        log_line("CHOICE", "-> event option %u/%u ('%s')", m.index, m.count, m.name);
    }
    return true;
}

void choice_on_event_update(void* world_event) {
    if (!g.on || !world_event) return;
    Guard guard;
    g.world_event = world_event;
    if (net_active()) {
        uint8_t cat_owner = kNoPeer;
        const bool has_cat_owner = screen_cat_owner(world_event, kEvt_CatChoice, &cat_owner);
        const bool local_owns = has_cat_owner ? (cat_owner == net_self()) : !g.is_client;
        sync_choice_panel(world_event, kEvt_Panel, "choice", local_owns,
                          kChoiceEventVote, follow_here_seed(), 0);
    }
    apply_pending_event(world_event);
}

// --- the level-up screen & shop/inventory guards ---------------------------

bool choice_on_level_reroll(void* screen) {
    if (!g.on || !screen) return true;
    if (g.injecting) return true;

    Guard guard;
    publish_candy_level(screen);

    uint8_t committed = 0;
    if (mem_read((const uint8_t*)screen + kLvl_Committed, &committed, 1) && committed)
        return true;

    uint8_t cat_owner = kNoPeer;
    const bool has_cat_owner = screen_cat_owner(screen, kLvl_Cat, &cat_owner);
    const bool local_owns = has_cat_owner ? (cat_owner == net_self()) : !g.is_client;
    if (!local_owns) {
        ++g.swallowed;
        log_line("CHOICE", "suppressed a local level-up reroll -- %s owns this cat",
                 has_cat_owner ? "another player" : "the host");
        return false;
    }

    clear_votes();

    ChoiceMsg m{};
    m.kind      = kChoiceLevelUpReroll;
    m.node_seed = follow_here_seed();
    m.cat_uid   = read_screen_cat_uid(screen, kLvl_Cat);
    if (net_send_choice(m)) {
        ++g.sent;
        log_line("CHOICE", "-> level-up reroll (cat=%016llx)", (unsigned long long)m.cat_uid);
    }
    return true;
}

void choice_on_shop_buy(void* shop, void* item, ShopBuyFn buy) {
    if (!g.on || !net_active()) {
        buy(shop, item);
        return;
    }
    Guard guard;
    if (g.is_client) {
        ++g.swallowed;
        log_line("CHOICE", "suppressed local shop purchase -- the host owns the run");
        return;
    }
    if (!sync_shop(shop)) {
        buy(shop, item);
        return;
    }

    ChoiceMsg purchase{};
    purchase.kind = kChoiceShopPurchase;
    purchase.node_seed = g.here_seed;
    uintptr_t begin = 0;
    uint32_t count = 0;
    int32_t stock = 0;
    const uintptr_t address = reinterpret_cast<uintptr_t>(item);
    if (!read_shop_items(shop, begin, count) || address < begin ||
        (address - begin) % kShop_ItemSize ||
        (address - begin) / kShop_ItemSize >= count ||
        !read_shop_item(item, purchase, stock)) {
        log_line("CHOICE", "!! could not identify shop purchase in local stock");
        buy(shop, item);
        return;
    }
    purchase.index = static_cast<uint32_t>((address - begin) / kShop_ItemSize);
    if (g.candy_shop || (purchase.aux == kShopItem_Candy && !shop_transition_is_idle()))
        return;

    buy(shop, item);
    int32_t remaining = 0;
    if (stock <= 0 || !read_field(item, kShopItem_Stock, remaining) || remaining >= stock)
        return;
    if (purchase.aux == kShopItem_Candy) {
        if (shop_transition_is_idle()) {
            log_line("CHOICE", "!! candy purchase did not start a level-up transition");
            return;
        }
        g.candy_purchase = purchase;
        g.candy_shop = shop;
    } else {
        send_shop_action(purchase);
    }
}

void choice_on_shop_exit(void* capture, ShopExitFn exit) {
    Guard guard;
    void* shop = nullptr;
    int32_t steal_count = 0;
    if (!read_field(capture, sizeof(void*), shop) || !sync_shop(shop) ||
        !read_field(shop, kShop_StealCount, steal_count) || steal_count > 0) {
        exit(capture);
        return;
    }
    // Keep the client in the shop until pending purchases and their level-ups
    // have run locally, then replay the host's exit.
    if (g.is_client || g.candy_shop || !shop_transition_is_idle()) return;

    exit(capture);
    if (!shop_transition_is_idle()) {
        ChoiceMsg action{};
        action.kind = kChoiceShopExit;
        action.node_seed = g.here_seed;
        send_shop_action(action);
    }
}

void choice_on_shop_update(void* shop, ShopBuyFn buy, ShopExitFn exit) {
    if (!g.on || !g.is_client) return;
    Guard guard;
    if (g.shop_actions.empty() || !sync_shop(shop) || g.candy_shop ||
        !shop_transition_is_idle())
        return;
    const void* scene = nullptr;
    uint8_t paused = 0;
    if (!read_field(shop, kComp_Scene, scene) ||
        !read_field(scene, kScene_Paused, paused) || paused)
        return;

    const auto action = std::find_if(g.shop_actions.begin(), g.shop_actions.end(),
        [](const ChoiceMsg& m) { return m.node_seed == g.here_seed; });
    if (action == g.shop_actions.end()) return;
    const ChoiceMsg purchase = *action;
    g.shop_actions.erase(action);
    if (purchase.kind == kChoiceShopExit) {
        ShopExitCapture capture{nullptr, shop};
        exit(&capture);
        return;
    }

    uintptr_t begin = 0;
    uint32_t count = 0;
    int32_t stock = 0;
    ChoiceMsg local{};
    void* item = nullptr;
    if (read_shop_items(shop, begin, count) && purchase.index < count)
        item = reinterpret_cast<void*>(begin + purchase.index * kShop_ItemSize);
    if (!read_shop_item(item, local, stock) || stock <= 0 ||
        local.count != purchase.count || local.aux != purchase.aux ||
        strcmp(local.name, purchase.name) != 0) {
        log_line("CHOICE", "!! host's shop purchase does not match local stock (slot %u)",
                 purchase.index);
        return;
    }
    if (purchase.aux == kShopItem_Candy && !shop_cat_by_uid(shop, purchase.cat_uid)) {
        log_line("CHOICE", "!! candy recipient %016llx is missing from the local run",
                 (unsigned long long)purchase.cat_uid);
        return;
    }

    buy(shop, item);
    int32_t remaining = 0;
    if (!read_field(item, kShopItem_Stock, remaining) || remaining >= stock ||
        (purchase.aux == kShopItem_Candy && shop_transition_is_idle())) {
        log_line("CHOICE", "!! failed to replay host's shop purchase (slot %u)", purchase.index);
        return;
    }
    if (purchase.aux == kShopItem_Candy) {
        g.candy_purchase = purchase;
        g.candy_shop = shop;
    }
    ++g.applied;
    log_line("CHOICE", "applied host's shop purchase (slot %u)", purchase.index);
}

void choice_on_shop_candy_level(void* capture, ShopExitFn open_level) {
    Guard guard;
    ShopCandyCapture local{};
    if (!g.on || !g.candy_shop || !mem_read(capture, &local, sizeof(local)) ||
        g.candy_shop != local.shop) {
        open_level(capture);
        return;
    }
    if (!g.is_client) {
        g.candy_level_opening = true;
        open_level(capture);
        return;
    }
    g.candy_shop = nullptr;
    void* cat = shop_cat_by_uid(local.shop, g.candy_purchase.cat_uid);
    if (!cat) {
        log_line("CHOICE", "!! candy recipient disappeared before the level-up screen opened");
        open_level(capture);
        return;
    }
    // The game still runs its normal roll and opens its usual LevelUpScreen.
    local.capacity = local.count = 1;
    local.cats = &cat;
    open_level(&local);
}

bool choice_on_inv_box_click(void* box_ptr) {
    (void)box_ptr;
    if (!g.on || !g.is_client || !net_active()) return true;
    if (!savefile_adventure_is_loaded()) return true;
    Guard guard;
    ++g.swallowed;
    log_line("CHOICE", "suppressed local adventure inventory click -- the host owns the run");
    return false;
}

bool choice_on_level_select(void* screen, void* opt) {
    if (!g.on || !screen || !opt) return true;
    if (g.injecting) return true;

    Guard guard;
    publish_candy_level(screen);

    uint8_t committed = 0;
    if (mem_read((const uint8_t*)screen + kLvl_Committed, &committed, 1) && committed)
        return true;

    uint8_t cat_owner = kNoPeer;
    const bool has_cat_owner = screen_cat_owner(screen, kLvl_Cat, &cat_owner);
    const bool local_owns = has_cat_owner ? (cat_owner == net_self()) : !g.is_client;

    uint8_t* begin = nullptr; uint32_t count = 0;
    if (!read_options(screen, kLvl_OptBegin, kLvl_OptEnd, begin, count)) {
        if (!local_owns) { ++g.swallowed; return false; }
        log_line("CHOICE", "!! could not read the level-up option array -- the peer will"
                           " not be told which option was picked");
        return true;
    }

    char want_name[48] = {};
    option_name(kChoiceLevelUp, (const uint8_t*)opt, want_name, sizeof(want_name));
    const uint32_t want_type = option_type((const uint8_t*)opt);

    uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = begin + (uintptr_t)i * kOptStride;
        if (option_type(e) != want_type) continue;
        char nm[48] = {};
        option_name(kChoiceLevelUp, e, nm, sizeof(nm));
        if (strcmp(nm, want_name) == 0) { found = i; break; }
    }

    if (!local_owns) {
        ++g.swallowed;
        if (found != UINT32_MAX) {
            cast_local_vote(kChoiceLevelUpVote, found, follow_here_seed(),
                            read_screen_cat_uid(screen, kLvl_Cat));
        }
        return false;
    }

    clear_votes();

    if (found == UINT32_MAX) {
        log_line("CHOICE", "!! the chosen level-up option (type %u '%s') matches none of"
                           " the %u on offer -- not publishing",
                 want_type, want_name, count);
        return true;
    }

    ChoiceMsg m{};
    m.kind      = kChoiceLevelUp;
    m.index     = found;
    m.count     = count;
    m.aux       = want_type;
    m.node_seed = follow_here_seed();
    m.cat_uid   = read_screen_cat_uid(screen, kLvl_Cat);
    memcpy(m.name, want_name, sizeof(m.name));
    if (net_send_choice(m)) {
        ++g.sent;
        log_line("CHOICE", "-> level-up option %u/%u (type %u '%s', cat=%016llx)",
                 m.index, m.count, m.aux, m.name, (unsigned long long)m.cat_uid);
    }
    return true;
}

void choice_on_level_update(void* level_screen) {
    if (!g.on || !level_screen) return;
    Guard guard;
    publish_candy_level(level_screen);
    void* lvl_scene = nullptr;
    if (read_field(level_screen, kComp_Scene, lvl_scene) && lvl_scene) {
        g.last_lvl_scene   = lvl_scene;
        g.last_lvl_cat_uid = read_screen_cat_uid(level_screen, kLvl_Cat);
    }
    if (net_active()) {
        uint8_t cat_owner = kNoPeer;
        const bool has_cat_owner = screen_cat_owner(level_screen, kLvl_Cat, &cat_owner);
        const bool local_owns = has_cat_owner ? (cat_owner == net_self()) : !g.is_client;
        const uint64_t uid = read_screen_cat_uid(level_screen, kLvl_Cat);
        sync_choice_panel(level_screen, kLvl_Panel, "opt", local_owns,
                          kChoiceLevelUpVote, follow_here_seed(), uid);
    }
    apply_pending_level(level_screen);
}

namespace {

void ability_chooser_owner(void* chooser, uint64_t& out_cat_uid, bool& out_local_owns) {
    out_cat_uid = 0;
    out_local_owns = !g.is_client;
    if (!chooser) return;

    void* parent_scene = nullptr;
    if (!read_field(chooser, kAC_ParentScene, parent_scene) || !parent_scene)
        return;

    if (parent_scene == g.last_lvl_scene && g.last_lvl_cat_uid) {
        out_cat_uid = g.last_lvl_cat_uid;
        uint8_t owner = kNoPeer;
        const bool has = draft_lookup_owner(out_cat_uid, &owner);
        out_local_owns = has ? (owner == net_self()) : !g.is_client;
        return;
    }

    if (g.world_event) {
        void* evt_scene = nullptr;
        if (read_field(g.world_event, kComp_Scene, evt_scene) && evt_scene == parent_scene) {
            out_cat_uid = read_screen_cat_uid(g.world_event, kEvt_CatChoice);
            uint8_t owner = kNoPeer;
            const bool has = out_cat_uid && draft_lookup_owner(out_cat_uid, &owner);
            out_local_owns = has ? (owner == net_self()) : !g.is_client;
            return;
        }
    }
}

void tint_all_panel_buttons(void* panel, bool grey_out) {
    if (!panel) return;
    void* head = nullptr;
    uint64_t count = 0;
    if (!mem_read((const uint8_t*)panel + kPanel_Buttons, &head, sizeof(head)) || !head)
        return;
    if (!mem_read((const uint8_t*)panel + kPanel_Buttons + 8, &count, sizeof(count)) ||
        count == 0 || count > 128)
        return;
    void* root = nullptr;
    if (!mem_read((const uint8_t*)head + 8, &root, sizeof(root)) || !root) return;

    void* stack[64];
    int sp = 0;
    stack[sp++] = root;
    uint32_t visited = 0;
    while (sp > 0 && visited < 128) {
        void* node = stack[--sp];
        if (!node || node == head) continue;
        uint8_t isnil = 1;
        if (!mem_read((const uint8_t*)node + 25, &isnil, 1) || isnil) continue;
        ++visited;
        void* left = nullptr; void* right = nullptr;
        mem_read((const uint8_t*)node + 0,  &left,  sizeof(left));
        mem_read((const uint8_t*)node + 16, &right, sizeof(right));
        if (right && right != head && sp < 64) stack[sp++] = right;
        if (left  && left  != head && sp < 64) stack[sp++] = left;
        void* btn = nullptr;
        if (mem_read((const uint8_t*)node + 64, &btn, sizeof(btn)) && btn)
            tint_button_clip(btn, grey_out);
    }
}

bool ability_chooser_is_closing(void* chooser) {
    void* panel = nullptr;
    void* ren   = nullptr;
    void* mc    = nullptr;
    uint8_t flags = 0;
    if (!read_field(chooser, kAC_Panel, panel) || !panel) return false;
    if (!read_field(panel, kPanel_Renderer, ren) || !ren) return false;
    if (!read_field(ren, kRenderer_Graphics, mc) || !mc)  return false;
    if (!mem_read((const uint8_t*)mc + 9, &flags, 1))     return false;
    return (flags & 2u) != 0;
}

} // namespace

bool choice_on_ability_chooser_close(void* chooser, int32_t slot) {
    if (!g.on || !chooser || g.injecting || !net_active()) return true;
    Guard guard;

    uint64_t cat_uid = 0;
    bool local_owns = false;
    ability_chooser_owner(chooser, cat_uid, local_owns);
    if (!local_owns) {
        ++g.swallowed;
        log_line("CHOICE", "suppressed local ability-replace click (slot=%d) -- another player owns this cat",
                 (int)slot);
        return false;
    }

    if (ability_chooser_is_closing(chooser)) return true;

    ChoiceMsg m{};
    m.kind      = kChoiceAbilityReplace;
    m.index     = static_cast<uint32_t>(slot);
    m.node_seed = follow_here_seed();
    m.cat_uid   = cat_uid;
    if (net_send_choice(m)) {
        ++g.sent;
        log_line("CHOICE", "-> ability replace slot %d (cat=%016llx)",
                 (int)slot, (unsigned long long)cat_uid);
    }
    return true;
}

void choice_on_ability_chooser_update(void* chooser, AbilityChooserCloseFn close_fn) {
    if (!g.on || !chooser) return;
    Guard guard;

    uint64_t cat_uid = 0;
    bool local_owns = false;
    ability_chooser_owner(chooser, cat_uid, local_owns);

    if (net_active()) {
        void* panel = nullptr;
        if (read_field(chooser, kAC_Panel, panel) && panel)
            tint_all_panel_buttons(panel, !local_owns);
    }

    if (g.ac_actions.empty() || !close_fn || ability_chooser_is_closing(chooser))
        return;

    for (auto it = g.ac_actions.begin(); it != g.ac_actions.end(); ++it) {
        if (it->node_seed && g.here_seed && it->node_seed != g.here_seed) continue;
        if (it->cat_uid && cat_uid && it->cat_uid != cat_uid) continue;
        const int32_t slot = static_cast<int32_t>(it->index);
        const uint64_t uid = it->cat_uid;
        g.ac_actions.erase(it);
        g.injecting = true;
        close_fn(chooser, slot);
        g.injecting = false;
        ++g.applied;
        log_line("CHOICE", "applied peer's ability replace: slot %d (cat=%016llx)",
                 (int)slot, (unsigned long long)uid);
        return;
    }
}

// --- receiving -------------------------------------------------------------

void choice_on_message(uint8_t from, const ChoiceMsg& m) {
    if (!g.on) return;
    Guard guard;

    if (m.kind == kChoiceEventVote || m.kind == kChoiceLevelUpVote) {
        if (from < kMaxPeers) {
            VoteEntry& v = g.votes[from];
            v.active    = (m.aux != 0);
            v.kind      = m.kind;
            v.index     = m.index;
            v.node_seed = m.node_seed;
            v.cat_uid   = m.cat_uid;
            log_line("CHOICE", "<- peer %u %s %s option %u",
                     (unsigned)from, v.active ? "voted for" : "cleared vote on",
                     kind_name(m.kind), m.index);
        }
        return;
    }

    if (was_skipped(m.node_seed)) {
        log_line("CHOICE", "!! a %s choice arrived for node %016llx, which this peer"
                           " passed over without entering -- refusing it.",
                 kind_name(m.kind), (unsigned long long)m.node_seed);
        return;
    }

    if (m.kind == kChoiceShopPurchase || m.kind == kChoiceShopExit) {
        if (!g.is_client || from != kHostPeer || !m.node_seed) return;
        if (g.shop_actions.size() >= 256) {
            log_line("CHOICE", "!! shop action queue full");
            return;
        }
        g.shop_actions.push_back(m);
        return;
    }

    if (m.kind == kChoiceAbilityReplace) {
        if (g.ac_actions.size() >= 32) g.ac_actions.pop_front();
        g.ac_actions.push_back(m);
        log_line("CHOICE", "<- peer %u ability replace slot %d (cat=%016llx) on node %016llx",
                 (unsigned)from, (int)(int32_t)m.index,
                 (unsigned long long)m.cat_uid, (unsigned long long)m.node_seed);
        return;
    }

    if (m.kind == kChoiceLevelUp || m.kind == kChoiceLevelUpReroll) {
        if (g.lvl_q_count >= State::kMaxPendingLvl) {
            log_line("CHOICE", "!! level-up choice queue full (%u) -- dropping oldest",
                     g.lvl_q_count);
            lvl_q_erase(0);
        }
        State::PendingLvl& slot = g.lvl_q[g.lvl_q_count++];
        slot.msg = m;
        slot.msg.name[sizeof(slot.msg.name) - 1] = '\0';
        slot.warned = false;

        if (m.kind == kChoiceLevelUpReroll)
            log_line("CHOICE", "<- peer rerolled level-up (cat=%016llx) on node %016llx",
                     (unsigned long long)m.cat_uid, (unsigned long long)m.node_seed);
        else
            log_line("CHOICE", "<- peer chose level-up option %u/%u ('%s', cat=%016llx) on node %016llx",
                     m.index, m.count, slot.msg.name,
                     (unsigned long long)m.cat_uid, (unsigned long long)m.node_seed);

        return;
    }

    if (g.pending_evt)
        log_line("CHOICE", "!! an event choice was still held when another arrived --"
                           " the older one (option %u) is dropped", g.pending_evt_index);

    g.pending_evt        = true;
    g.pending_evt_seed   = m.node_seed;
    g.pending_evt_index  = m.index;
    g.pending_evt_count  = m.count;
    g.pending_evt_warned = false;
    memcpy(g.pending_evt_name, m.name, sizeof(g.pending_evt_name));
    g.pending_evt_name[sizeof(g.pending_evt_name) - 1] = '\0';

    log_line("CHOICE", "<- host chose event option %u/%u ('%s') on node %016llx",
             m.index, m.count, g.pending_evt_name, (unsigned long long)m.node_seed);

    if (g.world_event) apply_pending_event(g.world_event);
}

void choice_on_node_entered(uint64_t node_seed) {
    if (!g.on) return;
    Guard guard;
    if (g.here_seed != node_seed) {
        discard_shop_actions(g.here_seed);
        g.candy_shop = nullptr;
        g.candy_level_opening = false;
    }
    g.here_seed = node_seed;

    g.world_event  = nullptr;
    clear_votes();
    g.btn_rects_ms = 0;

    if (g.pending_evt && g.pending_evt_seed == node_seed)
        g.pending_evt_warned = false;
    for (uint32_t i = 0; i < g.lvl_q_count; ++i)
        if (g.lvl_q[i].msg.node_seed == node_seed)
            g.lvl_q[i].warned = false;
}

void choice_on_node_skipped(uint64_t node_seed) {
    if (!g.on || !node_seed) return;
    Guard guard;
    remember_skipped(node_seed);
    discard_shop_actions(node_seed);
    g.ac_actions.erase(std::remove_if(g.ac_actions.begin(), g.ac_actions.end(),
        [node_seed](const ChoiceMsg& m) { return m.node_seed == node_seed; }),
        g.ac_actions.end());

    if (g.pending_evt && g.pending_evt_seed == node_seed) {
        log_line("CHOICE", "!! node %016llx was skipped -- dropping held event choice %u ('%s')",
                 (unsigned long long)node_seed, g.pending_evt_index, g.pending_evt_name);
        g.pending_evt        = false;
        g.pending_evt_warned = false;
    }
    for (uint32_t i = 0; i < g.lvl_q_count; ) {
        if (g.lvl_q[i].msg.node_seed == node_seed) {
            log_line("CHOICE", "!! node %016llx was skipped -- dropping held %s (cat=%016llx)",
                     (unsigned long long)node_seed, kind_name(g.lvl_q[i].msg.kind),
                     (unsigned long long)g.lvl_q[i].msg.cat_uid);
            lvl_q_erase(i);
        } else {
            ++i;
        }
    }
}

uint32_t choice_get_vote_badges(ChoiceVoteBadge* out, uint32_t cap) {
    if (!g.on || !out || cap == 0 || !g.btn_rects_ms) return 0;
    if (GetTickCount64() - g.btn_rects_ms > 200) return 0;

    Guard guard;
    uint32_t n = 0;
    for (uint32_t idx = 0; idx < kMaxVoteButtons && n < cap; ++idx) {
        const BtnRect& r = g.btn_rects[idx];
        if (!r.valid) continue;

        ChoiceVoteBadge b{};
        b.nx0 = r.nx0; b.ny0 = r.ny0;
        b.nx1 = r.nx1; b.ny1 = r.ny1;

        for (uint8_t p = 0; p < kMaxPeers && b.voter_count < 4; ++p) {
            const VoteEntry& v = g.votes[p];
            if (!v.active || v.kind != g.btn_vote_kind || v.index != idx) continue;
            if (v.node_seed && g.btn_node_seed && v.node_seed != g.btn_node_seed) continue;
            if (v.cat_uid && g.btn_cat_uid && v.cat_uid != g.btn_cat_uid) continue;
            b.voters[b.voter_count++] = p;
        }
        if (b.voter_count > 0) out[n++] = b;
    }
    return n;
}

} // namespace mgmp
