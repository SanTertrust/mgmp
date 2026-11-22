// mgmp_house.cpp -- host-authoritative House co-op implementation.

#include "mgmp_house.h"
#include "mgmp_addresses.h"
#include "mgmp_bytestream.h"
#include "mgmp_catsync.h"
#include "mgmp_config.h"
#include "mgmp_follow.h"
#include "mgmp_invsync.h"
#include "mgmp_leave.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_overlay.h"
#include "mgmp_proto.h"
#include "mgmp_resolve.h"
#include "mgmp_runhist.h"
#include "mgmp_savefile.h"
#include "mgmp_tuning.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <unordered_map>
#include <vector>

namespace mgmp {
namespace {

struct alignas(16) Vec2D { double x = 0.0, y = 0.0; };
struct Vec3D { double x = 0.0, y = 0.0, z = 0.0; };

using FnSceneUpdateCachedList = void(__fastcall*)(void* scene, int comp_id);
using FnHouseLocUnderPoint    = void*(__fastcall*)(void* house, Vec2D* pt);
using FnPlaceCat              = void(__fastcall*)(void* area, void* housecat);
using FnRemoveCat             = void(__fastcall*)(void* area, void* housecat);
using FnCameraGetMouse        = Vec2D*(__fastcall*)(void* cam, Vec2D* out);
using FnCameraMatrix          = float*(__fastcall*)(void* cam, float* out_mat);
using FnGameStrAlloc          = void*(__fastcall*)(size_t);
using FnGameStrDtor           = void(__fastcall*)(void*);
using FnComponentDestroy      = void(__fastcall*)(void* comp);
using FnSceneCreateEntity     = void*(__fastcall*)(void* scene);
using FnSceneCreateMewDir     = void*(__fastcall*)(void* scene, void* entity, void* std_str);
using FnMewDirContinueSave    = void(__fastcall*)(void* mew_dir);
using FnCatById                = void*(__fastcall*)(const void*, int64_t);
using FnSerializeCat          = void(__fastcall*)(void* catdata, void* bs, bool write);
using FnBsDtor                = void(__fastcall*)(void* bs);
using FnOfstreamCtor          = void(__fastcall*)(void* ofs);
using FnVoidPtr               = void(__fastcall*)(void* obj);
using FnVecPushBack           = void(__fastcall*)(void* vec, const void* elem);
using FnStrAssign             = void*(__fastcall*)(void* str, const char* ptr, size_t len);
using FnWStrAssign            = void*(__fastcall*)(void* wstr, const wchar_t* ptr, size_t len);
using FnPodBufResize          = void(__fastcall*)(void* pod, uint32_t count);
using FnVecInterEmplace       = void*(__fastcall*)(void* vec, void* where, const void* elem);
using FnDoInterstitials       = void(__fastcall*)(void* iq);
using FnGoOnAdventure         = void(__fastcall*)(void* md, void* map_str, void* collars_vec, void* cat_ids_buf);
using FnDirectorAddScene      = void*(__fastcall*)(void* dir, void* std_str);
using FnCreateActSel          = void*(__fastcall*)(void* scene, void* entity);
using FnAnimatorSetState      = void(__fastcall*)(void* animator, int32_t state, void* substate);
using FnCreateFurnPiece       = void*(__fastcall*)(void* house, int64_t* uid);
using FnCatSelectorShowStats  = void(__fastcall*)(void* selector, int64_t cat_id);
using FnInvScreen2Lookup      = void*(__fastcall*)(void* screen, int64_t uid);
using FnInvScreen2ItemEq      = int64_t(__fastcall*)(void* screen, int64_t uid);
using FnInvItemBoxClickApply  = void(__fastcall*)(void** box_ptr);

struct InterpSample {
    uint32_t host_tick_ms = 0;
    HouseCatPose pose;
};

struct ClientCatTrack {
    std::vector<InterpSample> samples;
    HouseCatPose last_applied{};
    bool         has_last = false;
    bool         predicted_held = false;
    uint32_t     pending_req_seq = 0;
    bool         pending_release_before_grant = false;
    float        pending_release_wx = 0.0f;
    float        pending_release_wy = 0.0f;
    uint32_t     capture_id = 0;
    uint8_t      holder_peer = kNoPeer;
    float        pred_wx = 0.0f;
    float        pred_wy = 0.0f;
    Vec2D        throw_velocity{};
    uint32_t     last_motion_ms = 0;
};

struct HostCapture {
    int64_t  cat_id       = 0;
    uint8_t  holder_peer  = kNoPeer;
    uint32_t capture_id   = 0;
    uint32_t last_seq     = 0;
    float    wx           = 0.0f;
    float    wy           = 0.0f;
    int32_t  orig_depth   = 0;
};

struct PeerCursorState {
    bool     valid    = false;
    uint8_t  in_house = 0;      // 1 = House world-space, 2 = Prep screen-space
    uint32_t gen      = 0;
    float    wx       = 0.0f;
    float    wy       = 0.0f;
    float    nx       = 0.0f;
    float    ny       = 0.0f;
    float    prev_wx  = 0.0f;
    float    prev_wy  = 0.0f;
    uint8_t  mode     = 0;
    uint32_t recv_ms  = 0;
    uint32_t prev_ms  = 0;
};

struct HouseState {
    bool      on           = false;
    bool      is_client    = false;
    uintptr_t base         = 0;

    // Resolved game functions & data slots
    const void**            director_slot   = nullptr;
    const void**            app_base_slot   = nullptr;
    const float*            screen_bounds   = nullptr;
    FnSceneUpdateCachedList update_cached   = nullptr;
    FnHouseLocUnderPoint    loc_under_point = nullptr;
    FnPlaceCat              place_cat       = nullptr;
    FnRemoveCat             remove_cat      = nullptr;
    FnCameraGetMouse        cam_get_mouse   = nullptr;
    FnCameraMatrix          cam_matrix      = nullptr;
    FnGameStrAlloc          str_alloc       = nullptr;
    FnGameStrDtor           str_dtor        = nullptr;
    FnComponentDestroy      comp_destroy    = nullptr;
    FnSceneCreateEntity     create_entity   = nullptr;
    FnSceneCreateMewDir     create_mew_dir  = nullptr;
    FnMewDirContinueSave    continue_save   = nullptr;
    FnSerializeCat          serialize_cat   = nullptr;
    FnCatById               cat_by_id       = nullptr;
    FnVoidPtr               regen_cat_parts = nullptr;
    FnAnimatorSetState      animator_set_state = nullptr;
    FnBsDtor                bs_dtor         = nullptr;
    FnOfstreamCtor          ofstream_ctor   = nullptr;
    FnVoidPtr               cat_ctor        = nullptr;
    FnVoidPtr               cat_dtor        = nullptr;
    FnVecPushBack           vec_cat_push    = nullptr;
    FnVecPushBack           vec_str_push    = nullptr;
    FnStrAssign             str_assign      = nullptr;
    FnWStrAssign            wstr_assign     = nullptr;
    FnPodBufResize          podbuf_resize   = nullptr;
    FnVoidPtr               inter_ctor      = nullptr;
    FnVoidPtr               inter_dtor      = nullptr;
    FnVecInterEmplace       vec_inter_empl  = nullptr;
    FnDoInterstitials       do_interstitials= nullptr;
    FnGoOnAdventure         go_on_adv       = nullptr;
    FnVoidPtr               poop_pop_orig   = nullptr;
    FnVoidPtr               autosave_house  = nullptr;
    FnVoidPtr               furn_remove     = nullptr;
    FnCreateFurnPiece       create_furn_piece = nullptr;
    FnDirectorAddScene      dir_add_scene   = nullptr;
    FnCreateActSel          create_act_sel  = nullptr;
    FnVoidPtr               class_tag_box_click = nullptr;
    FnCatSelectorShowStats  cat_selector_show_stats = nullptr;
    FnInvScreen2Lookup      inv_screen_lookup = nullptr;
    FnInvScreen2ItemEq      inv_screen_item_eq = nullptr;
    FnInvItemBoxClickApply  inv_item_box_click_apply = nullptr;

    // Lifecycle & phase
    HostPhase phase            = HostPhase::Outside;
    PrepStage prep_stage       = PrepStage::None;
    uint32_t  generation       = 0;
    uint32_t  roster_rev       = 1;
    bool      in_pass_day      = false;
    bool      in_return        = false;
    bool      need_day_publish = false;
    bool      need_ret_publish = false;
    bool      need_adv_publish = false;
    bool      need_prep_publish= false;
    bool      need_baseline    = false;
    bool      need_roster_publish = false;
    bool      suppress_local_drag = false;
    bool      in_native_client_pickup = false;
    bool      applying_remote_poop_pop = false;
    bool      suppress_autosave_publish = false;

    // Current House scene/actor index (game thread only, cleared on gen change)
    void* current_house     = nullptr;
    void* current_scene     = nullptr;
    void* current_click_mgr = nullptr;
    void* spoof_saved_held  = nullptr;
    bool  spoof_active      = false;
    std::unordered_map<int64_t, void*> cat_actors; // cat_id -> HouseCat*

    // Host state
    uint32_t next_capture_id  = 1;
    uint32_t last_full_ms     = 0;
    uint32_t last_move_ms     = 0;
    uint32_t last_drag_pose_ms = 0;
    uint32_t last_cursor_ms   = 0;
    int64_t  local_host_held  = 0;
    std::vector<int64_t> prev_actor_ids;
    std::vector<uint8_t> pending_interstitial_blob;
    bool                 has_pending_interstitial_blob = false;
    std::unordered_map<int64_t, HostCapture> host_captures;
    std::unordered_map<int64_t, HouseCatPose> last_sent_poses;
    std::unordered_map<int64_t, HouseCatPose> returning_first_poses;
    uint32_t return_first_tick = 0;

    // Preparation spectator state
    HousePrepMsg last_prep{};
    bool         have_prep = false;
    bool         peer_map_ready[kMaxPeers] = {};
    bool         peer_needs_checkpoint[kMaxPeers] = {};
    uint64_t     peer_map_deadline[kMaxPeers] = {};
    bool         waiting_first_node = false;
    uint32_t     client_map_generation = 0;
    uint32_t     sent_map_generation = 0;
    bool         pose_clock_valid = false;
    uint32_t     pose_clock_offset = 0;
    uint32_t     queued_baseline_tick = 0;
    bool         queued_returning = false;
    bool         playing_arrival = false;
    uint32_t     arrival_started_ms = 0;
    uint32_t     arrival_host_tick = 0;

    // Client state
    uint32_t client_gen           = 0;
    uint32_t client_roster_rev    = 0;
    uint32_t client_req_seq       = 1;
    uint32_t client_move_seq      = 1;
    int64_t  client_held_cat      = 0;
    uint32_t client_held_cap_id   = 0;
    uint32_t client_pending_req   = 0;
    int64_t  client_pending_cat   = 0;
    bool     client_waiting_house = false;
    uint32_t client_last_drag_ms  = 0;
    std::unordered_map<int64_t, ClientCatTrack> client_tracks;
    struct CatRecord {
        uint64_t hash = 0;
        std::vector<uint8_t> bytes;
    };
    std::unordered_map<int64_t, CatRecord> queued_cat_records;
    std::vector<HouseCatPose> queued_baseline_cats;
    uint32_t                  queued_baseline_gen = 0;
    bool                      has_queued_baseline = false;
    std::vector<uint8_t>      queued_interstitial_blob;
    uint32_t                  queued_interstitial_gen = 0;
    bool                      has_queued_interstitial = false;
    int                       reload_stage = 0; // 0 = idle, 1 = wait for old scenes
    static constexpr const char* reload_scenes[] = {
        kScene_House, "ClassChooser", "StorageItems", "ActSelection", "Map"
    };
    void*                     cached_director = nullptr;
    HousePrepMsg              pending_class_chooser{};
    uint32_t                  pending_class_chooser_gen = 0;
    bool                      has_pending_class_chooser = false;
    uint32_t                  entered_class_chooser_gen = 0;

    // Peer cursors & diagnostics
    PeerCursorState peer_cursors[kMaxPeers];
    uint8_t         known_peers[kMaxPeers] = {};
    uint8_t         known_peer_count = 0;
    uint32_t        stat_grabs_granted  = 0;
    uint32_t        stat_grabs_rejected = 0;
    uint32_t        stat_drops          = 0;
};

static HouseState g;

uint32_t now_ms() { return GetTickCount(); }

void write_game_std_string(void* str_addr, const char* s) {
    if (!str_addr || !s) return;
    char cur[64] = {};
    if (mem_read_std_string(str_addr, cur, sizeof(cur)) && strcmp(cur, s) == 0)
        return;

    uint8_t* raw = (uint8_t*)str_addr;
    uint64_t len = (uint64_t)strlen(s);
    uint64_t res = 0;
    memcpy(&res, raw + 24, 8);

    if (len <= 15 && res <= 15) {
        memcpy(raw, s, len + 1);
        memcpy(raw + 16, &len, 8);
        return;
    }
    if (res > 15 && len <= res) {
        char* ptr = nullptr;
        memcpy(&ptr, raw, 8);
        if (ptr) {
            memcpy(ptr, s, len + 1);
            memcpy(raw + 16, &len, 8);
            return;
        }
    }
    if (g.str_alloc && g.str_dtor) {
        g.str_dtor(str_addr);
        if (len <= 15) {
            memcpy(raw, s, len + 1);
            uint64_t sso = 15;
            memcpy(raw + 16, &len, 8);
            memcpy(raw + 24, &sso, 8);
        } else {
            uint64_t cap = len | 0xF;
            char* buf = (char*)g.str_alloc((size_t)(cap + 1));
            if (buf) {
                memcpy(buf, s, len + 1);
                memcpy(raw, &buf, 8);
                memcpy(raw + 16, &len, 8);
                memcpy(raw + 24, &cap, 8);
            }
        }
    }
}

bool get_cached_list(void* scene, int comp_id, void*** out_data, uint32_t* out_count) {
    *out_data  = nullptr;
    *out_count = 0;
    if (!scene || !g.update_cached) return false;
    g.update_cached(scene, comp_id);
    const uint8_t* lists = nullptr;
    if (!mem_read((const uint8_t*)scene + kScene_CachedCompLists, &lists, sizeof(lists)) || !lists)
        return false;
    const uint8_t* block = nullptr;
    if (!mem_read(lists + (size_t)comp_id * 16, &block, sizeof(block)) || !block)
        return false;
    uint32_t count = 0;
    void**   data  = nullptr;
    if (!mem_read(block + 12, &count, sizeof(count))) return false;
    if (count > 4096) return false;
    if (count > 0 && !mem_read(block + 16, &data, sizeof(data))) return false;
    *out_data  = data;
    *out_count = count;
    return true;
}

const void* current_cat_registry() {
    const uint8_t* director = nullptr;
    const void* registry = nullptr;
    if (g.director_slot && mem_read(g.director_slot, &director, sizeof(director)) && director)
        mem_read(director + kDir_CatRegistry, &registry, sizeof(registry));
    return registry;
}

void rebuild_actor_map() {
    g.cat_actors.clear();
    g.current_house = nullptr;
    g.current_scene = nullptr;

    const void* scene = nullptr;
    bool suspended = false;
    if (!leave_find_scene(kScene_House, &scene, &suspended) || !scene)
        return;

    g.current_scene = (void*)scene;

    void**   hdata  = nullptr;
    uint32_t hcount = 0;
    if (get_cached_list((void*)scene, kCompId_House, &hdata, &hcount) && hcount > 0 && hdata)
        g.current_house = hdata[0];

    void**   cdata  = nullptr;
    uint32_t ccount = 0;
    if (get_cached_list((void*)scene, kCompId_HouseCat, &cdata, &ccount) && cdata) {
        for (uint32_t i = 0; i < ccount; ++i) {
            void* hc = cdata[i];
            if (!hc) continue;
            uint8_t exists = 0, departed = 0;
            mem_read((const uint8_t*)hc + kHCat_Exists, &exists, 1);
            mem_read((const uint8_t*)hc + kHCat_Departed, &departed, 1);
            if (!exists || departed) continue;
            int64_t id = 0;
            if (mem_read((const uint8_t*)hc + kHCat_CatId, &id, sizeof(id)) && id != 0)
                g.cat_actors[id] = hc;
        }
    }
}

void* find_zone_by_name(const char* name) {
    if (!name || !name[0] || !g.current_scene) return nullptr;
    void**   zdata  = nullptr;
    uint32_t zcount = 0;
    if (!get_cached_list(g.current_scene, kCompId_CatPlacementArea, &zdata, &zcount) || !zdata)
        return nullptr;
    for (uint32_t i = 0; i < zcount; ++i) {
        void* area = zdata[i];
        if (!area) continue;
        char rname[48] = {};
        if (mem_read_std_string((const uint8_t*)area + kCPA_RoomName, rname, sizeof(rname)) &&
            strcmp(rname, name) == 0)
            return area;
    }
    return nullptr;
}

Vec2D cat_drag_anchor(const void* cat, bool flipped = true) {
    const auto* hc = static_cast<const uint8_t*>(cat);
    const uint8_t* transform = nullptr;
    const uint8_t* bounds = nullptr;
    Vec2D scale{}, flip{}, center{};
    if (!mem_read(hc + kHCat_Transform, &transform, sizeof(transform)) || !transform ||
        !mem_read(hc + kHCat_ClickBounds, &bounds, sizeof(bounds)) || !bounds ||
        !mem_read(transform + kTransform_Scale, &scale, sizeof(scale)) ||
        !mem_read(transform + kTransform_Flip, &flip, sizeof(flip)) ||
        !mem_read(bounds + kCollider_DragCenter, &center, sizeof(center)))
        return {};
    return {center.x * scale.x * (flipped ? flip.x : 1.0),
            center.y * scale.y * (flipped ? flip.y : 1.0)};
}

void move_held_cat(void* hc, float wx, float wy) {
    void* transform = nullptr;
    if (mem_read(static_cast<const uint8_t*>(hc) + kHCat_Transform,
                 &transform, sizeof(transform)) && transform) {
        const auto anchor = cat_drag_anchor(hc);
        const Vec3D pos{wx - anchor.x, wy - anchor.y, 0.0};
        memcpy(static_cast<uint8_t*>(transform) + kTransform_Pos, &pos, sizeof(pos));
    }
}

bool is_house_pipe(const void* area) {
    if (!area) return false;
    const auto* vtable = *static_cast<const uintptr_t* const*>(area);
    return vtable[kCPA_TryPlaceSlot] == addr_of(T_HousePipePlace);
}

bool read_housecat_pose(void* hc, HouseCatPose& out) {
    if (!hc) return false;
    int64_t id = 0;
    if (!mem_read((const uint8_t*)hc + kHCat_CatId, &id, sizeof(id)) || id == 0)
        return false;
    out.cat_id = id;

    void* tr = nullptr;
    if (mem_read((const uint8_t*)hc + kHCat_Transform, &tr, sizeof(tr)) && tr) {
        Vec3D pos{};
        if (mem_read((const uint8_t*)tr + kTransform_Pos, &pos, sizeof(pos))) {
            out.x = (float)pos.x;
            out.y = (float)pos.y;
            out.z = (float)pos.z;
        }
        Vec2D flip{};
        if (mem_read((const uint8_t*)tr + kTransform_Flip, &flip, sizeof(flip)))
            out.flip = (flip.x < 0.0) ? -1 : 1;
    }

    void* phys = nullptr;
    if (mem_read((const uint8_t*)hc + kHCat_Physics, &phys, sizeof(phys)) && phys) {
        Vec2D vel{};
        if (mem_read((const uint8_t*)phys + kHCatPhys_Velocity, &vel, sizeof(vel))) {
            out.vx = (float)vel.x;
            out.vy = (float)vel.y;
        }
    }

    mem_read((const uint8_t*)hc + kHCat_Depth, &out.depth, sizeof(out.depth));

    void* rend = nullptr;
    if (mem_read((const uint8_t*)hc + kHCat_Renderer, &rend, sizeof(rend)) && rend) {
        uint8_t vis = 1;
        mem_read((const uint8_t*)rend + kRenderer_Visible, &vis, 1);
        out.visible = vis ? 1 : 0;
        mem_read((const uint8_t*)rend + kRenderer_RenderLayer,
                 &out.render_layer, sizeof(out.render_layer));
    }

    void* anim = nullptr;
    if (mem_read((const uint8_t*)hc + kHCat_Animator, &anim, sizeof(anim)) && anim) {
        const uint8_t* st = (const uint8_t*)anim + kAnim_CurrentState;
        mem_read(st + kAnimState_State, &out.anim_state, sizeof(out.anim_state));
        int32_t aflip = 1;
        if (mem_read(st + kAnimState_Flip, &aflip, sizeof(aflip)) && aflip != 0)
            out.flip = (aflip < 0) ? -1 : 1;
        mem_read_std_string(st + kAnimState_Anim, out.anim, sizeof(out.anim));
        mem_read_std_string(st + kAnimState_Substate, out.substate, sizeof(out.substate));
    }

    void* loc = nullptr;
    out.zone[0] = 0;
    if (mem_read((const uint8_t*)hc + kHCat_CurrentLocation, &loc, sizeof(loc)) && loc) {
        if (is_house_pipe(loc))
            out.visible = 0; // The host masks this cat with the actual pipe renderer.
        else
            mem_read_std_string((const uint8_t*)loc + kCPA_RoomName, out.zone, sizeof(out.zone));
    }

    auto it = g.host_captures.find(id);
    if (it != g.host_captures.end()) {
        out.holder_peer = it->second.holder_peer;
        out.capture_id  = it->second.capture_id;
    } else {
        out.holder_peer = kNoPeer;
        out.capture_id  = 0;
    }
    return true;
}

struct RoomCats { uint32_t capacity, count; void** data; };

RoomCats* room_cats(void* area) {
    return reinterpret_cast<RoomCats*>(static_cast<uint8_t*>(area) + kCPA_OwnedCats);
}

void remove_client_room_member(void* area, void* hc) {
    auto* cats = room_cats(area);
    if (!cats->data) return;
    for (uint32_t i = 0; i < cats->count; ++i) {
        if (cats->data[i] == hc) {
            cats->data[i] = cats->data[--cats->count];
            break;
        }
    }
}

void apply_client_room(void* hc, void* target) {
    auto** current = reinterpret_cast<void**>(static_cast<uint8_t*>(hc) + kHCat_CurrentLocation);
    if (*current == target) return;

    RoomCats* next = nullptr;
    if (target) {
        next = room_cats(target);
        if (next->count >= kMaxHouseCats) return;
        if (next->count == next->capacity) {
            if (!g.podbuf_resize) return;
            g.podbuf_resize(next, next->count + 1);
        }
        if (!next->data) return;
    }
    // Native placement callbacks can mutate CatData or trigger ButchBox effects.
    if (*current) remove_client_room_member(*current, hc);
    if (next) next->data[next->count++] = hc;
    *current = target;
}

void prepare_held_ragdoll(void* cat) {
    const auto* ragdoll = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(cat) + kHCat_Ragdoll);
    if (!ragdoll) return;
    auto* physics = *reinterpret_cast<uint8_t* const*>(ragdoll + kCatRagdoll_Physics);
    if (!physics) return;
    constexpr double infinity = std::numeric_limits<double>::infinity();
    const double bounds[] = { -infinity, infinity, -infinity, infinity };
    memcpy(physics + kRagdoll_ConstrainBounds, bounds, sizeof(bounds));
    *reinterpret_cast<uint32_t*>(physics + kRagdoll_ExtraPlaneCount) = 0;
}

void apply_pose_to_client_actor(void* hc, const HouseCatPose& p, bool snap_zone) {
    if (!hc) return;

    void* tr = nullptr;
    if (mem_read((const uint8_t*)hc + kHCat_Transform, &tr, sizeof(tr)) && tr) {
        Vec3D pos{ (double)p.x, (double)p.y, (double)p.z };
        memcpy((uint8_t*)tr + kTransform_Pos, &pos, sizeof(pos));
        Vec2D flip{ (double)(p.flip < 0 ? -1.0 : 1.0), 1.0 };
        memcpy((uint8_t*)tr + kTransform_Flip, &flip, sizeof(flip));
        const double sort_depth = -static_cast<double>(p.depth);
        memcpy((uint8_t*)tr + kTransform_SortDepth, &sort_depth, sizeof(sort_depth));
        memcpy((uint8_t*)tr + kTransform_PrevSortDepth, &sort_depth, sizeof(sort_depth));
    }

    int32_t depth = p.depth;
    memcpy((uint8_t*)hc + kHCat_Depth, &depth, sizeof(depth));

    uint8_t no_overlap = 0;
    memcpy((uint8_t*)hc + kHCat_CheckOverlaps, &no_overlap, 1);

    const bool held = p.holder_peer != kNoPeer;
    if (held) prepare_held_ragdoll(hc);
    uint8_t rag = (held || p.anim_state == 8 || p.anim_state == 9) ? 1 : 0;
    memcpy((uint8_t*)hc + kHCat_Ragdolling, &rag, 1);

    void* rend = nullptr;
    if (mem_read((const uint8_t*)hc + kHCat_Renderer, &rend, sizeof(rend)) && rend) {
        uint8_t vis = p.visible ? 1 : 0;
        memcpy((uint8_t*)rend + kRenderer_Visible, &vis, 1);
        auto* bytes = static_cast<uint8_t*>(rend);
        auto& layer = *reinterpret_cast<int32_t*>(bytes + kRenderer_RenderLayer);
        if (layer != p.render_layer) {
            layer = p.render_layer;
            if (*reinterpret_cast<void**>(bytes + kRenderer_RenderCore)) {
                const auto* vtable = *static_cast<FnVoidPtr**>(rend);
                vtable[kRenderer_RefreshLayerSlot](rend);
            }
        }
    }

    void* anim = nullptr;
    if (mem_read((const uint8_t*)hc + kHCat_Animator, &anim, sizeof(anim)) && anim) {
        uint8_t* req = (uint8_t*)anim + kAnim_RequestedState;
        int32_t aflip = p.flip < 0 ? -1 : 1;
        if (snap_zone) {
            memcpy(req + kAnimState_State, &p.anim_state, sizeof(p.anim_state));
            if (p.anim[0]) write_game_std_string(req + kAnimState_Anim, p.anim);
            write_game_std_string(req + kAnimState_Substate, p.substate);
        } else if (g.animator_set_state &&
                   *reinterpret_cast<int32_t*>(req + kAnimState_State) != p.anim_state) {
            // SetState resolves the actual ragdoll clip from the cat's definition.
            // It consumes the by-value string using the game's allocator.
            struct {
                char text[16] = "_DEFAULT";
                uint64_t size = 8;
                uint64_t capacity = 15;
            } substate;
            g.animator_set_state(anim, p.anim_state, &substate);
        }
        memcpy(req + kAnimState_Flip, &aflip, sizeof(aflip));
    }

    if (snap_zone)
        apply_client_room(hc, p.zone[0] ? find_zone_by_name(p.zone) : nullptr);
}

void clear_generation_state() {
    g.cat_actors.clear();
    g.prev_actor_ids.clear();
    g.current_house     = nullptr;
    g.current_scene     = nullptr;
    g.current_click_mgr = nullptr;
    g.spoof_saved_held  = nullptr;
    g.spoof_active      = false;
    g.host_captures.clear();
    g.last_sent_poses.clear();
    g.returning_first_poses.clear();
    g.return_first_tick = 0;
    g.need_roster_publish = false;
    g.pose_clock_valid = false;
    g.playing_arrival = false;
    g.local_host_held    = 0;
    g.client_held_cat    = 0;
    g.client_held_cap_id = 0;
    g.client_pending_req = 0;
    g.client_pending_cat = 0;
    g.client_tracks.clear();
    g.queued_cat_records.clear();
    g.client_roster_rev = 0;
    g.client_map_generation = 0;
    g.sent_map_generation = 0;
    g.has_pending_class_chooser = false;
    g.entered_class_chooser_gen = 0;
    g.queued_interstitial_blob.clear();
    g.has_queued_interstitial = false;
    g.queued_baseline_cats.clear();
    g.has_queued_baseline = false;
    g.have_prep = false;
    memset(&g.last_prep, 0, sizeof(g.last_prep));
    for (int i = 0; i < kMaxPeers; ++i)
        g.peer_cursors[i].valid = false;
}

void advance_generation(const char* why) {
    ++g.generation;
    ++g.roster_rev;
    clear_generation_state();
    log_line("HOUSE", "generation -> %u (%s)", g.generation, why ? why : "");
}

static bool live_house_scene_valid() {
    if (!g.current_house || !g.current_scene) return false;
    const void* hs = nullptr;
    bool susp = false;
    if (!leave_find_scene(kScene_House, &hs, &susp) || hs != g.current_scene)
        return false;
    uint8_t destroying = 0;
    if (!mem_read((const uint8_t*)hs + kScene_Destroying, &destroying, 1) || destroying)
        return false;
    return true;
}

bool house_mouse_world(float* out_wx, float* out_wy) {
    if (!live_house_scene_valid() || !g.cam_get_mouse) return false;
    void* cam = nullptr;
    if (!mem_read((const uint8_t*)g.current_house + kHouse_MainCam, &cam, sizeof(cam)) || !cam)
        return false;
    Vec2D w{};
    g.cam_get_mouse(cam, &w);
    if (!std::isfinite(w.x) || !std::isfinite(w.y)) return false;
    *out_wx = (float)w.x;
    *out_wy = (float)w.y;
    return true;
}

bool project_world_to_norm(float wx, float wy, float* out_nx, float* out_ny) {
    if (!live_house_scene_valid() || !g.cam_matrix || !g.screen_bounds) return false;
    void* cam = nullptr;
    if (!mem_read((const uint8_t*)g.current_house + kHouse_MainCam, &cam, sizeof(cam)) || !cam)
        return false;
    alignas(16) float m[16] = {};
    g.cam_matrix(cam, m);
    float sx = m[0] * wx + m[4] * wy + m[12];
    float sy = m[1] * wx + m[5] * wy + m[13];

    float left   = g.screen_bounds[0];
    float right  = g.screen_bounds[1];
    float top    = g.screen_bounds[2];
    float bottom = g.screen_bounds[3];
    if (fabsf(right - left) < 1e-5f || fabsf(top - bottom) < 1e-5f) return false;

    *out_nx = (sx - left) / (right - left);
    *out_ny = (sy - bottom) / (top - bottom);
    return std::isfinite(*out_nx) && std::isfinite(*out_ny);
}

void send_cat_records(const std::vector<HouseCatPose>& poses, uint16_t count, uint8_t peer) {
    const void* registry = current_cat_registry();
    if (!registry || !g.cat_by_id) return;
    unsigned sent = 0;
    for (uint16_t i = 0; i < count; ++i) {
        const int64_t id = poses[i].cat_id;
        void* cat = g.cat_by_id(registry, id);
        if (!cat) continue;
        CatDataMsg record{};
        record.id = static_cast<uint64_t>(id);
        record.house_generation = g.generation;
        if (catsync_pack_cat(cat, record)) {
            if (net_send_catdata(record, peer)) ++sent;
            free(record.data);
        }
    }
    log_line("HOUSE", "-> %u of %u live cat records (gen=%u)", sent,
             (unsigned)count, g.generation);
}

void send_baseline(uint8_t target_peer, bool with_cat_records = false, bool returning = false) {
    if (g.is_client || !net_active()) return;
    rebuild_actor_map();

    std::vector<HouseCatPose> list;
    list.reserve(g.cat_actors.size());
    for (auto& kv : g.cat_actors) {
        HouseCatPose p{};
        if (!read_housecat_pose(kv.second, p)) continue;
        const auto first = g.returning_first_poses.find(p.cat_id);
        list.push_back(returning && first != g.returning_first_poses.end() ? first->second : p);
    }

    uint8_t night = 0;
    if (g.current_house)
        mem_read((const uint8_t*)g.current_house + kHouse_Night, &night, 1);

    HouseBaselineMsg msg{};
    msg.generation   = g.generation;
    msg.roster_rev   = g.roster_rev;
    msg.host_tick_ms = returning && g.return_first_tick ? g.return_first_tick : now_ms();
    msg.phase        = (uint8_t)g.phase;
    msg.night        = night ? 1 : 0;
    msg.returning    = returning && !g.returning_first_poses.empty();
    msg.count        = (uint16_t)(list.size() > kMaxHouseCats ? kMaxHouseCats : list.size());
    msg.cats         = list.empty() ? nullptr : list.data();

    const bool sent = target_peer == kNoPeer ? net_send_house_baseline(msg)
                                             : net_send_house_baseline_to(target_peer, msg);
    if (sent && with_cat_records)
        send_cat_records(list, msg.count, target_peer);

    log_line("HOUSE", "-> BASELINE gen=%u cats=%u phase=%u (%s)",
             msg.generation, (unsigned)msg.count, (unsigned)msg.phase,
             target_peer == kNoPeer ? "broadcast" : "peer");
}

bool discrete_pose_changed(const HouseCatPose& a, const HouseCatPose& b) {
    if (a.holder_peer != b.holder_peer || a.capture_id != b.capture_id) return true;
    if (a.flip != b.flip || a.depth != b.depth || a.visible != b.visible ||
        a.render_layer != b.render_layer || a.anim_state != b.anim_state)
        return true;
    if (strcmp(a.anim, b.anim) != 0 || strcmp(a.substate, b.substate) != 0 ||
        strcmp(a.zone, b.zone) != 0)
        return true;
    return false;
}

bool pose_moved(const HouseCatPose& a, const HouseCatPose& b) {
    return fabsf(a.x - b.x) > 0.005f || fabsf(a.y - b.y) > 0.005f ||
           fabsf(a.z - b.z) > 0.005f;
}

void host_release_capture(int64_t cat_id, float wx, float wy, bool apply_drop, Vec2D velocity = {});

void host_sample_and_send_poses() {
    if (g.phase != HostPhase::House || !net_active()) return;
    rebuild_actor_map();
    if (!g.current_house) return;

    uint32_t t = now_ms();
    bool do_full = (t - g.last_full_ms >= tune::kHouseFullPoseMs);
    bool do_move = (t - g.last_move_ms >= tune::kHouseMovePoseMs);
    const bool do_drag = t - g.last_drag_pose_ms >= tune::kHouseCursorMs;

    // Detect mid-day cat additions or removals (e.g. flushing a retiree down
    // the NPC pipe) and broadcast a fresh BASELINE so the client removes them.
    std::vector<int64_t> cur_ids;
    cur_ids.reserve(g.cat_actors.size());
    for (const auto& kv : g.cat_actors) cur_ids.push_back(kv.first);
    std::sort(cur_ids.begin(), cur_ids.end());
    if (cur_ids != g.prev_actor_ids) {
        const bool added_cat = !g.prev_actor_ids.empty() &&
            !std::includes(g.prev_actor_ids.begin(), g.prev_actor_ids.end(),
                           cur_ids.begin(), cur_ids.end());
        std::vector<int64_t> removed_captures;
        for (const auto& item : g.host_captures)
            if (!g.cat_actors.count(item.first)) removed_captures.push_back(item.first);
        for (int64_t id : removed_captures) {
            if (g.local_host_held == id && g.current_click_mgr) {
                void* empty = nullptr;
                memcpy(static_cast<uint8_t*>(g.current_click_mgr) + kHCatClick_HeldCat,
                       &empty, sizeof(empty));
            }
            host_release_capture(id, 0.0f, 0.0f, false);
            g.last_sent_poses.erase(id);
        }
        ++g.roster_rev;
        g.prev_actor_ids = cur_ids;
        if (!g.need_day_publish && !g.need_ret_publish) {
            g.need_roster_publish |= added_cat;
            if (!g.need_roster_publish) send_baseline(kNoPeer);
        }
    }

    std::vector<HouseCatPose> out;
    out.reserve(g.cat_actors.size());

    for (auto& kv : g.cat_actors) {
        HouseCatPose p{};
        if (!read_housecat_pose(kv.second, p)) continue;
        if (do_full) {
            out.push_back(p);
            g.last_sent_poses[p.cat_id] = p;
        } else {
            auto it = g.last_sent_poses.find(p.cat_id);
            if (it == g.last_sent_poses.end() || discrete_pose_changed(it->second, p) ||
                ((p.capture_id ? do_drag : do_move) && pose_moved(it->second, p))) {
                out.push_back(p);
                g.last_sent_poses[p.cat_id] = p;
            }
        }
    }

    if (do_full) g.last_full_ms = t;
    if (do_move) g.last_move_ms = t;
    if (do_drag) g.last_drag_pose_ms = t;
    if (out.empty()) return;

    HousePoseMsg msg{};
    msg.generation   = g.generation;
    msg.host_tick_ms = t;
    msg.full         = do_full ? 1 : 0;
    msg.count        = (uint16_t)(out.size() > kMaxHouseCats ? kMaxHouseCats : out.size());
    msg.cats         = out.data();
    net_send_house_pose(msg);
}

Vec2D clamp_throw_velocity(Vec2D velocity) {
    constexpr double max_speed = 20.0; // HouseCatPhysics::update
    const double speed = std::hypot(velocity.x, velocity.y);
    if (speed <= max_speed) return velocity;
    const double scale = max_speed / speed;
    return {velocity.x * scale, velocity.y * scale};
}

void sample_client_drag(ClientCatTrack& track, uint32_t now) {
    float wx = track.pred_wx, wy = track.pred_wy;
    if (!house_mouse_world(&wx, &wy)) {
        track.throw_velocity = {};
        track.last_motion_ms = now;
        return;
    }
    const double dx = double(wx) - track.pred_wx;
    const double dy = double(wy) - track.pred_wy;
    const uint32_t elapsed = now - track.last_motion_ms;
    // Keep the final moving sample across a stationary tick at button-up.
    if (elapsed == 0) return;
    if (dx != 0 || dy != 0) {
        const double scale = 500.0 / elapsed; // native half-speed, from ms to seconds
        track.throw_velocity = clamp_throw_velocity({dx * scale, dy * scale});
        track.last_motion_ms = now;
    } else if (elapsed >= 50) {
        track.throw_velocity = {};
        track.last_motion_ms = now;
    }
    track.pred_wx = wx;
    track.pred_wy = wy;
}

void host_release_capture(int64_t cat_id, float wx, float wy, bool apply_drop, Vec2D velocity) {
    auto it = g.host_captures.find(cat_id);
    if (it == g.host_captures.end()) return;
    HostCapture cap = it->second;
    g.host_captures.erase(it);
    if (cap.holder_peer == kHostPeer && g.local_host_held == cat_id)
        g.local_host_held = 0;

    char zone[48] = {};
    bool accepted = false;
    bool in_pipe = false;
    HouseCatPose pose{};
    bool have_pose = false;
    auto ait = g.cat_actors.find(cat_id);
    if (ait != g.cat_actors.end() && ait->second) {
        void* hc = ait->second;
        memcpy((uint8_t*)hc + kHCat_Depth, &cap.orig_depth, sizeof(cap.orig_depth));

        if (apply_drop) {
            move_held_cat(hc, wx, wy);
            if (cap.holder_peer != kHostPeer) {
                void* phys = nullptr;
                if (mem_read((const uint8_t*)hc + kHCat_Physics, &phys, sizeof(phys)) && phys) {
                    const Vec2D vel = clamp_throw_velocity(velocity);
                    const auto anchor = cat_drag_anchor(hc);
                    const Vec2D ppos{wx - anchor.x, wy - anchor.y};
                    uint8_t zero = 0;
                    memcpy((uint8_t*)phys + kHCatPhys_Velocity, &vel, sizeof(vel));
                    memcpy((uint8_t*)phys + kHCatPhys_PrevPos, &ppos, sizeof(ppos));
                    memcpy((uint8_t*)phys + kHCatPhys_PrevDragging, &zero, 1);
                }
                uint8_t rag = 1;
                memcpy((uint8_t*)hc + kHCat_Ragdolling, &rag, 1);
            }
            if (g.current_house && g.loc_under_point) {
                // Native drop measures the click bounds without visual mirroring.
                const auto held_anchor = cat_drag_anchor(hc);
                const auto drop_anchor = cat_drag_anchor(hc, false);
                Vec2D pt{wx - held_anchor.x + drop_anchor.x, wy - held_anchor.y + drop_anchor.y};
                void* area = g.loc_under_point(g.current_house, &pt);
                if (area && g.place_cat) {
                    g.place_cat(area, hc);
                }
            }
        }
        have_pose = read_housecat_pose(hc, pose);
        if (have_pose) {
            wx = pose.x;
            wy = pose.y;
            strcpy_s(zone, pose.zone);
            void* location = nullptr;
            mem_read(static_cast<uint8_t*>(hc) + kHCat_CurrentLocation, &location, sizeof(location));
            in_pipe = is_house_pipe(location);
            accepted = location != nullptr;
        }
    }

    ++g.stat_drops;
    log_line("HOUSE", "released cat %lld (peer=%u capture=%u move=%u zone='%s' velocity=%.2f,%.2f)",
             (long long)cat_id, (unsigned)cap.holder_peer, cap.capture_id, cap.last_seq,
             in_pipe ? "HousePipe" : zone,
             pose.vx, pose.vy);
    HouseDropRespMsg resp{};
    resp.generation  = g.generation;
    resp.cat_id      = cat_id;
    resp.capture_id  = cap.capture_id;
    resp.holder_peer = cap.holder_peer;
    resp.wx          = wx;
    resp.wy          = wy;
    strncpy_s(resp.zone, zone, _TRUNCATE);
    resp.accepted    = accepted ? 1 : 0;
    net_send_house_drop_resp(resp);
    if (have_pose) {
        HousePoseMsg snapshot{g.generation, now_ms(), 0, 1, &pose};
        net_send_house_pose(snapshot);
    }
}

void host_release_all_captures() {
    std::vector<int64_t> ids;
    ids.reserve(g.host_captures.size());
    for (auto& kv : g.host_captures) ids.push_back(kv.first);
    for (int64_t id : ids) {
        auto it = g.host_captures.find(id);
        if (it != g.host_captures.end())
            host_release_capture(id, it->second.wx, it->second.wy, true);
    }
    if (g.current_click_mgr) {
        void* empty = nullptr;
        memcpy(static_cast<uint8_t*>(g.current_click_mgr) + kHCatClick_HeldCat,
               &empty, sizeof(empty));
    }
}

bool host_try_grant_grab(uint8_t peer, int64_t cat_id, uint32_t req_seq, float wx, float wy) {
    if (g.phase != HostPhase::House) {
        ++g.stat_grabs_rejected;
        HouseGrabRespMsg resp{ g.generation, cat_id, kNoPeer, 0, 0, req_seq, wx, wy };
        net_send_house_grab_resp_to(peer, resp);
        return false;
    }

    // Enforce max 1 held cat per peer.
    for (auto& kv : g.host_captures) {
        if (kv.second.holder_peer == peer) {
            ++g.stat_grabs_rejected;
            HouseGrabRespMsg resp{ g.generation, cat_id, kv.second.holder_peer,
                                   kv.second.capture_id, 0, req_seq, wx, wy };
            if (peer != kHostPeer) net_send_house_grab_resp_to(peer, resp);
            return false;
        }
    }

    // Already held by another peer?
    auto existing = g.host_captures.find(cat_id);
    if (existing != g.host_captures.end()) {
        ++g.stat_grabs_rejected;
        HouseGrabRespMsg resp{ g.generation, cat_id, existing->second.holder_peer,
                               existing->second.capture_id, 0, req_seq,
                               existing->second.wx, existing->second.wy };
        if (peer != kHostPeer) net_send_house_grab_resp_to(peer, resp);
        return false;
    }

    auto ait = g.cat_actors.find(cat_id);
    if (ait == g.cat_actors.end() || !ait->second) {
        ++g.stat_grabs_rejected;
        HouseGrabRespMsg resp{ g.generation, cat_id, kNoPeer, 0, 0, req_seq, wx, wy };
        if (peer != kHostPeer) net_send_house_grab_resp_to(peer, resp);
        return false;
    }

    void* hc = ait->second;
    // A peer may still be looking at a pose from before the host placed the cat.
    void* location = nullptr;
    mem_read(static_cast<uint8_t*>(hc) + kHCat_CurrentLocation, &location, sizeof(location));
    if (peer != kHostPeer && is_house_pipe(location)) {
        ++g.stat_grabs_rejected;
        HouseGrabRespMsg resp{g.generation, cat_id, kNoPeer, 0, 0, req_seq, wx, wy};
        net_send_house_grab_resp_to(peer, resp);
        return false;
    }
    HostCapture cap{};
    cap.cat_id      = cat_id;
    cap.holder_peer = peer;
    cap.capture_id  = g.next_capture_id++;
    cap.last_seq    = 0;
    cap.wx           = wx;
    cap.wy           = wy;
    mem_read((const uint8_t*)hc + kHCat_Depth, &cap.orig_depth, sizeof(cap.orig_depth));

    // Preserve native pickup semantics: remove from room and raise depth.
    if (location && g.remove_cat) {
        g.remove_cat(location, hc);
        void* null_ptr = nullptr;
        memcpy((uint8_t*)hc + kHCat_CurrentLocation, &null_ptr, sizeof(null_ptr));
    }
    if (peer == kHostPeer) {
        // The native click manager has already raised depth.
        cap.orig_depth -= 100000;
    } else {
        const int32_t raised_depth = cap.orig_depth + 100000;
        memcpy(static_cast<uint8_t*>(hc) + kHCat_Depth, &raised_depth, sizeof(raised_depth));
        move_held_cat(hc, wx, wy);
    }

    g.host_captures[cat_id] = cap;
    if (peer == kHostPeer) g.local_host_held = cat_id;
    ++g.stat_grabs_granted;
    log_line("HOUSE", "granted cat %lld to peer %u (capture=%u)",
             (long long)cat_id, (unsigned)peer, cap.capture_id);

    HouseGrabRespMsg resp{};
    resp.generation  = g.generation;
    resp.cat_id      = cat_id;
    resp.holder_peer = peer;
    resp.capture_id  = cap.capture_id;
    resp.accepted    = 1;
    resp.req_seq     = req_seq;
    resp.wx          = wx;
    resp.wy          = wy;
    net_send_house_grab_resp(resp);
    return true;
}

void read_prep_equipment(const uint8_t* cat, HousePrepCat& summary) {
    size_t used = 0;
    for (size_t slot = 0; slot < 5; ++slot) {
        char item[48]{};
        const auto* name = cat + kCatData_Equipment + slot * kEquipment_Size + kEquipment_Name;
        if (!mem_read_std_string(name, item, sizeof(item)) || !item[0]) continue;
        strncpy_s(summary.eq_slots[slot], item, _TRUNCATE);
        const int written = sprintf_s(summary.eq_summary + used,
                                      sizeof(summary.eq_summary) - used,
                                      "%s%s", used ? ", " : "", item);
        if (written > 0) used += written;
    }
}

void read_prep_class_tags(HousePrepMsg& summary) {
    const void* scene = nullptr;
    bool suspended = false;
    if (!leave_find_scene("ClassChooser", &scene, &suspended) || suspended) return;
    void** choosers = nullptr;
    uint32_t chooser_count = 0;
    if (get_cached_list(const_cast<void*>(scene), kCompId_ClassChooser, &choosers, &chooser_count) &&
        chooser_count > 0 && choosers && choosers[0]) {
        int64_t focused = 0;
        if (mem_read((const uint8_t*)choosers[0] + kClassChooser_CurrentCat, &focused, sizeof(focused)))
            summary.focused_cat_id = focused;
        void* selector = nullptr;
        if (mem_read((const uint8_t*)choosers[0] + kClassChooser_Selector, &selector, sizeof(selector)) && selector) {
            int32_t sel_idx = 0;
            if (mem_read((const uint8_t*)selector + kCatSelector_SelectedIndex, &sel_idx, sizeof(sel_idx)))
                summary.focused_index = sel_idx;
        }
    }
    void** boxes = nullptr;
    uint32_t count = 0;
    if (!get_cached_list(const_cast<void*>(scene), kCompId_ClassTagBox, &boxes, &count) || !boxes)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        const auto* box = static_cast<const uint8_t*>(boxes[i]);
        int64_t id = -1;
        if (!box || !mem_read(box + kClassTagBox_CatId, &id, sizeof(id)) || id == -1) continue;
        for (uint8_t cat = 0; cat < summary.count; ++cat) {
            auto& entry = summary.cats[cat];
            if (entry.cat_id == id) {
                mem_read_std_string(box + kClassTagBox_Class, entry.cat_class,
                                    sizeof(entry.cat_class));
                break;
            }
        }
    }
}

void read_prep_storage_focus(HousePrepMsg& summary) {
    const void* scene = nullptr;
    bool suspended = false;
    if (!leave_find_scene("StorageItems", &scene, &suspended) || suspended) return;
    void** screens = nullptr;
    uint32_t screen_count = 0;
    if (!get_cached_list(const_cast<void*>(scene), kCompId_InventoryScreen2, &screens, &screen_count) ||
        screen_count == 0 || !screens || !screens[0])
        return;
    int64_t focused = 0;
    if (mem_read((const uint8_t*)screens[0] + kInvScreen2_CurrentCat, &focused, sizeof(focused)))
        summary.focused_cat_id = focused;
    void* selector = nullptr;
    if (mem_read((const uint8_t*)screens[0] + kInvScreen2_Selector, &selector, sizeof(selector)) && selector) {
        int32_t sel_idx = 0;
        if (mem_read((const uint8_t*)selector + kCatSelector_SelectedIndex, &sel_idx, sizeof(sel_idx)))
            summary.focused_index = sel_idx;
    }
}

void client_sync_class_chooser() {
    if (!g.is_client || !g.have_prep || g.last_prep.stage != (uint8_t)PrepStage::ClassChooser)
        return;
    if (!g.class_tag_box_click || !g.cat_selector_show_stats)
        return;

    const void* cc_scene = nullptr;
    bool cc_susp = false;
    if (!leave_find_scene("ClassChooser", &cc_scene, &cc_susp) || !cc_scene || cc_susp)
        return;

    uint8_t destroying = 0;
    if (!mem_read((const uint8_t*)cc_scene + kScene_Destroying, &destroying, 1) || destroying)
        return;

    void** choosers = nullptr;
    uint32_t chooser_count = 0;
    if (!get_cached_list(const_cast<void*>(cc_scene), kCompId_ClassChooser, &choosers, &chooser_count) ||
        chooser_count == 0 || !choosers || !choosers[0])
        return;
    void* chooser = choosers[0];

    void* selector = nullptr;
    if (!mem_read((const uint8_t*)chooser + kClassChooser_Selector, &selector, sizeof(selector)) || !selector)
        return;

    void** boxes = nullptr;
    uint32_t box_count = 0;
    get_cached_list(const_cast<void*>(cc_scene), kCompId_ClassTagBox, &boxes, &box_count);

    const void* reg = current_cat_registry();
    const auto& prep = g.last_prep;

    auto is_party_cat = [&](int64_t cid) -> bool {
        if (cid == 0) return false;
        for (uint8_t i = 0; i < prep.count; ++i)
            if (prep.cats[i].cat_id == cid) return true;
        return false;
    };

    auto** scene_current = reinterpret_cast<void**>(addr_of_data(D_SceneCurrent));
    void* prev_scene = scene_current ? *scene_current : nullptr;
    if (scene_current) *scene_current = const_cast<void*>(cc_scene);

    auto focus_cat = [&](int64_t cid, bool apply_host_index) {
        if (!is_party_cat(cid)) return;
        if (apply_host_index)
            *(int32_t*)((uintptr_t)selector + kCatSelector_SelectedIndex) = prep.focused_index;
        int64_t sel_cur = 0;
        mem_read((const uint8_t*)selector + kCatSelector_CurrentCat, &sel_cur, sizeof(sel_cur));
        if (sel_cur != cid)
            g.cat_selector_show_stats(selector, cid);
        *(int64_t*)((uintptr_t)chooser + kClassChooser_CurrentCat) = cid;
    };

    // Pass 1: unequip any ClassTagBox whose assigned cat no longer wants that collar.
    if (boxes && box_count > 0) {
        for (uint32_t bi = 0; bi < box_count; ++bi) {
            void* box = boxes[bi];
            if (!box) continue;
            int64_t box_cid = -1;
            if (!mem_read((const uint8_t*)box + kClassTagBox_CatId, &box_cid, sizeof(box_cid)) || box_cid == -1)
                continue;
            char box_cls[48]{};
            mem_read_std_string((const uint8_t*)box + kClassTagBox_Class, box_cls, sizeof(box_cls));
            for (uint8_t ci = 0; ci < prep.count; ++ci) {
                if (prep.cats[ci].cat_id != box_cid) continue;
                if (strcmp(box_cls, prep.cats[ci].cat_class) != 0) {
                    focus_cat(box_cid, box_cid == prep.focused_cat_id);
                    g.class_tag_box_click(box);
                    log_line("HOUSE", "client ClassChooser unequip '%s' from cat %lld",
                             box_cls, (long long)box_cid);
                }
                break;
            }
        }
    }

    // Pass 2: equip missing collars for each cat.
    if (boxes && box_count > 0) {
        for (uint8_t ci = 0; ci < prep.count; ++ci) {
            const int64_t cid = prep.cats[ci].cat_id;
            const char* desired = prep.cats[ci].cat_class;
            if (cid == 0 || !desired[0] || strcmp(desired, "Colorless") == 0) continue;

            bool already_box = false;
            for (uint32_t bi = 0; bi < box_count; ++bi) {
                void* box = boxes[bi];
                if (!box) continue;
                int64_t box_cid = -1;
                if (!mem_read((const uint8_t*)box + kClassTagBox_CatId, &box_cid, sizeof(box_cid)) || box_cid != cid)
                    continue;
                char box_cls[48]{};
                mem_read_std_string((const uint8_t*)box + kClassTagBox_Class, box_cls, sizeof(box_cls));
                if (strcmp(box_cls, desired) == 0) {
                    already_box = true;
                    break;
                }
            }
            if (already_box) continue;

            if (reg && g.cat_by_id) {
                const auto* cat = static_cast<const uint8_t*>(g.cat_by_id(reg, cid));
                if (cat) {
                    char cur_cls[48]{};
                    mem_read_std_string(cat + kCatData_Class, cur_cls, sizeof(cur_cls));
                    if (strcmp(cur_cls, desired) == 0) continue;
                }
            }

            for (uint32_t bi = 0; bi < box_count; ++bi) {
                void* box = boxes[bi];
                if (!box) continue;
                int64_t box_cid = -1;
                if (!mem_read((const uint8_t*)box + kClassTagBox_CatId, &box_cid, sizeof(box_cid)) || box_cid != -1)
                    continue;
                char box_cls[48]{};
                mem_read_std_string((const uint8_t*)box + kClassTagBox_Class, box_cls, sizeof(box_cls));
                if (strcmp(box_cls, desired) == 0) {
                    focus_cat(cid, cid == prep.focused_cat_id);
                    g.class_tag_box_click(box);
                    log_line("HOUSE", "client ClassChooser equip '%s' on cat %lld",
                             desired, (long long)cid);
                    break;
                }
            }
        }
    }

    // Pass 3: mirror host's focused cat & carousel wrap index.
    if (prep.focused_cat_id != 0)
        focus_cat(prep.focused_cat_id, true);

    if (scene_current) *scene_current = prev_scene;
}

void client_sync_storage_items() {
    if (!g.is_client || !g.have_prep || g.last_prep.stage != (uint8_t)PrepStage::StorageItems)
        return;
    if (!g.inv_screen_lookup || !g.inv_screen_item_eq || !g.inv_item_box_click_apply ||
        !g.cat_selector_show_stats)
        return;

    const void* st_scene = nullptr;
    bool st_susp = false;
    if (!leave_find_scene("StorageItems", &st_scene, &st_susp) || !st_scene || st_susp)
        return;

    uint8_t destroying = 0;
    if (!mem_read((const uint8_t*)st_scene + kScene_Destroying, &destroying, 1) || destroying)
        return;

    void** screens = nullptr;
    uint32_t screen_count = 0;
    if (!get_cached_list(const_cast<void*>(st_scene), kCompId_InventoryScreen2, &screens, &screen_count) ||
        screen_count == 0 || !screens || !screens[0])
        return;
    void* screen = screens[0];

    void* selector = nullptr;
    if (!mem_read((const uint8_t*)screen + kInvScreen2_Selector, &selector, sizeof(selector)) || !selector)
        return;

    void** boxes = nullptr;
    uint32_t box_count = 0;
    get_cached_list(const_cast<void*>(st_scene), kCompId_InventoryItemBox, &boxes, &box_count);

    const void* reg = current_cat_registry();
    const auto& prep = g.last_prep;

    auto is_party_cat = [&](int64_t cid) -> bool {
        if (cid == 0) return false;
        for (uint8_t i = 0; i < prep.count; ++i)
            if (prep.cats[i].cat_id == cid) return true;
        return false;
    };

    auto** scene_current = reinterpret_cast<void**>(addr_of_data(D_SceneCurrent));
    void* prev_scene = scene_current ? *scene_current : nullptr;
    if (scene_current) *scene_current = const_cast<void*>(st_scene);

    auto focus_cat = [&](int64_t cid, bool apply_host_index) {
        if (!is_party_cat(cid)) return;
        if (apply_host_index)
            *(int32_t*)((uintptr_t)selector + kCatSelector_SelectedIndex) = prep.focused_index;
        int64_t sel_cur = 0;
        mem_read((const uint8_t*)selector + kCatSelector_CurrentCat, &sel_cur, sizeof(sel_cur));
        if (sel_cur != cid)
            g.cat_selector_show_stats(selector, cid);
        *(int64_t*)((uintptr_t)screen + kInvScreen2_CurrentCat) = cid;
    };

    // Pass 1: unequip any item whose cat no longer wears that item in that slot.
    if (boxes && box_count > 0 && reg && g.cat_by_id) {
        for (uint32_t bi = 0; bi < box_count; ++bi) {
            void* box = boxes[bi];
            if (!box) continue;
            int64_t uid = 0;
            if (!mem_read((const uint8_t*)box + kInvItemBox_ItemUid, &uid, sizeof(uid)) || uid == 0)
                continue;
            const int64_t eq_cat = g.inv_screen_item_eq(screen, uid);
            if (eq_cat <= 0) continue;

            const auto* cat = static_cast<const uint8_t*>(g.cat_by_id(reg, eq_cat));
            if (!cat) continue;

            int found_slot = -1;
            char cur_name[48]{};
            for (int s = 0; s < 5; ++s) {
                const uint8_t* slot_ptr = cat + kCatData_Equipment + s * kEquipment_Size;
                int64_t slot_uid = 0;
                if (mem_read(slot_ptr, &slot_uid, sizeof(slot_uid)) && slot_uid == uid) {
                    found_slot = s;
                    mem_read_std_string(slot_ptr + kEquipment_Name, cur_name, sizeof(cur_name));
                    break;
                }
            }
            if (found_slot < 0) continue;

            for (uint8_t ci = 0; ci < prep.count; ++ci) {
                if (prep.cats[ci].cat_id != eq_cat) continue;
                if (strcmp(cur_name, prep.cats[ci].eq_slots[found_slot]) != 0) {
                    focus_cat(eq_cat, eq_cat == prep.focused_cat_id);
                    void* box_ptr = box;
                    g.inv_item_box_click_apply(&box_ptr);
                    log_line("HOUSE", "client StorageItems unequip '%s' from cat %lld (slot %d)",
                             cur_name, (long long)eq_cat, found_slot);
                }
                break;
            }
        }
    }

    // Pass 2: equip missing items onto each cat's slots.
    if (boxes && box_count > 0 && reg && g.cat_by_id) {
        for (uint8_t ci = 0; ci < prep.count; ++ci) {
            const int64_t cid = prep.cats[ci].cat_id;
            if (cid == 0) continue;
            const auto* cat = static_cast<const uint8_t*>(g.cat_by_id(reg, cid));
            if (!cat) continue;

            for (int s = 0; s < 5; ++s) {
                const char* desired = prep.cats[ci].eq_slots[s];
                if (!desired[0]) continue;

                char cur_name[48]{};
                mem_read_std_string(cat + kCatData_Equipment + s * kEquipment_Size + kEquipment_Name,
                                    cur_name, sizeof(cur_name));
                if (strcmp(cur_name, desired) == 0) continue;

                for (uint32_t bi = 0; bi < box_count; ++bi) {
                    void* box = boxes[bi];
                    if (!box) continue;
                    int64_t uid = 0;
                    if (!mem_read((const uint8_t*)box + kInvItemBox_ItemUid, &uid, sizeof(uid)) || uid == 0)
                        continue;
                    if (g.inv_screen_item_eq(screen, uid) != -1) continue;
                    const auto* eq = static_cast<const uint8_t*>(g.inv_screen_lookup(screen, uid));
                    if (!eq) continue;
                    char eq_name[48]{};
                    mem_read_std_string(eq + kEquipment_Name, eq_name, sizeof(eq_name));
                    if (strcmp(eq_name, desired) == 0) {
                        focus_cat(cid, cid == prep.focused_cat_id);
                        void* box_ptr = box;
                        g.inv_item_box_click_apply(&box_ptr);
                        log_line("HOUSE", "client StorageItems equip '%s' on cat %lld (slot %d)",
                                 desired, (long long)cid, s);
                        break;
                    }
                }
            }
        }
    }

    // Pass 3: mirror host's focused cat & carousel wrap index.
    if (prep.focused_cat_id != 0)
        focus_cat(prep.focused_cat_id, true);

    if (scene_current) *scene_current = prev_scene;
}

void host_poll_prep_state() {
    if (g.phase != HostPhase::Prep || !g.director_slot) return;
    const void* director = nullptr;
    if (!mem_read(g.director_slot, &director, sizeof(director)) || !director) return;

    uint32_t count = 0;
    const int64_t* ids = nullptr;
    if (!mem_read((const uint8_t*)director + kDir_CatIdCount, &count, sizeof(count))) return;
    if (count > kMaxPrepCats) count = kMaxPrepCats;
    if (count > 0 && !mem_read((const uint8_t*)director + kDir_CatIdData, &ids, sizeof(ids)))
        return;

    const void* reg = nullptr;
    mem_read((const uint8_t*)director + kDir_CatRegistry, &reg, sizeof(reg));

    HousePrepMsg msg{};
    msg.generation   = g.generation;
    msg.active       = 1;
    msg.stage        = (uint8_t)g.prep_stage;
    msg.collar_count = g.last_prep.collar_count;
    memcpy(msg.map_name, g.last_prep.map_name, sizeof(msg.map_name));
    memcpy(msg.collars,  g.last_prep.collars,  sizeof(msg.collars));
    msg.count        = (uint8_t)count;
    for (uint8_t i = 0; i < msg.count; ++i) {
        int64_t cid = 0;
        if (ids) mem_read(ids + i, &cid, sizeof(cid));
        msg.cats[i].cat_id = cid;
        if (reg && cid != 0 && g.cat_by_id) {
            const auto* cat = static_cast<const uint8_t*>(g.cat_by_id(reg, cid));
            if (cat) {
                mem_read_std_string(cat + kCatData_Class, msg.cats[i].cat_class,
                                    sizeof(msg.cats[i].cat_class));
                read_prep_equipment(cat, msg.cats[i]);
            }
        }
    }

    if (g.prep_stage == PrepStage::ClassChooser) read_prep_class_tags(msg);
    else if (g.prep_stage == PrepStage::StorageItems) read_prep_storage_focus(msg);
    if (!g.have_prep || memcmp(&msg, &g.last_prep, sizeof(msg)) != 0) {
        g.last_prep = msg;
        g.have_prep = true;
        net_send_house_prep(msg);
    }
}

void apply_client_cat_records() {
    if (g.queued_cat_records.empty() || !g.cat_by_id || !g.current_house) return;
    const void* registry = current_cat_registry();
    if (!registry) return;
    for (auto it = g.queued_cat_records.begin(); it != g.queued_cat_records.end();) {
        auto found = g.cat_actors.find(it->first);
        if (found == g.cat_actors.end()) {
            ++it;
            continue;
        }
        void* cat = g.cat_by_id(registry, it->first);
        auto& bytes = it->second.bytes;
        const CatDataMsg record{ static_cast<uint64_t>(it->first),
                                 static_cast<uint32_t>(bytes.size()), it->second.hash,
                                 bytes.data(), g.client_gen };
        CatDataMsg local{};
        const bool unchanged = cat && catsync_pack_cat(cat, local) && local.hash == record.hash;
        free(local.data);
        if (!unchanged && cat && catsync_unpack_cat(cat, record)) {
            void* art = nullptr;
            if (g.regen_cat_parts && mem_read(static_cast<const uint8_t*>(found->second)
                                            + kHCat_CatArt, &art, sizeof(art)) && art)
                g.regen_cat_parts(art);
        } else if (!unchanged) {
            log_line("HOUSE", "!! could not apply live cat record %lld", (long long)it->first);
        }
        it = g.queued_cat_records.erase(it);
    }
}

void observe_pose_clock(uint32_t host_tick, uint32_t received_at) {
    const uint32_t offset = received_at - host_tick;
    if (!g.pose_clock_valid || static_cast<int32_t>(offset - g.pose_clock_offset) < 0) {
        g.pose_clock_offset = offset;
        g.pose_clock_valid = true;
    }
}

void client_interpolate_cats() {
    if (!g.is_client || g.phase != HostPhase::House || g.reload_stage || savefile_has_pending())
        return;
    const uint32_t now = now_ms();
    rebuild_actor_map();
    if (!live_house_scene_valid()) return;
    apply_client_cat_records();

    // Apply any queued baseline once House actors exist.
    if (g.has_queued_baseline && g.queued_baseline_gen == g.client_gen) {
        if (g.queued_returning) {
            g.playing_arrival = true;
            g.arrival_started_ms = now;
            g.arrival_host_tick = g.queued_baseline_tick;
        }
        std::vector<int64_t> keep_ids;
        keep_ids.reserve(g.queued_baseline_cats.size());
        for (const auto& p : g.queued_baseline_cats) {
            keep_ids.push_back(p.cat_id);
            auto ait = g.cat_actors.find(p.cat_id);
            if (ait != g.cat_actors.end())
                apply_pose_to_client_actor(ait->second, p, true);
            auto& tr = g.client_tracks[p.cat_id];
            tr.holder_peer = p.holder_peer;
            tr.capture_id  = p.capture_id;
            tr.last_applied = p;
            tr.has_last = true;
            if (tr.samples.empty() ||
                static_cast<int32_t>(tr.samples.front().host_tick_ms - g.queued_baseline_tick) > 0)
                tr.samples.insert(tr.samples.begin(), {g.queued_baseline_tick, p});
        }
        std::sort(keep_ids.begin(), keep_ids.end());
        for (auto it = g.cat_actors.begin(); it != g.cat_actors.end(); ) {
            if (!std::binary_search(keep_ids.begin(), keep_ids.end(), it->first)) {
                void* hc = it->second;
                if (hc) {
                    apply_client_room(hc, nullptr);
                    *(uint8_t*)((uintptr_t)hc + kHCat_Exists)   = 0;
                    *(uint8_t*)((uintptr_t)hc + kHCat_Departed) = 1;
                    if (g.comp_destroy) g.comp_destroy(hc);
                }
                g.client_tracks.erase(it->first);
                it = g.cat_actors.erase(it);
            } else {
                ++it;
            }
        }
        g.has_queued_baseline = false;
        g.queued_baseline_cats.clear();
    }

    const uint32_t live_target = now - g.pose_clock_offset - tune::kHouseInterpDelayMs;
    uint32_t target_ms = live_target;
    if (g.playing_arrival) {
        const uint32_t elapsed = now - g.arrival_started_ms;
        target_ms = g.arrival_host_tick + elapsed;
        // Catch up to the live clock after showing the beginning of the walk.
        if (elapsed >= 750 || static_cast<int32_t>(target_ms - live_target) >= 0) {
            g.playing_arrival = false;
            target_ms = live_target;
        }
    }

    for (auto& kv : g.cat_actors) {
        int64_t id = kv.first;
        void*   hc = kv.second;
        auto tit = g.client_tracks.find(id);
        if (tit == g.client_tracks.end()) continue;
        ClientCatTrack& tr = tit->second;

        // Local predicted/granted drag follows local mouse every frame.
        if (tr.predicted_held || (g.client_held_cat == id && g.client_held_cap_id != 0)) {
            float wx = tr.pred_wx, wy = tr.pred_wy;
            house_mouse_world(&wx, &wy);
            HouseCatPose p = tr.has_last ? tr.last_applied : HouseCatPose{};
            if (p.holder_peer == kNoPeer) p.depth += 100000;
            p.cat_id      = id;
            p.holder_peer = net_self();
            p.render_layer = 20; // native held-cat layer
            p.anim_state  = 8; // native held ragdoll
            const auto anchor = cat_drag_anchor(hc);
            p.x           = static_cast<float>(wx - anchor.x);
            p.y           = static_cast<float>(wy - anchor.y);
            p.z           = 0.0f;
            apply_pose_to_client_actor(hc, p, false);
            continue;
        }

        if (tr.samples.empty()) continue;

        const auto reached = [target_ms](uint32_t tick) {
            return static_cast<int32_t>(target_ms - tick) >= 0;
        };
        const InterpSample* s0 = &tr.samples.front();
        const InterpSample* s1 = &tr.samples.back();

        if (tr.samples.size() == 1 || !reached(s0->host_tick_ms)) {
            apply_pose_to_client_actor(hc, s0->pose, true);
            tr.last_applied = s0->pose;
            tr.has_last = true;
        } else if (reached(s1->host_tick_ms)) {
            uint32_t dt_ms = target_ms - s1->host_tick_ms;
            if (dt_ms > tune::kHouseMaxExtrapMs) dt_ms = tune::kHouseMaxExtrapMs;
            float dt = (float)dt_ms * 0.001f;
            HouseCatPose p = s1->pose;
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            apply_pose_to_client_actor(hc, p, true);
            tr.last_applied = p;
            tr.has_last = true;
        } else {
            for (size_t i = 0; i + 1 < tr.samples.size(); ++i) {
                if (reached(tr.samples[i].host_tick_ms) && !reached(tr.samples[i + 1].host_tick_ms)) {
                    s0 = &tr.samples[i];
                    s1 = &tr.samples[i + 1];
                    break;
                }
            }
            uint32_t span = s1->host_tick_ms - s0->host_tick_ms;
            float alpha = span > 0 ? (float)(target_ms - s0->host_tick_ms) / (float)span : 1.0f;
            HouseCatPose p = s1->pose;
            // Snap if large authoritative correction.
            float dx = s1->pose.x - s0->pose.x;
            float dy = s1->pose.y - s0->pose.y;
            if (dx * dx + dy * dy <= 9.0f) {
                p.x = s0->pose.x + dx * alpha;
                p.y = s0->pose.y + dy * alpha;
                p.z = s0->pose.z + (s1->pose.z - s0->pose.z) * alpha;
            }
            apply_pose_to_client_actor(hc, p, true);
            tr.last_applied = p;
            tr.has_last = true;
        }

        // Trim old samples beyond interpolation window.
        while (tr.samples.size() > 2 && reached(tr.samples[1].host_tick_ms))
            tr.samples.erase(tr.samples.begin());
    }
}

void send_cursor_sample() {
    if (!net_active()) return;
    if (g.phase != HostPhase::House && g.phase != HostPhase::Prep) return;
    uint32_t t = now_ms();
    if (t - g.last_cursor_ms < tune::kHouseCursorMs) return;
    g.last_cursor_ms = t;

    float nx = 0.5f, ny = 0.5f;
    uint8_t mode = 0;
    const bool have_ptr = overlay_local_pointer(nx, ny, mode);

    HouseCursorMsg m{};
    m.generation = g.is_client ? g.client_gen : g.generation;
    m.mode       = mode;

    if (g.phase == HostPhase::House && live_house_scene_valid()) {
        float wx = 0.0f, wy = 0.0f;
        if (!house_mouse_world(&wx, &wy)) return;
        project_world_to_norm(wx, wy, &nx, &ny);
        m.wx       = wx;
        m.wy       = wy;
        m.nx       = nx;
        m.ny       = ny;
        m.in_house = 1;
    } else {
        if (!have_ptr) return;
        m.nx       = nx;
        m.ny       = ny;
        m.in_house = 2; // Prep screen-space (ClassChooser / StorageItems / ActSelection)
    }
    net_send_house_cursor(m);
}

void check_peer_departures() {
    uint8_t cur[kMaxPeers] = {};
    if (!net_peer_ids(cur, kMaxPeers)) return;
    uint8_t cur_n = net_peer_count();
    for (uint8_t i = 0; i < g.known_peer_count; ++i) {
        uint8_t old_id = g.known_peers[i];
        bool still = false;
        for (uint8_t j = 0; j < cur_n; ++j)
            if (cur[j] == old_id) { still = true; break; }
        if (!still)
            house_on_peer_disconnect(old_id);
    }
    memcpy(g.known_peers, cur, sizeof(cur));
    g.known_peer_count = cur_n;
}

// ---------------------------------------------------------------------------
// InterstitialQueue binary serialization (for night/breeding animations)
// ---------------------------------------------------------------------------

void iq_w_u8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }
void iq_w_u32(std::vector<uint8_t>& out, uint32_t v) {
    const uint8_t* p = (const uint8_t*)&v;
    out.insert(out.end(), p, p + 4);
}
void iq_w_u64(std::vector<uint8_t>& out, uint64_t v) {
    const uint8_t* p = (const uint8_t*)&v;
    out.insert(out.end(), p, p + 8);
}
void iq_w_bytes(std::vector<uint8_t>& out, const void* data, uint32_t len) {
    iq_w_u32(out, len);
    if (len && data) {
        const uint8_t* p = (const uint8_t*)data;
        out.insert(out.end(), p, p + len);
    }
}
void iq_w_std_str(std::vector<uint8_t>& out, const void* std_str) {
    uint64_t len = 0, cap = 0;
    memcpy(&len, (const uint8_t*)std_str + 16, 8);
    memcpy(&cap, (const uint8_t*)std_str + 24, 8);
    const char* ptr = (cap > 15) ? *(const char* const*)std_str : (const char*)std_str;
    iq_w_bytes(out, ptr, (uint32_t)len);
}
void iq_w_std_wstr(std::vector<uint8_t>& out, const void* std_wstr) {
    uint64_t len = 0, cap = 0;
    memcpy(&len, (const uint8_t*)std_wstr + 16, 8);
    memcpy(&cap, (const uint8_t*)std_wstr + 24, 8);
    const wchar_t* ptr = (cap > 7) ? *(const wchar_t* const*)std_wstr : (const wchar_t*)std_wstr;
    iq_w_bytes(out, ptr, (uint32_t)(len * sizeof(wchar_t)));
}
bool iq_w_cat_vec(std::vector<uint8_t>& out, const void* vec) {
    if (!g.serialize_cat || !g.bs_dtor || !g.ofstream_ctor) return false;
    const uint8_t* first = nullptr;
    const uint8_t* last  = nullptr;
    memcpy(&first, (const uint8_t*)vec + 0, 8);
    memcpy(&last,  (const uint8_t*)vec + 8, 8);
    uint32_t count = (first && last >= first) ? (uint32_t)((last - first) / kCatData_Size) : 0;
    iq_w_u32(out, count);
    for (uint32_t i = 0; i < count; ++i) {
        void* cd = (void*)(first + (size_t)i * kCatData_Size);
        uint8_t bs[kByteStreamSize];
        memset(bs, 0, sizeof(bs));
        g.ofstream_ctor(bs + kBS_Ofstream);
        uint32_t mode = 1, swap = 8;
        memcpy(bs + kBS_Mode, &mode, 4);
        memcpy(bs + kBS_SwapElem, &swap, 4);
        g.serialize_cat(cd, bs, true);
        uint32_t wlen = 0;
        const void* wbuf = nullptr;
        memcpy(&wlen, bs + kBS_WriteLen, 4);
        memcpy(&wbuf, bs + kBS_WriteBuf, 8);
        iq_w_bytes(out, wbuf, wlen);
        g.bs_dtor(bs);
    }
    return true;
}

bool serialize_interstitial_queue(const void* iq, std::vector<uint8_t>& out) {
    out.clear();
    if (!iq) return false;
    const uint8_t* q = (const uint8_t*)iq;
    iq_w_u8(out, q[kIQ_Flag0]);
    iq_w_u8(out, q[kIQ_Flag1]);
    uint32_t st = 0;
    memcpy(&st, q + kIQ_State, 4);
    iq_w_u32(out, st);

    auto write_str_vec = [&](const uint8_t* vec) {
        const uint8_t* first = nullptr;
        const uint8_t* last  = nullptr;
        memcpy(&first, vec + 0, 8);
        memcpy(&last,  vec + 8, 8);
        uint32_t n = (first && last >= first) ? (uint32_t)((last - first) / 32) : 0;
        iq_w_u32(out, n);
        for (uint32_t i = 0; i < n; ++i)
            iq_w_std_str(out, first + (size_t)i * 32);
    };
    write_str_vec(q + kIQ_NightCutscenes);
    write_str_vec(q + kIQ_DayCutscenes);

    const uint8_t* ifirst = nullptr;
    const uint8_t* ilast  = nullptr;
    memcpy(&ifirst, q + kIQ_Items + 0, 8);
    memcpy(&ilast,  q + kIQ_Items + 8, 8);
    uint32_t icount = (ifirst && ilast >= ifirst) ? (uint32_t)((ilast - ifirst) / kInterstitial_Size) : 0;
    iq_w_u32(out, icount);
    for (uint32_t i = 0; i < icount; ++i) {
        const uint8_t* it = ifirst + (size_t)i * kInterstitial_Size;
        iq_w_std_str(out,  it + kInterstitial_Str0);
        iq_w_std_str(out,  it + kInterstitial_Str1);
        iq_w_std_str(out,  it + kInterstitial_Str2);
        iq_w_std_wstr(out, it + kInterstitial_WStr0);
        iq_w_std_wstr(out, it + kInterstitial_WStr1);
        iq_w_u8(out, it[kInterstitial_Flag0]);
        iq_w_u8(out, it[kInterstitial_Flag1]);
        uint32_t pod_n = 0;
        const int64_t* pod_p = nullptr;
        memcpy(&pod_n, it + kInterstitial_Pod + 4, 4);
        memcpy(&pod_p, it + kInterstitial_Pod + 8, 8);
        iq_w_u32(out, pod_n);
        for (uint32_t j = 0; j < pod_n; ++j)
            iq_w_u64(out, (uint64_t)(pod_p ? pod_p[j] : 0));
        if (!iq_w_cat_vec(out, it + kInterstitial_Cats0)) return false;
        if (!iq_w_cat_vec(out, it + kInterstitial_Cats1)) return false;
        uint64_t tail = 0;
        memcpy(&tail, it + kInterstitial_TailU64, 8);
        iq_w_u64(out, tail);
    }
    return true;
}

struct IqReader {
    const uint8_t* p = nullptr;
    uint32_t       len = 0;
    uint32_t       pos = 0;
    bool           ok  = true;

    uint8_t u8() {
        if (pos + 1 > len) { ok = false; return 0; }
        return p[pos++];
    }
    uint32_t u32() {
        if (pos + 4 > len) { ok = false; return 0; }
        uint32_t v = 0; memcpy(&v, p + pos, 4); pos += 4; return v;
    }
    uint64_t u64() {
        if (pos + 8 > len) { ok = false; return 0; }
        uint64_t v = 0; memcpy(&v, p + pos, 8); pos += 8; return v;
    }
    bool bytes(const uint8_t*& out_ptr, uint32_t& out_len) {
        out_len = u32();
        if (!ok || pos + out_len > len) { ok = false; return false; }
        out_ptr = p + pos;
        pos += out_len;
        return true;
    }
};

bool deserialize_and_run_interstitials(const uint8_t* data, uint32_t size,
                                       const void* house_scene) {
    auto* scene_current = (void**)addr_of_data(D_SceneCurrent);
    if (!data || !size || !house_scene || !scene_current ||
        !g.do_interstitials || !g.inter_ctor || !g.inter_dtor ||
        !g.vec_inter_empl || !g.vec_str_push || !g.str_assign || !g.wstr_assign ||
        !g.podbuf_resize || !g.cat_ctor || !g.cat_dtor || !g.vec_cat_push ||
        !g.serialize_cat || !g.bs_dtor || !g.ofstream_ctor || !g.str_dtor) {
        log_line("HOUSE", "!! cannot start client interstitials (missing scene/resolver)");
        return false;
    }

    IqReader r{ data, size, 0, true };
    uint8_t iq[kInterstitialQueue_Size];
    memset(iq, 0, sizeof(iq));

    iq[kIQ_Flag0] = r.u8();
    iq[kIQ_Flag1] = r.u8();
    uint32_t st   = r.u32();
    memcpy(iq + kIQ_State, &st, 4);

    auto read_str_vec = [&](uint8_t* vec) -> bool {
        uint32_t n = r.u32();
        if (!r.ok || n > 4096) return false;
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* sp = nullptr; uint32_t sl = 0;
            if (!r.bytes(sp, sl)) return false;
            uint8_t gs[32];
            memset(gs, 0, sizeof(gs));
            uint64_t sso = 15;
            memcpy(gs + 24, &sso, 8);
            g.str_assign(gs, (const char*)sp, sl);
            g.vec_str_push(vec, gs);
            g.str_dtor(gs);
        }
        return true;
    };
    if (!read_str_vec(iq + kIQ_NightCutscenes)) return false;
    if (!read_str_vec(iq + kIQ_DayCutscenes))   return false;

    uint32_t icount = r.u32();
    if (!r.ok || icount > 512) return false;

    auto read_cat_vec = [&](uint8_t* vec) -> bool {
        uint32_t cn = r.u32();
        if (!r.ok || cn > 128) return false;
        for (uint32_t j = 0; j < cn; ++j) {
            const uint8_t* cp = nullptr; uint32_t cl = 0;
            if (!r.bytes(cp, cl)) return false;
            uint8_t cd[kCatData_Size];
            g.cat_ctor(cd);
            uint8_t bs[kByteStreamSize];
            memset(bs, 0, sizeof(bs));
            g.ofstream_ctor(bs + kBS_Ofstream);
            uint32_t mode = 0, swap = 8;
            memcpy(bs + kBS_Mode,     &mode, 4);
            memcpy(bs + kBS_ReadBuf,  &cp,   8);
            memcpy(bs + kBS_ReadLen,  &cl,   4);
            bs[kBS_ReadOwns] = 0;
            memcpy(bs + kBS_SwapElem, &swap, 4);
            g.serialize_cat(cd, bs, false);
            g.bs_dtor(bs);
            g.vec_cat_push(vec, cd);
            g.cat_dtor(cd);
        }
        return true;
    };

    for (uint32_t i = 0; i < icount; ++i) {
        uint8_t item[kInterstitial_Size];
        g.inter_ctor(item);
        const uint8_t* sp = nullptr; uint32_t sl = 0;
        if (!r.bytes(sp, sl)) { g.inter_dtor(item); return false; }
        g.str_assign(item + kInterstitial_Str0, (const char*)sp, sl);
        if (!r.bytes(sp, sl)) { g.inter_dtor(item); return false; }
        g.str_assign(item + kInterstitial_Str1, (const char*)sp, sl);
        if (!r.bytes(sp, sl)) { g.inter_dtor(item); return false; }
        g.str_assign(item + kInterstitial_Str2, (const char*)sp, sl);
        if (!r.bytes(sp, sl)) { g.inter_dtor(item); return false; }
        g.wstr_assign(item + kInterstitial_WStr0, (const wchar_t*)sp, sl / sizeof(wchar_t));
        if (!r.bytes(sp, sl)) { g.inter_dtor(item); return false; }
        g.wstr_assign(item + kInterstitial_WStr1, (const wchar_t*)sp, sl / sizeof(wchar_t));
        item[kInterstitial_Flag0] = r.u8();
        item[kInterstitial_Flag1] = r.u8();
        uint32_t pod_n = r.u32();
        if (!r.ok || pod_n > 1024) { g.inter_dtor(item); return false; }
        if (pod_n > 0) {
            g.podbuf_resize(item + kInterstitial_Pod, pod_n);
            memcpy(item + kInterstitial_Pod + 4, &pod_n, 4);
            int64_t* pod_p = nullptr;
            memcpy(&pod_p, item + kInterstitial_Pod + 8, 8);
            for (uint32_t j = 0; j < pod_n; ++j) {
                uint64_t v = r.u64();
                if (pod_p) pod_p[j] = (int64_t)v;
            }
        }
        if (!read_cat_vec(item + kInterstitial_Cats0) ||
            !read_cat_vec(item + kInterstitial_Cats1)) {
            g.inter_dtor(item);
            return false;
        }
        uint64_t tail = r.u64();
        memcpy(item + kInterstitial_TailU64, &tail, 8);
        if (!r.ok) { g.inter_dtor(item); return false; }

        void* last = nullptr;
        memcpy(&last, iq + kIQ_Items + 8, 8);
        g.vec_inter_empl(iq + kIQ_Items, last, item);
        g.inter_dtor(item);
    }

    // Unsuspend House scene once all interstitials finish.
    new (iq + kIQ_Callback) std::function<void()>([]() {
        const void* hs = nullptr;
        bool susp = false;
        if (leave_find_scene(kScene_House, &hs, &susp) && hs)
            *(uint8_t*)((uintptr_t)hs + kScene_Suspended) = 0;
    });

    log_line("HOUSE", "starting %u night/breeding interstitial(s) on client", icount);
    void* prev_scene = *scene_current;
    *scene_current = const_cast<void*>(house_scene);
    g.do_interstitials(iq);
    *scene_current = prev_scene;
    return true;
}

struct FurnitureRec {
    int64_t uid   = 0;
    int32_t x     = 0;
    int32_t y     = 0;
    int32_t fx    = 1;
    int32_t fy    = 1;
    int32_t depth = 0;
    uint8_t room_len = 0;
    char    room[64] = {};
};

void* current_house_inventory() {
    auto* slot = (void**)addr_of_data(D_HouseInventoryPtr);
    if (!slot) return nullptr;
    void* inv = nullptr;
    if (!mem_read(slot, &inv, sizeof(inv))) return nullptr;
    return inv;
}

bool serialize_furniture_layout(std::vector<uint8_t>& out) {
    out.clear();
    void* hinv = current_house_inventory();
    if (!hinv) return false;

    uint32_t count = 0;
    void** entries = nullptr;
    if (!mem_read((const uint8_t*)hinv + kHInv_FurnitureCount, &count, sizeof(count))) return false;
    if (count > 2048) return false;
    if (count > 0 && !mem_read((const uint8_t*)hinv + kHInv_FurnitureData, &entries, sizeof(entries)))
        return false;
    if (count > 0 && !entries) return false;

    auto push_bytes = [&](const void* p, size_t n) {
        const auto* b = (const uint8_t*)p;
        out.insert(out.end(), b, b + n);
    };

    uint16_t n16 = (uint16_t)count;
    push_bytes(&n16, sizeof(n16));
    uint16_t written = 0;
    for (uint32_t i = 0; i < count; ++i) {
        void* e = nullptr;
        if (!mem_read(entries + i, &e, sizeof(e)) || !e) continue;
        int64_t uid = 0;
        int32_t pos[2] = {};
        int32_t flip[2] = { 1, 1 };
        int32_t depth = 0;
        char room[64] = {};
        if (!mem_read((const uint8_t*)e + kFPE_Uid, &uid, sizeof(uid))) continue;
        mem_read((const uint8_t*)e + kFPE_PlacedPos, pos, sizeof(pos));
        mem_read((const uint8_t*)e + kFPE_PlacedFlip, flip, sizeof(flip));
        mem_read((const uint8_t*)e + kFPE_PlacedDepth, &depth, sizeof(depth));
        mem_read_std_string((const uint8_t*)e + kFPE_PlacedRoom, room, sizeof(room));
        uint8_t rlen = (uint8_t)strnlen(room, sizeof(room) - 1);

        push_bytes(&uid, sizeof(uid));
        push_bytes(pos, sizeof(pos));
        push_bytes(flip, sizeof(flip));
        push_bytes(&depth, sizeof(depth));
        push_bytes(&rlen, 1);
        if (rlen) push_bytes(room, rlen);
        ++written;
    }
    memcpy(out.data(), &written, sizeof(written));
    return true;
}

bool apply_furniture_layout(const uint8_t* data, uint32_t size, void* house_scene, void* house) {
    if (!data || size < 2 || !house_scene || !house) return false;
    if (!g.furn_remove || !g.create_furn_piece || !g.comp_destroy || !g.str_assign) return false;
    void* hinv = current_house_inventory();
    if (!hinv) return false;

    size_t off = 0;
    auto take = [&](void* dst, size_t n) -> bool {
        if (off + n > size) return false;
        memcpy(dst, data + off, n);
        off += n;
        return true;
    };

    uint16_t rec_count = 0;
    if (!take(&rec_count, sizeof(rec_count)) || rec_count > 2048) return false;

    std::vector<FurnitureRec> recs;
    recs.reserve(rec_count);
    for (uint16_t i = 0; i < rec_count; ++i) {
        FurnitureRec r{};
        if (!take(&r.uid, sizeof(r.uid))) return false;
        if (!take(&r.x, sizeof(r.x)) || !take(&r.y, sizeof(r.y))) return false;
        if (!take(&r.fx, sizeof(r.fx)) || !take(&r.fy, sizeof(r.fy))) return false;
        if (!take(&r.depth, sizeof(r.depth))) return false;
        if (!take(&r.room_len, 1)) return false;
        if (r.room_len >= sizeof(r.room)) return false;
        if (r.room_len && !take(r.room, r.room_len)) return false;
        r.room[r.room_len] = '\0';
        recs.push_back(r);
    }

    // Index local HouseInventory entries by uid so unknown uids are ignored.
    uint32_t local_count = 0;
    void** local_entries = nullptr;
    if (!mem_read((const uint8_t*)hinv + kHInv_FurnitureCount, &local_count, sizeof(local_count)) ||
        local_count > 2048)
        return false;
    if (local_count > 0 &&
        (!mem_read((const uint8_t*)hinv + kHInv_FurnitureData, &local_entries, sizeof(local_entries)) ||
         !local_entries))
        return false;

    std::unordered_map<int64_t, void*> by_uid;
    by_uid.reserve(local_count);
    for (uint32_t i = 0; i < local_count; ++i) {
        void* e = nullptr;
        int64_t uid = 0;
        if (mem_read(local_entries + i, &e, sizeof(e)) && e &&
            mem_read((const uint8_t*)e + kFPE_Uid, &uid, sizeof(uid)))
            by_uid[uid] = e;
    }

    auto* scene_current = (void**)addr_of_data(D_SceneCurrent);
    void* prev_scene = scene_current ? *scene_current : nullptr;
    if (scene_current) *scene_current = house_scene;

    // Remove and destroy currently placed FurniturePiece components.
    void** pieces = nullptr;
    uint32_t piece_count = 0;
    if (get_cached_list(house_scene, kCompId_FurniturePiece, &pieces, &piece_count) && pieces && piece_count > 0) {
        std::vector<void*> to_remove;
        to_remove.reserve(piece_count);
        for (uint32_t i = 0; i < piece_count; ++i) {
            void* p = pieces[i];
            if (!p) continue;
            void* entry = nullptr;
            void* grid  = nullptr;
            if (mem_read((const uint8_t*)p + kFPiece_Entry, &entry, sizeof(entry)) && entry &&
                mem_read((const uint8_t*)p + kFPiece_PlacedGrid, &grid, sizeof(grid)) && grid)
                to_remove.push_back(p);
        }
        for (void* p : to_remove) {
            g.furn_remove(p);
            g.comp_destroy(p);
        }
    }

    // Apply incoming layout to matching HouseInventory entries and re-spawn placed pieces.
    uint32_t applied = 0, spawned = 0;
    for (const auto& r : recs) {
        auto it = by_uid.find(r.uid);
        if (it == by_uid.end()) continue;
        uint8_t* entry = (uint8_t*)it->second;
        g.str_assign(entry + kFPE_PlacedRoom, r.room, r.room_len);
        int32_t pos[2]  = { r.x, r.y };
        int32_t flip[2] = { r.fx, r.fy };
        memcpy(entry + kFPE_PlacedPos,   pos,      sizeof(pos));
        memcpy(entry + kFPE_PlacedFlip,  flip,     sizeof(flip));
        memcpy(entry + kFPE_PlacedDepth, &r.depth, sizeof(r.depth));
        ++applied;
        if (r.room_len > 0) {
            int64_t uid_arg = r.uid;
            g.create_furn_piece(house, &uid_arg);
            ++spawned;
        }
    }

    if (scene_current) *scene_current = prev_scene;
    log_line("HOUSE", "applied furniture layout: %u/%u entries updated, %u placed",
             applied, (unsigned)recs.size(), spawned);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void house_init() {
    g.on        = tune::kHouseCoop;
    g.is_client = (net_role() == NetRole::Client);
    g.phase     = HostPhase::Outside;
    g.generation = 0;
    g.client_gen = 0;
    g.roster_rev = 1;
    g.waiting_first_node = false;
    g.need_adv_publish = false;
    g.need_day_publish = false;
    g.need_ret_publish = false;
    g.need_prep_publish = false;
    g.need_baseline = false;
    g.in_pass_day = false;
    g.in_return = false;
    g.pending_interstitial_blob.clear();
    g.has_pending_interstitial_blob = false;
    g.reload_stage = 0;
    g.cached_director = nullptr;
    memset(g.peer_needs_checkpoint, 0, sizeof(g.peer_needs_checkpoint));
    memset(g.peer_map_deadline, 0, sizeof(g.peer_map_deadline));
    clear_generation_state();
    memset(g.peer_map_ready, 0, sizeof(g.peer_map_ready));
    g.known_peer_count = 0;
    if (g.on)
        log_line("HOUSE", "armed (%s)", g.is_client ? "client" : "host");
}

void house_shutdown() {
    clear_generation_state();
    g.on    = false;
    g.phase = HostPhase::Outside;
}

void house_set_base(uintptr_t base) {
    g.base            = base;
    g.director_slot   = (const void**)addr_of_data(D_MewDirectorPtr);
    g.app_base_slot   = (const void**)addr_of_data(D_ApplicationBase);
    g.screen_bounds   = (const float*)(base + kRva_ScreenBounds);
    g.update_cached   = (FnSceneUpdateCachedList)addr_of_call(C_SceneUpdateCachedList);
    g.loc_under_point = (FnHouseLocUnderPoint)addr_of_call(C_HouseLocationUnderPoint);
    g.place_cat       = (FnPlaceCat)addr_of_call(C_CatPlacementPlaceCat);
    g.remove_cat      = (FnRemoveCat)addr_of_call(C_CatPlacementRemoveCat);
    g.cam_get_mouse   = (FnCameraGetMouse)addr_of_call(C_CameraGetMouse);
    g.cam_matrix      = (FnCameraMatrix)addr_of_call(C_CameraMatrix);
    g.str_alloc       = (FnGameStrAlloc)addr_of_call(C_GameStrAlloc);
    g.str_dtor        = (FnGameStrDtor)addr_of_call(C_GameStrDtor);
    g.comp_destroy    = (FnComponentDestroy)addr_of_call(C_ComponentDestroy);
    g.create_entity   = (FnSceneCreateEntity)addr_of_call(C_SceneCreateEntity);
    g.create_mew_dir  = (FnSceneCreateMewDir)addr_of_call(C_SceneCreateMewDirector);
    g.continue_save   = (FnMewDirContinueSave)addr_of_call(C_MewDirectorContinueSave);
    g.serialize_cat   = (FnSerializeCat)addr_of_call(C_SerializeCatData);
    g.cat_by_id       = (FnCatById)addr_of_call(C_CatDataById);
    g.regen_cat_parts = (FnVoidPtr)addr_of_call(C_CatPartsRegen);
    g.animator_set_state = (FnAnimatorSetState)addr_of_call(C_AnimatorSetState);
    g.bs_dtor         = (FnBsDtor)addr_of_call(C_ByteStreamDtor);
    g.ofstream_ctor   = (FnOfstreamCtor)addr_of_call(C_OfstreamCtor);
    g.cat_ctor        = (FnVoidPtr)addr_of_call(C_CatDataCtor);
    g.cat_dtor        = (FnVoidPtr)addr_of_call(C_CatDataDtor);
    g.vec_cat_push    = (FnVecPushBack)addr_of_call(C_VecCatDataPushBack);
    g.vec_str_push    = (FnVecPushBack)addr_of_call(C_VecStrPushBack);
    g.str_assign      = (FnStrAssign)addr_of_call(C_StrAssign);
    g.wstr_assign     = (FnWStrAssign)addr_of_call(C_WStrAssign);
    g.podbuf_resize   = (FnPodBufResize)addr_of_call(C_PodBuf8Resize);
    g.inter_ctor      = (FnVoidPtr)addr_of_call(C_InterstitialCtor);
    g.inter_dtor      = (FnVoidPtr)addr_of_call(C_InterstitialDtor);
    g.vec_inter_empl  = (FnVecInterEmplace)addr_of_call(C_VecInterstitialEmplace);
    g.dir_add_scene   = (FnDirectorAddScene)addr_of_call(C_DirectorAddScene);
    g.create_act_sel  = (FnCreateActSel)addr_of_call(C_CreateActSelection);
    g.furn_remove     = (FnVoidPtr)addr_of_call(C_FurniturePieceRemove);
    g.create_furn_piece = (FnCreateFurnPiece)addr_of_call(C_CreateFurniturePiece);
    g.class_tag_box_click = (FnVoidPtr)addr_of_call(C_ClassTagBoxClick);
    g.cat_selector_show_stats = (FnCatSelectorShowStats)addr_of_call(C_CatSelectorShowStats);
    g.inv_screen_lookup = (FnInvScreen2Lookup)addr_of_call(C_InvScreen2Lookup);
    g.inv_screen_item_eq = (FnInvScreen2ItemEq)addr_of_call(C_InvScreen2ItemEquipped);
    g.inv_item_box_click_apply = (FnInvItemBoxClickApply)addr_of_call(C_InvItemBoxClickApply);
}

void house_pump() {
    if (!g.on) return;
    check_peer_departures();

    if (!g.is_client) {
        // Detect House or Prep scene liveness when host starts session already in it.
        const void* hscene = nullptr;
        bool suspended = false;
        bool has_house = leave_find_scene(kScene_House, &hscene, &suspended);
        if (has_house && !suspended && !g.in_pass_day && g.phase == HostPhase::Outside) {
            g.phase = HostPhase::House;
            g.prep_stage = PrepStage::None;
            if (g.generation == 0) g.generation = 1;
            rebuild_actor_map();
            log_line("HOUSE", "host detected live House scene (gen=%u, cats=%zu)",
                     g.generation, g.cat_actors.size());
            send_baseline(kNoPeer, true);
        }
        if (g.phase == HostPhase::Outside) {
            const void* psc = nullptr;
            bool psusp = false;
            PrepStage detected = PrepStage::None;
            if (leave_find_scene("ActSelection", &psc, &psusp) && psc)
                detected = PrepStage::ActSelection;
            else if (leave_find_scene("StorageItems", &psc, &psusp) && psc)
                detected = PrepStage::StorageItems;
            else if (leave_find_scene("ClassChooser", &psc, &psusp) && psc)
                detected = PrepStage::ClassChooser;
            if (detected != PrepStage::None) {
                g.phase = HostPhase::Prep;
                g.prep_stage = detected;
                if (g.generation == 0) g.generation = 1;
                log_line("HOUSE", "host detected live Prep scene stage=%u (gen=%u)",
                         (unsigned)detected, g.generation);
                HousePhaseMsg pm{ g.generation, (uint8_t)HostPhase::Prep, 0 };
                net_send_house_phase(pm);
                host_poll_prep_state();
            }
        }
        if (g.phase == HostPhase::Prep && g.prep_stage != PrepStage::ActSelection) {
            const void* asc = nullptr;
            bool asusp = false;
            if (leave_find_scene("ActSelection", &asc, &asusp) && asc) {
                g.prep_stage = PrepStage::ActSelection;
                host_poll_prep_state();
                log_line("HOUSE", "host entered ActSelection (gen=%u)", g.generation);
            }
        }
        if (g.need_baseline && has_house && !suspended && !g.in_pass_day &&
            !g.need_day_publish && !g.need_ret_publish && !g.in_return) {
            g.need_baseline = false;
            send_baseline(kNoPeer, true);
        }

        // Deferred checkpoint publishes once SQLite transaction commits.
        // For PassDay, EndSave has already committed before DoInterstitialsInternal,
        // so we can publish immediately (even while House is suspended for breeding).
        if (g.need_day_publish && has_house && !g.in_pass_day) {
            if (savefile_publish_active(kNoPeer, /*fresh=*/true, /*flush_adventure=*/false)) {
                g.need_day_publish = false;
                send_baseline(kNoPeer, true);
                if (g.has_pending_interstitial_blob) {
                    HouseInterstitialsMsg im{};
                    im.generation = g.generation;
                    im.size       = (uint32_t)g.pending_interstitial_blob.size();
                    im.data       = g.pending_interstitial_blob.data();
                    net_send_house_interstitials(im);
                    g.has_pending_interstitial_blob = false;
                    g.pending_interstitial_blob.clear();
                }
            }
        }
        if (g.need_ret_publish && has_house && !suspended) {
            if (savefile_publish_active(kNoPeer, /*fresh=*/true, /*flush_adventure=*/false)) {
                g.need_ret_publish = false;
                send_baseline(kNoPeer, true, true);
                g.returning_first_poses.clear();
                g.return_first_tick = 0;
            }
        }
        if (g.need_roster_publish && g.phase == HostPhase::House && has_house && !suspended &&
            savefile_publish_active(kNoPeer, /*fresh=*/true, /*flush_adventure=*/false)) {
            g.need_roster_publish = false;
            send_baseline(kNoPeer, true);
        }
        if (g.need_prep_publish) {
            if (savefile_publish_active(kNoPeer, /*fresh=*/true, /*flush_adventure=*/false)) {
                g.need_prep_publish = false;
                host_poll_prep_state();
            }
        }
        bool map_suspended = false;
        if (g.need_adv_publish && follow_on_map() && follow_node_count() > 0 &&
            leave_find_scene("Map", nullptr, &map_suspended) && !map_suspended) {
            if (savefile_publish_active(kNoPeer, /*fresh=*/true, /*flush_adventure=*/false)) {
                g.need_adv_publish = false;
                HousePhaseMsg pm{ g.generation, (uint8_t)HostPhase::Adventure, 0 };
                net_send_house_phase(pm);
                uint8_t ids[kMaxPeers]{};
                if (net_peer_ids(ids, kMaxPeers)) {
                    const uint64_t deadline = GetTickCount64() + tune::kHouseMapReadyMs;
                    for (uint8_t i = 0; i < net_peer_count(); ++i) {
                        const uint8_t peer = ids[i];
                        if (peer == kHostPeer || peer >= kMaxPeers) continue;
                        g.peer_needs_checkpoint[peer] = false;
                        g.peer_map_deadline[peer] = deadline;
                    }
                }
            }
        }
        if (!g.need_adv_publish) {
            for (uint8_t peer = 1; peer < kMaxPeers; ++peer) {
                if (g.peer_needs_checkpoint[peer]) house_catchup(peer);
                if (g.waiting_first_node && !g.peer_map_ready[peer] &&
                    g.peer_map_deadline[peer] && GetTickCount64() >= g.peer_map_deadline[peer]) {
                    g.peer_map_deadline[peer] = 0;
                    log_line("HOUSE", "peer %u timed out loading the map; it can reconnect", peer);
                    net_disconnect_peer(peer);
                }
            }
        }

        host_sample_and_send_poses();
        host_poll_prep_state();
        send_cursor_sample();
    } else {
        // Client: lock all interactive buttons/item boxes in prep scenes so only
        // the host selects collars, items, and acts.
        static const char* const kPrepScenes[] = { "ClassChooser", "StorageItems", "ActSelection" };
        for (const char* sname : kPrepScenes) {
            const void* psc = nullptr;
            bool psusp = false;
            if (leave_find_scene(sname, &psc, &psusp) && psc)
                *(uint8_t*)((uintptr_t)psc + kScene_ControlsPrevented) = 1;
        }

        // Client: enter ClassChooser when host clicks GoOnAnAdventure in House.
        if (g.has_pending_class_chooser && g.pending_class_chooser_gen == g.client_gen &&
            g.reload_stage == 0 && !savefile_has_pending()) {
            const void* ss = nullptr;
            bool ss_susp = false;
            const void* hs = nullptr;
            bool hs_susp = false;
            const void* cc = nullptr;
            bool cc_susp = false;
            if (!leave_find_scene(kScene_SaveSelect, &ss, &ss_susp) &&
                leave_find_scene(kScene_House, &hs, &hs_susp) && hs &&
                !leave_find_scene("ClassChooser", &cc, &cc_susp)) {
                void* client_md = nullptr;
                if (g.director_slot)
                    mem_read(g.director_slot, &client_md, sizeof(client_md));
                if (client_md && g.go_on_adv && g.str_assign && g.str_dtor &&
                    g.vec_str_push && g.podbuf_resize) {
                    const auto& pm = g.pending_class_chooser;
                    uint8_t map_s[32];
                    memset(map_s, 0, sizeof(map_s));
                    uint64_t sso = 15;
                    memcpy(map_s + 24, &sso, 8);
                    g.str_assign(map_s, pm.map_name, strlen(pm.map_name));

                    uint8_t collars_vec[24];
                    memset(collars_vec, 0, sizeof(collars_vec));
                    for (uint8_t i = 0; i < pm.collar_count; ++i) {
                        uint8_t cs[32];
                        memset(cs, 0, sizeof(cs));
                        memcpy(cs + 24, &sso, 8);
                        g.str_assign(cs, pm.collars[i], strlen(pm.collars[i]));
                        g.vec_str_push(collars_vec, cs);
                        g.str_dtor(cs);
                    }

                    uint8_t cat_ids[16];
                    memset(cat_ids, 0, sizeof(cat_ids));
                    if (pm.count > 0) {
                        g.podbuf_resize(cat_ids, pm.count);
                        uint32_t cnt = pm.count;
                        memcpy(cat_ids + 4, &cnt, 4);
                        int64_t* ptr = nullptr;
                        memcpy(&ptr, cat_ids + 8, 8);
                        if (ptr) {
                            for (uint8_t i = 0; i < pm.count; ++i)
                                ptr[i] = pm.cats[i].cat_id;
                        }
                    }

                    log_line("HOUSE", "client entering ClassChooser (map=%s, cats=%u, collars=%u)",
                             pm.map_name, (unsigned)pm.count, (unsigned)pm.collar_count);
                    g.go_on_adv(client_md, map_s, collars_vec, cat_ids);
                    g.has_pending_class_chooser = false;
                    g.entered_class_chooser_gen = g.client_gen;
                }
            }
        }

        // Client: mirror host's collar/item assignments & cat carousel focus in prep screens.
        client_sync_class_chooser();
        client_sync_storage_items();

        // Client: transition StorageItems -> ActSelection when host opens ActSelection.
        if (g.phase == HostPhase::Prep && g.have_prep &&
            g.last_prep.stage == (uint8_t)PrepStage::ActSelection &&
            g.reload_stage == 0 && !savefile_has_pending()) {
            const void* st = nullptr;
            bool st_susp = false;
            const void* act = nullptr;
            bool act_susp = false;
            if (leave_find_scene("StorageItems", &st, &st_susp) && st &&
                !leave_find_scene("ActSelection", &act, &act_susp)) {
                void* client_md = nullptr;
                if (g.director_slot)
                    mem_read(g.director_slot, &client_md, sizeof(client_md));
                void* dir = nullptr;
                if (client_md)
                    mem_read((const uint8_t*)client_md + kDir_SceneDirector, &dir, sizeof(dir));
                if (dir && g.dir_add_scene && g.create_entity && g.create_act_sel) {
                    *(uint8_t*)((uintptr_t)st + kScene_Destroying) = 1;
                    uint8_t s_act[32];
                    memset(s_act, 0, sizeof(s_act));
                    const char* kAct = "ActSelection";
                    uint64_t len = 12, cap = 15;
                    memcpy(s_act, kAct, len + 1);
                    memcpy(s_act + 16, &len, 8);
                    memcpy(s_act + 24, &cap, 8);
                    void* asc = g.dir_add_scene(dir, s_act);
                    if (asc && !*(uint8_t*)((uintptr_t)asc + kScene_DoingDestruction)) {
                        void* ent = g.create_entity(asc);
                        g.create_act_sel(asc, ent);
                        *(uint8_t*)((uintptr_t)asc + kScene_ControlsPrevented) = 1;
                    }
                    log_line("HOUSE", "client entered ActSelection");
                }
            }
        }

        // Client: reload the mirror in-place when a fresh House/Prep/Adventure
        // save arrives while in House or any Prep scene.
        if (g.reload_stage == 0 && savefile_has_pending()) {
            const void* ss = nullptr;
            bool ss_susp = false;
            if (!leave_find_scene(kScene_SaveSelect, &ss, &ss_susp)) {
                bool in_reloadable = false;
                for (const char* sname : HouseState::reload_scenes) {
                    if (g.phase == HostPhase::Adventure && strcmp(sname, "Map") == 0)
                        continue;
                    const void* sc = nullptr;
                    bool susp = false;
                    if (leave_find_scene(sname, &sc, &susp) && sc) {
                        in_reloadable = true;
                        break;
                    }
                }
                if (in_reloadable) {
                    void* old_md = nullptr;
                    if (g.director_slot)
                        mem_read(g.director_slot, &old_md, sizeof(old_md));
                    if (old_md && g.comp_destroy && g.create_entity && g.create_mew_dir && g.continue_save) {
                        void* dir = nullptr;
                        mem_read((const uint8_t*)old_md + kComp_Director, &dir, sizeof(dir));
                        if (dir) {
                            g.cached_director = dir;
                            for (const char* sname : HouseState::reload_scenes) {
                                const void* sc = nullptr;
                                bool susp = false;
                                if (leave_find_scene(sname, &sc, &susp) && sc)
                                    *(uint8_t*)((uintptr_t)sc + kScene_Destroying) = 1;
                            }
                            g.comp_destroy(old_md);
                            g.cat_actors.clear();
                            g.current_house = nullptr;
                            g.current_scene = nullptr;
                            g.current_click_mgr = nullptr;
                            g.reload_stage = 1;
                            log_line("HOUSE", "reloading from host save checkpoint");
                            return;
                        }
                    }
                }
            }
        } else if (g.reload_stage == 1) {
            for (const char* name : HouseState::reload_scenes)
                if (leave_scene_attached(name)) return;
            if (g.cached_director && g.create_entity && g.create_mew_dir && g.continue_save) {
                void* shared = nullptr;
                mem_read((const uint8_t*)g.cached_director + kDirector_Shared, &shared, sizeof(shared));
                if (shared && !*(uint8_t*)((uintptr_t)shared + kScene_DoingDestruction) &&
                    savefile_write_pending_coop()) {
                    void* ent = g.create_entity(shared);
                    uint8_t s_name[32];
                    memset(s_name, 0, sizeof(s_name));
                    const char* name = savefile_coop_name();
                    const uint64_t len = strlen(name), cap = 15;
                    memcpy(s_name, name, len + 1);
                    memcpy(s_name + 16, &len, 8);
                    memcpy(s_name + 24, &cap, 8);
                    void* new_md = g.create_mew_dir(shared, ent, s_name);
                    g.continue_save(new_md);
                    g.reload_stage = 0;
                    log_line("HOUSE", "House checkpoint reloaded via MewDirector::ContinueSave");
                }
            }
            if (g.reload_stage != 0) return;
        }

        // Client: once the new House scene is loaded, kick off any queued
        // night/breeding interstitials for this generation before House ticks.
        if (g.has_queued_interstitial && g.queued_interstitial_gen == g.client_gen &&
            g.reload_stage == 0 && !savefile_has_pending()) {
            const void* hs = nullptr;
            bool hs_susp = false;
            if (leave_find_scene(kScene_House, &hs, &hs_susp) && hs) {
                *(uint8_t*)((uintptr_t)hs + kScene_Suspended) = 1;
                if (!deserialize_and_run_interstitials(g.queued_interstitial_blob.data(),
                                                       (uint32_t)g.queued_interstitial_blob.size(),
                                                       hs)) {
                    *(uint8_t*)((uintptr_t)hs + kScene_Suspended) = 0;
                }
                g.has_queued_interstitial = false;
                g.queued_interstitial_blob.clear();
            }
        }

        client_interpolate_cats();
        send_cursor_sample();
    }
}

bool house_catchup(uint8_t peer) {
    if (!g.on || g.is_client) return false;
    if (peer == kHostPeer || peer >= kMaxPeers) return false;
    if (g.phase == HostPhase::Outside) {
        const void* scene = nullptr;
        bool suspended = false;
        if (leave_find_scene("ActSelection", &scene, &suspended))
            g.prep_stage = PrepStage::ActSelection;
        else if (leave_find_scene("StorageItems", &scene, &suspended))
            g.prep_stage = PrepStage::StorageItems;
        else if (leave_find_scene("ClassChooser", &scene, &suspended))
            g.prep_stage = PrepStage::ClassChooser;
        if (g.prep_stage != PrepStage::None) {
            g.phase = HostPhase::Prep;
            if (g.generation == 0) g.generation = 1;
            host_poll_prep_state();
        } else if (leave_find_scene(kScene_House, &scene, &suspended) && !suspended) {
            g.phase = HostPhase::House;
            if (g.generation == 0) g.generation = 1;
        }
    }

    const bool starting = g.phase == HostPhase::Adventure && g.waiting_first_node;
    if (!starting && g.phase != HostPhase::House && g.phase != HostPhase::Prep) {
        g.peer_needs_checkpoint[peer] = false;
        return false;
    }
    g.peer_map_ready[peer] = false;
    g.peer_needs_checkpoint[peer] = true;
    if (starting && !g.peer_map_deadline[peer])
        g.peer_map_deadline[peer] = GetTickCount64() + tune::kHouseMapReadyMs;
    if ((starting && g.need_adv_publish) || g.in_pass_day || g.need_day_publish ||
        g.in_return || g.need_ret_publish)
        return true;
    if (!savefile_publish_active(peer, /*fresh=*/true, /*flush_adventure=*/false))
        return true;
    g.peer_needs_checkpoint[peer] = false;

    if (starting) {
        HousePhaseMsg pm{ g.generation, (uint8_t)HostPhase::Adventure, 0 };
        net_send_house_phase_to(peer, pm);
        g.peer_map_deadline[peer] = GetTickCount64() + tune::kHouseMapReadyMs;
        return true;
    }
    send_baseline(peer, true);
    if (g.phase == HostPhase::Prep) {
        HousePhaseMsg pm{ g.generation, (uint8_t)HostPhase::Prep, 0 };
        net_send_house_phase_to(peer, pm);
        if (g.have_prep)
            net_send_house_prep_to(peer, g.last_prep);
    }
    return true;
}

void house_on_peer_disconnect(uint8_t peer) {
    if (peer < kMaxPeers) {
        g.peer_cursors[peer].valid = false;
        g.peer_map_ready[peer]     = false;
        g.peer_needs_checkpoint[peer] = false;
        g.peer_map_deadline[peer] = 0;
    }
    if (g.is_client) return;
    std::vector<int64_t> to_release;
    for (auto& kv : g.host_captures)
        if (kv.second.holder_peer == peer)
            to_release.push_back(kv.first);
    for (int64_t id : to_release) {
        auto it = g.host_captures.find(id);
        if (it != g.host_captures.end()) {
            log_line("HOUSE", "releasing cat %lld held by disconnected peer %u",
                     (long long)id, (unsigned)peer);
            host_release_capture(id, it->second.wx, it->second.wy, true);
        }
    }
}

void house_on_message(uint8_t from, const NetMsg& m) {
    if (!g.on) return;
    uint32_t now = now_ms();

    switch (m.type) {
        case MSG_HOUSE_BASELINE: {
            if (!g.is_client || from != kHostPeer) break;
            const auto& b = m.house_baseline;
            if (!b.generation || b.generation < g.client_gen ||
                b.phase > static_cast<uint8_t>(HostPhase::Adventure) ||
                (b.generation == g.client_gen &&
                 (b.roster_rev < g.client_roster_rev ||
                  (g.phase == HostPhase::Adventure &&
                   b.phase == static_cast<uint8_t>(HostPhase::House)))))
                break;
            if (b.generation != g.client_gen) {
                g.client_gen = b.generation;
                clear_generation_state();
            }
            const auto phase = static_cast<HostPhase>(b.phase);
            if (g.phase == HostPhase::Adventure && phase == HostPhase::House) {
                follow_forget();
                lockstep_leave_run();
            }
            g.phase = phase;
            g.client_roster_rev = b.roster_rev;
            g.queued_baseline_cats.clear();
            for (uint16_t i = 0; i < b.count; ++i)
                g.queued_baseline_cats.push_back(b.cats[i]);
            g.queued_baseline_gen = b.generation;
            g.queued_baseline_tick = b.host_tick_ms;
            g.queued_returning = b.returning != 0;
            observe_pose_clock(b.host_tick_ms, now);
            g.has_queued_baseline = true;
            log_line("HOUSE", "<- BASELINE gen=%u cats=%u phase=%u",
                     b.generation, (unsigned)b.count, (unsigned)b.phase);
            break;
        }
        case MSG_CATDATA: {
            const auto& cat = m.catdata;
            if (!g.is_client || from != kHostPeer || cat.house_generation != g.client_gen ||
                !cat.id || !cat.data || !cat.size)
                break;
            const auto id = static_cast<int64_t>(cat.id);
            if (g.queued_cat_records.size() >= kMaxHouseCats && !g.queued_cat_records.count(id))
                break;
            auto& record = g.queued_cat_records[id];
            record.hash = cat.hash;
            record.bytes.assign(cat.data, cat.data + cat.size);
            break;
        }
        case MSG_HOUSE_POSE: {
            if (!g.is_client || from != kHostPeer) break;
            const auto& hp = m.house_pose;
            if (hp.generation != g.client_gen || g.phase != HostPhase::House) break;
            observe_pose_clock(hp.host_tick_ms, now);
            for (uint16_t i = 0; i < hp.count; ++i) {
                const auto& p = hp.cats[i];
                auto& tr = g.client_tracks[p.cat_id];
                if (tr.samples.size() > 16)
                    tr.samples.erase(tr.samples.begin());
                if (!tr.samples.empty()) {
                    const auto& prev = tr.samples.back().pose;
                    const float dx = p.x - prev.x;
                    const float dy = p.y - prev.y;
                    if (p.capture_id != prev.capture_id || strcmp(p.zone, prev.zone) ||
                        dx * dx + dy * dy > 9.0f)
                        tr.samples.clear();
                }
                tr.samples.push_back(InterpSample{ hp.host_tick_ms, p });
            }
            break;
        }
        case MSG_HOUSE_GRAB_REQ: {
            if (g.is_client) break;
            const auto& req = m.house_grab_req;
            if (req.generation != g.generation) {
                HouseGrabRespMsg resp{ g.generation, req.cat_id, kNoPeer, 0, 0,
                                       req.req_seq, req.wx, req.wy };
                net_send_house_grab_resp_to(from, resp);
                break;
            }
            host_try_grant_grab(from, req.cat_id, req.req_seq, req.wx, req.wy);
            break;
        }
        case MSG_HOUSE_GRAB_RESP: {
            if (!g.is_client || from != kHostPeer) break;
            const auto& r = m.house_grab_resp;
            if (r.generation != g.client_gen) break;
            auto& tr = g.client_tracks[r.cat_id];
            if (r.accepted) {
                tr.holder_peer = r.holder_peer;
                tr.capture_id  = r.capture_id;
                if (r.holder_peer == net_self()) {
                    if (g.client_pending_cat == r.cat_id) {
                        g.client_pending_cat = 0;
                        g.client_pending_req = 0;
                    }
                    if (tr.pending_release_before_grant) {
                        tr.pending_release_before_grant = false;
                        tr.predicted_held = false;
                        HouseDropReqMsg drop{ g.client_gen, r.cat_id, r.capture_id,
                                              g.client_move_seq++,
                                              tr.pending_release_wx, tr.pending_release_wy,
                                              static_cast<float>(tr.throw_velocity.x),
                                              static_cast<float>(tr.throw_velocity.y) };
                        net_send_house_drop_req(drop);
                    } else {
                        g.client_held_cat    = r.cat_id;
                        g.client_held_cap_id = r.capture_id;
                        tr.predicted_held    = true;
                    }
                } else {
                    tr.predicted_held = false;
                }
            } else {
                // Rejected: reconcile predicted pickup back to host pose.
                if (tr.pending_req_seq == r.req_seq) {
                    tr.predicted_held = false;
                    tr.pending_release_before_grant = false;
                    if (g.client_pending_cat == r.cat_id) {
                        g.client_pending_cat = 0;
                        g.client_pending_req = 0;
                    }
                    auto ait = g.cat_actors.find(r.cat_id);
                    if (ait != g.cat_actors.end() && tr.has_last)
                        apply_pose_to_client_actor(ait->second, tr.last_applied, true);
                }
            }
            break;
        }
        case MSG_HOUSE_DRAG_MOVE: {
            const auto& mv = m.house_drag_move;
            if (!g.is_client) {
                if (mv.generation != g.generation) break;
                auto it = g.host_captures.find(mv.cat_id);
                if (it == g.host_captures.end()) break;
                if (it->second.holder_peer != from || it->second.capture_id != mv.capture_id)
                    break;
                if (mv.seq <= it->second.last_seq && it->second.last_seq != 0) break;
                it->second.last_seq = mv.seq;
                it->second.wx = mv.wx;
                it->second.wy = mv.wy;
                auto ait = g.cat_actors.find(mv.cat_id);
                if (ait != g.cat_actors.end() && ait->second)
                    move_held_cat(ait->second, mv.wx, mv.wy);
            }
            break;
        }
        case MSG_HOUSE_DROP_REQ: {
            if (g.is_client) break;
            const auto& dr = m.house_drop_req;
            if (dr.generation != g.generation) break;
            auto it = g.host_captures.find(dr.cat_id);
            if (it == g.host_captures.end()) break;
            if (it->second.holder_peer != from || it->second.capture_id != dr.capture_id ||
                dr.seq <= it->second.last_seq)
                break;
            host_release_capture(dr.cat_id, dr.wx, dr.wy, true, {dr.vx, dr.vy});
            break;
        }
        case MSG_HOUSE_DROP_RESP: {
            if (!g.is_client || from != kHostPeer) break;
            const auto& dr = m.house_drop_resp;
            if (dr.generation != g.client_gen) break;
            auto& tr = g.client_tracks[dr.cat_id];
            if (tr.capture_id && tr.capture_id != dr.capture_id) break;
            tr.holder_peer = kNoPeer;
            tr.capture_id  = 0;
            tr.predicted_held = false;
            if (g.client_held_cat == dr.cat_id) {
                g.client_held_cat    = 0;
                g.client_held_cap_id = 0;
            }
            // The next pose carries the resolved layer, visibility and room.
            tr.samples.clear();
            break;
        }
        case MSG_HOUSE_CURSOR: {
            if (from >= kMaxPeers) break;
            const auto& c = m.house_cursor;
            uint32_t expected_gen = g.is_client ? g.client_gen : g.generation;
            if (c.generation != expected_gen) break;
            auto& pc = g.peer_cursors[from];
            pc.prev_wx = pc.valid && pc.gen == c.generation ? pc.wx : c.wx;
            pc.prev_wy = pc.valid && pc.gen == c.generation ? pc.wy : c.wy;
            pc.prev_ms = pc.valid && pc.gen == c.generation ? pc.recv_ms : now;
            pc.valid    = (c.in_house != 0);
            pc.in_house = c.in_house;
            pc.gen      = c.generation;
            pc.wx      = c.wx;
            pc.wy      = c.wy;
            pc.nx      = c.nx;
            pc.ny      = c.ny;
            pc.mode    = c.mode;
            pc.recv_ms = now;
            break;
        }
        case MSG_HOUSE_PREP: {
            if (!g.is_client || from != kHostPeer || m.house_prep.generation != g.client_gen)
                break;
            g.last_prep = m.house_prep;
            g.have_prep = (m.house_prep.active != 0);
            if (m.house_prep.active && m.house_prep.map_name[0] &&
                m.house_prep.stage == (uint8_t)PrepStage::ClassChooser &&
                g.entered_class_chooser_gen != m.house_prep.generation) {
                g.pending_class_chooser     = m.house_prep;
                g.pending_class_chooser_gen = m.house_prep.generation;
                g.has_pending_class_chooser = true;
            }
            break;
        }
        case MSG_HOUSE_PHASE: {
            const auto& pm = m.house_phase;
            if (!g.is_client) {
                if (g.waiting_first_node && pm.generation == g.generation &&
                    pm.phase == (uint8_t)HostPhase::Adventure && (pm.flags & 1) &&
                    from > kHostPeer && from < kMaxPeers && !g.peer_needs_checkpoint[from]) {
                    g.peer_map_ready[from] = true;
                    log_line("HOUSE", "peer %u map ready", (unsigned)from);
                }
            } else if (from == kHostPeer && pm.generation >= g.client_gen) {
                if (pm.generation != g.client_gen) {
                    clear_generation_state();
                    g.client_gen = pm.generation;
                }
                g.phase = (HostPhase)pm.phase;
                if (g.phase == HostPhase::Prep) {
                    g.client_held_cat    = 0;
                    g.client_held_cap_id = 0;
                    g.client_tracks.clear();
                } else if (g.phase == HostPhase::Adventure) {
                    // Clear House actor references before adventure transition.
                    g.has_pending_class_chooser = false;
    g.entered_class_chooser_gen = 0;
                    g.prep_stage = PrepStage::None;
                    g.client_map_generation = pm.generation;
                    g.sent_map_generation = 0;
                    g.cat_actors.clear();
                    g.current_house = nullptr;
                    g.current_scene = nullptr;
                }
            }
            break;
        }
        case MSG_HOUSE_INTERSTITIALS: {
            if (!g.is_client || from != kHostPeer) break;
            const auto& im = m.house_interstitials;
            if (im.generation == g.client_gen && im.data && im.size > 0) {
                g.queued_interstitial_blob.assign(im.data, im.data + im.size);
                g.queued_interstitial_gen = im.generation;
                g.has_queued_interstitial = true;
                log_line("HOUSE", "<- INTERSTITIALS gen=%u bytes=%u",
                         im.generation, im.size);
            }
            break;
        }
        case MSG_HOUSE_POOP_POP: {
            const auto& pm = m.house_poop_pop;
            const uint32_t expected_gen = g.is_client ? g.client_gen : g.generation;
            if (pm.generation == 0 || pm.generation != expected_gen) break;
            if (g.is_client && from != kHostPeer) break;
            if (!g.poop_pop_orig) break;

            const void* hs = nullptr;
            bool hs_susp = false;
            if (leave_find_scene(kScene_House, &hs, &hs_susp) && hs) {
                void** pieces = nullptr;
                uint32_t count = 0;
                if (get_cached_list(const_cast<void*>(hs), kCompId_FurniturePiece, &pieces, &count) && pieces) {
                    for (uint32_t i = 0; i < count; ++i) {
                        void* piece = pieces[i];
                        if (!piece) continue;
                        void* entry = nullptr;
                        int64_t uid = 0;
                        if (!mem_read((const uint8_t*)piece + kFPiece_Entry, &entry, sizeof(entry)) || !entry)
                            continue;
                        if (!mem_read((const uint8_t*)piece + kFPiece_Uid, &uid, sizeof(uid)) || uid != pm.piece_uid)
                            continue;
                        auto* scene_current = (void**)addr_of_data(D_SceneCurrent);
                        void* prev_scene = scene_current ? *scene_current : nullptr;
                        if (scene_current) *scene_current = const_cast<void*>(hs);
                        g.applying_remote_poop_pop = true;
                        g.poop_pop_orig(piece);
                        g.applying_remote_poop_pop = false;
                        if (scene_current) *scene_current = prev_scene;
                        log_line("HOUSE", "<- POOP_POP uid=%lld from peer %u",
                                 (long long)pm.piece_uid, (unsigned)from);
                        break;
                    }
                }
            }
            if (!g.is_client) {
                // Relay to all clients (originating client already popped it locally, so its lookup is a no-op).
                net_send_house_poop_pop(pm);
            }
            break;
        }
        case MSG_HOUSE_FURNITURE: {
            const auto& fm = m.house_furniture;
            const uint32_t expected_gen = g.is_client ? g.client_gen : g.generation;
            if (fm.generation == 0 || fm.generation != expected_gen) break;
            if (g.is_client && from != kHostPeer) break;
            if (!fm.data || fm.size < 2) break;

            const void* hs = nullptr;
            bool hs_susp = false;
            if (leave_find_scene(kScene_House, &hs, &hs_susp) && hs) {
                void** hdata = nullptr;
                uint32_t hcount = 0;
                if (get_cached_list(const_cast<void*>(hs), kCompId_House, &hdata, &hcount) &&
                    hcount > 0 && hdata && hdata[0]) {
                    if (apply_furniture_layout(fm.data, fm.size, const_cast<void*>(hs), hdata[0])) {
                        log_line("HOUSE", "<- FURNITURE (%u bytes) from peer %u", fm.size, (unsigned)from);
                        if (!g.is_client) {
                            // Persist client's layout into host save without triggering a full client reload.
                            void* md = nullptr;
                            if (g.autosave_house && g.director_slot &&
                                mem_read(g.director_slot, &md, sizeof(md)) && md) {
                                g.suppress_autosave_publish = true;
                                g.autosave_house(md);
                                g.suppress_autosave_publish = false;
                            }
                            // Relay to any other connected clients (3-4 player sessions).
                            uint8_t ids[kMaxPeers]{};
                            if (net_peer_ids(ids, kMaxPeers)) {
                                for (uint8_t i = 0; i < net_peer_count(); ++i) {
                                    const uint8_t peer = ids[i];
                                    if (peer == kHostPeer || peer == from || peer >= kMaxPeers) continue;
                                    net_send_house_furniture_to(peer, fm);
                                }
                            }
                        }
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

HostPhase house_phase()      { return g.phase; }
uint32_t  house_generation() { return g.is_client ? g.client_gen : g.generation; }
bool      house_in_house_or_prep() {
    return g.on && (g.phase == HostPhase::House || g.phase == HostPhase::Prep);
}

bool house_clients_map_ready() {
    if (!g.on || g.is_client || !g.waiting_first_node) return true;
    uint8_t ids[kMaxPeers] = {};
    if (!net_peer_ids(ids, kMaxPeers)) return true;
    const uint8_t n = net_peer_count();
    if (n <= 1) return true;
    if (g.need_adv_publish) return false;
    for (uint8_t i = 0; i < n; ++i) {
        if (ids[i] == kHostPeer) continue;
        if (!g.peer_map_ready[ids[i]]) return false;
    }
    return true;
}

void house_on_map_ready() {
    if (!g.on || !g.is_client || g.phase != HostPhase::Adventure ||
        !g.client_map_generation || g.sent_map_generation == g.client_map_generation ||
        g.reload_stage || savefile_has_pending() || !savefile_adventure_is_loaded())
        return;

    const void* scene = nullptr;
    bool suspended = false;
    if (!leave_find_scene("Map", &scene, &suspended) || suspended) return;

    invsync_apply_pending("adventure map ready");
    catsync_apply_pending("adventure map ready");
    runhist_apply_pending("adventure map ready");
    HousePhaseMsg pm{ g.client_map_generation, (uint8_t)HostPhase::Adventure, 1 };
    if (net_send_house_phase(pm)) g.sent_map_generation = g.client_map_generation;
}

void house_on_first_node() {
    g.waiting_first_node = false;
    g.need_adv_publish = false;
}

void house_status(char* out, size_t out_size) {
    if (!out || !out_size) return;
    const char* ph = "Outside";
    switch (g.phase) {
        case HostPhase::House:     ph = "House"; break;
        case HostPhase::Prep:      ph = "Prep"; break;
        case HostPhase::Adventure: ph = "Adventure"; break;
        default: break;
    }
    _snprintf_s(out, out_size, _TRUNCATE,
                "phase=%s gen=%u cats=%zu cap=%zu (ok=%u rej=%u drop=%u)",
                ph, (unsigned)house_generation(), g.cat_actors.size(),
                g.host_captures.size(), g.stat_grabs_granted,
                g.stat_grabs_rejected, g.stat_drops);
}

const HousePrepMsg* house_spectator_prep() {
    if (!g.on || !g.is_client || g.phase != HostPhase::Prep || !g.have_prep) return nullptr;
    if (g.last_prep.stage == (uint8_t)PrepStage::ClassChooser ||
        g.last_prep.stage == (uint8_t)PrepStage::StorageItems)
        return nullptr;
    return &g.last_prep;
}

bool house_get_peer_cursor(uint8_t peer, float* out_nx, float* out_ny,
                           uint8_t* out_mode, bool* out_active) {
    if (out_active) *out_active = false;
    if (!g.on || peer >= kMaxPeers) return false;
    if (g.phase != HostPhase::House && g.phase != HostPhase::Prep) return false;
    const auto& pc = g.peer_cursors[peer];
    if (!pc.valid) return false;
    if (now_ms() - pc.recv_ms > 2000) return false;
    if (pc.in_house == 2) {
        // Prep screen-space cursor (ClassChooser / StorageItems / ActSelection).
        if (out_nx) *out_nx = pc.nx;
        if (out_ny) *out_ny = pc.ny;
        if (out_mode) *out_mode = pc.mode;
        if (out_active) *out_active = true;
        return true;
    }
    if (!live_house_scene_valid()) return false;
    const uint32_t span = pc.recv_ms - pc.prev_ms;
    const uint32_t delay = std::min(span, tune::kHouseInterpDelayMs);
    const uint32_t target = now_ms() - delay;
    const int32_t elapsed = static_cast<int32_t>(target - pc.prev_ms);
    const float alpha = span ? std::clamp(static_cast<float>(elapsed) / span, 0.0f, 1.0f) : 1.0f;
    float wx = pc.prev_wx + alpha * (pc.wx - pc.prev_wx);
    float wy = pc.prev_wy + alpha * (pc.wy - pc.prev_wy);
    if (g.is_client) {
        for (const auto& item : g.client_tracks) {
            const auto& track = item.second;
            if (track.holder_peer == peer && track.capture_id && track.has_last) {
                const auto actor = g.cat_actors.find(item.first);
                if (actor != g.cat_actors.end()) {
                    const auto anchor = cat_drag_anchor(actor->second);
                    wx = static_cast<float>(track.last_applied.x + anchor.x);
                    wy = static_cast<float>(track.last_applied.y + anchor.y);
                }
                break;
            }
        }
    } else {
        for (const auto& item : g.host_captures) {
            const auto& capture = item.second;
            if (capture.holder_peer == peer) {
                wx = capture.wx;
                wy = capture.wy;
                break;
            }
        }
    }
    float nx = pc.nx, ny = pc.ny;
    project_world_to_norm(wx, wy, &nx, &ny);
    if (out_nx) *out_nx = nx;
    if (out_ny) *out_ny = ny;
    if (out_mode) *out_mode = pc.mode;
    if (out_active) *out_active = true;
    return true;
}

bool house_local_mouse_world(float* out_wx, float* out_wy) {
    if (!g.on || g.phase != HostPhase::House || !live_house_scene_valid()) return false;
    return house_mouse_world(out_wx, out_wy);
}

bool house_project_world(float wx, float wy, float* out_nx, float* out_ny) {
    if (!g.on || g.phase != HostPhase::House || !live_house_scene_valid()) return false;
    return project_world_to_norm(wx, wy, out_nx, out_ny);
}

// ---------------------------------------------------------------------------
// Game Hook Callbacks
// ---------------------------------------------------------------------------

void house_on_house_test_init(void* /*house_test*/, bool /*arg1*/, bool /*arg2*/) {
    if (!g.on) return;
    if (!g.is_client) {
        if (!g.in_pass_day && !g.in_return && !g.need_ret_publish) {
            advance_generation("HouseTest::init");
            g.need_baseline = true;
        }
        g.phase = HostPhase::House;
    } else {
        g.cat_actors.clear();
        g.current_house = nullptr;
        g.current_scene = nullptr;
        if (g.phase == HostPhase::Outside) g.phase = HostPhase::House;
    }
}

bool is_remote_captured_cat(void* hc) {
    if (g.is_client || !hc) return false;
    int64_t id = 0;
    if (!mem_read((const uint8_t*)hc + kHCat_CatId, &id, sizeof(id)) || id == 0)
        return false;
    auto it = g.host_captures.find(id);
    return it != g.host_captures.end() && it->second.holder_peer != kHostPeer;
}

bool house_allows_special_placement(void* cat) {
    return !g.on || (!g.is_client && !is_remote_captured_cat(cat));
}

void spoof_remote_held_begin(void* hc) {
    if (!g.current_click_mgr || !is_remote_captured_cat(hc)) return;
    mem_read((const uint8_t*)g.current_click_mgr + kHCatClick_HeldCat,
             &g.spoof_saved_held, sizeof(g.spoof_saved_held));
    memcpy((uint8_t*)g.current_click_mgr + kHCatClick_HeldCat, &hc, sizeof(hc));
    g.spoof_active = true;
}

void spoof_remote_held_end() {
    if (!g.spoof_active || !g.current_click_mgr) return;
    memcpy((uint8_t*)g.current_click_mgr + kHCatClick_HeldCat,
           &g.spoof_saved_held, sizeof(g.spoof_saved_held));
    g.spoof_saved_held = nullptr;
    g.spoof_active     = false;
}

bool house_on_housecat_update(void* housecat) {
    if (!g.on) return true;
    // Clients do not run local HouseCat AI/brain simulation; Animator + Renderer
    // still run natively from Scene's update/render lists.
    if (g.is_client) return false;
    spoof_remote_held_begin(housecat);
    return true;
}

void house_on_housecat_update_post(void* /*housecat*/) {
    spoof_remote_held_end();
}

bool house_on_housecat_late_update(void* /*housecat*/) {
    // Native late_update re-enables renderer visibility and can trigger swats.
    return !g.on || !g.is_client;
}

bool house_on_housecat_physics_update(void* physics) {
    if (!g.on) return true;
    if (g.is_client) return false;

    // On host, spoof ClickManager::held_cat for remotely-held cats so native
    // HouseCatPhysics zeroes velocity and tracks prev_dragging/prev_pos.
    if (physics) {
        void* hc = nullptr;
        if (mem_read((const uint8_t*)physics + kHCatPhys_Cat, &hc, sizeof(hc)) && hc) {
            if (is_remote_captured_cat(hc)) {
                if (g.current_click_mgr) {
                    spoof_remote_held_begin(hc);
                    return true;
                }
                Vec2D zero{ 0.0, 0.0 };
                memcpy((uint8_t*)physics + kHCatPhys_Velocity, &zero, sizeof(zero));
                return false;
            }
        }
    }
    return true;
}

void house_on_housecat_physics_update_post(void* /*physics*/) {
    spoof_remote_held_end();
}

bool house_on_click_manager_update(void* click_mgr) {
    if (!g.on || !click_mgr) return true;
    g.current_click_mgr = click_mgr;
    if (g.phase == HostPhase::Prep)
        return false;

    if (g.is_client) {
        const uint32_t t = now_ms();
        const bool left_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        // Handle pending release-before-grant if LBUTTON was released while
        // waiting for the host's GRAB_RESP.
        if (g.client_pending_cat != 0) {
            auto& tr = g.client_tracks[g.client_pending_cat];
            if (tr.predicted_held) {
                sample_client_drag(tr, t);
                if (!left_down) {
                    tr.predicted_held = false;
                    tr.pending_release_before_grant = true;
                    tr.pending_release_wx = tr.pred_wx;
                    tr.pending_release_wy = tr.pred_wy;
                }
            }
        }
        // If client currently has an active granted capture, send coalesced drag moves.
        if (g.client_held_cat != 0 && g.client_held_cap_id != 0) {
            auto& tr = g.client_tracks[g.client_held_cat];
            sample_client_drag(tr, t);
            if (left_down && t - g.client_last_drag_ms >= tune::kHouseCursorMs) {
                g.client_last_drag_ms = t;
                HouseDragMoveMsg mv{};
                mv.generation = g.client_gen;
                mv.cat_id     = g.client_held_cat;
                mv.capture_id = g.client_held_cap_id;
                mv.seq        = g.client_move_seq++;
                mv.wx         = tr.pred_wx;
                mv.wy         = tr.pred_wy;
                mv.flip       = 1;
                net_send_house_drag_move(mv);
            }
            if (!left_down) {
                HouseDropReqMsg dr{};
                dr.generation = g.client_gen;
                dr.cat_id     = g.client_held_cat;
                dr.capture_id = g.client_held_cap_id;
                dr.seq        = g.client_move_seq++;
                dr.wx         = tr.pred_wx;
                dr.wy         = tr.pred_wy;
                dr.vx         = static_cast<float>(tr.throw_velocity.x);
                dr.vy         = static_cast<float>(tr.throw_velocity.y);
                net_send_house_drop_req(dr);
                tr.predicted_held    = false;
                g.client_held_cat    = 0;
                g.client_held_cap_id = 0;
            }
        }
    }
    g.in_native_client_pickup = g.is_client;
    return true;
}

void house_on_click_manager_update_post(void* click_mgr) {
    g.in_native_client_pickup = false;
    if (!g.on || !click_mgr || g.phase != HostPhase::House) return;
    void* held = nullptr;
    mem_read((const uint8_t*)click_mgr + kHCatClick_HeldCat, &held, sizeof(held));

    if (!g.is_client) {
        if (held && g.local_host_held == 0) {
            int64_t id = 0;
            if (mem_read((const uint8_t*)held + kHCat_CatId, &id, sizeof(id)) && id != 0) {
                float wx = 0.0f, wy = 0.0f;
                house_mouse_world(&wx, &wy);
                if (!host_try_grant_grab(kHostPeer, id, 0, wx, wy)) {
                    // Undo the native pickup on a cat already controlled by a peer.
                    auto capture = g.host_captures.find(id);
                    if (capture != g.host_captures.end()) {
                        move_held_cat(held, capture->second.wx, capture->second.wy);
                        const int32_t depth = capture->second.orig_depth + 100000;
                        memcpy(static_cast<uint8_t*>(held) + kHCat_Depth, &depth, sizeof(depth));
                    }
                    void* null_ptr = nullptr;
                    memcpy((uint8_t*)click_mgr + kHCatClick_HeldCat, &null_ptr, sizeof(null_ptr));
                }
            }
        } else if (held && g.local_host_held != 0) {
            auto capture = g.host_captures.find(g.local_host_held);
            if (capture != g.host_captures.end()) {
                house_mouse_world(&capture->second.wx, &capture->second.wy);
            }
        } else if (!held && g.local_host_held != 0) {
            float wx = 0.0f, wy = 0.0f;
            house_mouse_world(&wx, &wy);
            host_release_capture(g.local_host_held, wx, wy, false);
        }
        return;
    }

    // Client: if native ClickManager just picked up a cat, hijack it into
    // MGMP's predicted grab + GRAB_REQ path and clear the native slot.
    if (held) {
        void* null_ptr = nullptr;
        memcpy((uint8_t*)click_mgr + kHCatClick_HeldCat, &null_ptr, sizeof(null_ptr));
        if (g.client_held_cat == 0 && g.client_pending_cat == 0) {
            int64_t id = 0;
            if (mem_read((const uint8_t*)held + kHCat_CatId, &id, sizeof(id)) && id != 0) {
                auto& tr = g.client_tracks[id];
                if (tr.holder_peer == kNoPeer) {
                    float wx = 0.0f, wy = 0.0f;
                    house_mouse_world(&wx, &wy);
                    uint32_t req_seq = g.client_req_seq++;
                    tr.predicted_held               = true;
                    tr.pending_req_seq              = req_seq;
                    tr.pending_release_before_grant = false;
                    tr.pred_wx                      = wx;
                    tr.pred_wy                      = wy;
                    tr.throw_velocity               = {};
                    tr.last_motion_ms               = now_ms();
                    g.client_pending_cat            = id;
                    g.client_pending_req            = req_seq;

                    HouseGrabReqMsg req{};
                    req.generation = g.client_gen;
                    req.cat_id     = id;
                    req.req_seq    = req_seq;
                    req.wx         = wx;
                    req.wy         = wy;
                    net_send_house_grab_req(req);
                }
            }
        }
    }
}

bool house_on_click_manager_drop(void* click_mgr) {
    if (!g.on) return true;
    if (g.is_client) {
        // Client never executes native HouseCatClickManager::drop; host resolves
        // zone placement and broadcasts MSG_HOUSE_DROP_RESP.
        if (click_mgr) {
            void* null_ptr = nullptr;
            memcpy((uint8_t*)click_mgr + kHCatClick_HeldCat, &null_ptr, sizeof(null_ptr));
        }
        return false;
    }
    return true;
}

void house_on_click_manager_drop_post() {
    if (g.on && !g.is_client && g.local_host_held != 0)
        host_release_capture(g.local_host_held, 0.0f, 0.0f, false);
}

bool house_on_room_remove(void* area, void* cat) {
    if (!g.on || !g.is_client || !g.in_native_client_pickup) return true;
    if (area && cat) remove_client_room_member(area, cat);
    return false;
}

bool house_allows_local_mutations() {
    return !g.on || !g.is_client;
}

void house_on_layout_saved_pre() {
    if (g.on && !g.is_client && g.phase == HostPhase::House)
        g.suppress_autosave_publish = true;
}

void house_on_layout_saved() {
    g.suppress_autosave_publish = false;
    if (!g.on || net_peer_count() == 0) return;
    const uint32_t gen = g.is_client ? g.client_gen : g.generation;
    if (gen == 0) return;

    std::vector<uint8_t> blob;
    if (serialize_furniture_layout(blob) && !blob.empty()) {
        HouseFurnitureMsg msg{};
        msg.generation = gen;
        msg.size       = (uint32_t)blob.size();
        msg.data       = blob.data();
        net_send_house_furniture(msg);
        log_line("HOUSE", "-> FURNITURE gen=%u (%u bytes)", gen, msg.size);
    } else if (!g.is_client && g.phase == HostPhase::House) {
        g.need_roster_publish = true;
    }
}

void house_set_autosave_orig(void* fn) {
    g.autosave_house = (FnVoidPtr)fn;
}

void house_on_autosave_house() {
    if (!g.on || g.is_client) return;
    if (g.phase != HostPhase::House) return;
    if (g.in_pass_day || g.in_return || g.suppress_autosave_publish ||
        g.need_day_publish || g.need_ret_publish || g.need_adv_publish || g.need_prep_publish)
        return;
    g.need_roster_publish = true;
    log_line("HOUSE", "host AutoSaveHouse -> scheduling checkpoint publish");
}

void house_on_pass_day_cb_pre() {
    if (!g.on || g.is_client) return;
    g.in_pass_day = true;
    host_release_all_captures();
}

void house_on_pass_day_cb_post() {
    if (!g.on || g.is_client) return;
    g.in_pass_day = false;
    advance_generation("House::PassDay");
    g.phase            = HostPhase::House;
    g.need_day_publish = true;
}

void house_on_go_on_adventure_cb_pre() {
    if (!g.on || g.is_client) return;
    host_release_all_captures();
    if (g.phase != HostPhase::Prep) {
        g.phase = HostPhase::Prep;
        HousePhaseMsg pm{ g.generation, (uint8_t)HostPhase::Prep, 0 };
        net_send_house_phase(pm);
    }
    log_line("HOUSE", "host finished ClassChooser -> StorageItems (gen=%u)", g.generation);
}

void house_on_go_on_adventure_cb_post() {
    if (!g.on || g.is_client) return;
    g.prep_stage        = PrepStage::StorageItems;
    g.need_prep_publish = true;
    host_poll_prep_state();
}

void house_on_start_adventure_cb_post() {
    if (!g.on || g.is_client) return;
    g.phase            = HostPhase::Adventure;
    g.prep_stage       = PrepStage::None;
    g.need_prep_publish = false;
    memset(g.peer_map_ready, 0, sizeof(g.peer_map_ready));
    memset(g.peer_map_deadline, 0, sizeof(g.peer_map_deadline));
    if (g.generation == 0) g.generation = 1;
    g.waiting_first_node = true;
    g.need_adv_publish = true;
    log_line("HOUSE", "host started adventure -- publishing map checkpoint");
}

void house_on_return_house_pre(bool success) {
    if (!g.on || g.is_client) return;
    g.in_return = true;
    advance_generation(success ? "ReturnToHouseSuccess" : "ReturnToHouseFail");
    g.phase = HostPhase::House;
    g.waiting_first_node = false;
    g.need_adv_publish = false;
    g.need_prep_publish = false;
    // Reset per-run follow state so the next adventure starts clean.
    follow_forget();
    lockstep_leave_run();
}

void house_on_return_house_post(bool /*success*/) {
    if (!g.on || g.is_client) return;
    g.in_return = false;
    g.need_ret_publish = true;
}

void house_on_return_cat_from_adv(void* /*house*/, int64_t cat_id) {
    if (!g.on || g.is_client) return;
    rebuild_actor_map();
    const auto found = g.cat_actors.find(cat_id);
    HouseCatPose pose{};
    if (found != g.cat_actors.end() && read_housecat_pose(found->second, pose)) {
        if (!g.return_first_tick) g.return_first_tick = now_ms();
        g.returning_first_poses.emplace(cat_id, pose);
    }
    log_line("HOUSE", "returning cat %lld spawned in House", (long long)cat_id);
}

void house_set_do_interstitials_orig(void* fn) {
    g.do_interstitials = (FnDoInterstitials)fn;
}

void house_on_do_interstitials(void* queue) {
    if (!g.on || g.is_client || !g.in_pass_day || !queue) return;
    if (serialize_interstitial_queue(queue, g.pending_interstitial_blob)) {
        g.has_pending_interstitial_blob = true;
        log_line("HOUSE", "captured PassDay InterstitialQueue (%zu bytes)",
                 g.pending_interstitial_blob.size());
    } else {
        g.has_pending_interstitial_blob = false;
        g.pending_interstitial_blob.clear();
    }
}

void house_set_go_on_adventure_orig(void* fn) {
    g.go_on_adv = (FnGoOnAdventure)fn;
}

void house_set_poop_pop_orig(void* fn) {
    g.poop_pop_orig = (FnVoidPtr)fn;
}

void house_on_poop_pop(void* piece) {
    if (!g.poop_pop_orig) return;
    if (!g.on || g.applying_remote_poop_pop || !piece) {
        g.poop_pop_orig(piece);
        return;
    }
    void* entry_before = nullptr;
    int64_t uid = 0;
    mem_read((const uint8_t*)piece + kFPiece_Entry, &entry_before, sizeof(entry_before));
    mem_read((const uint8_t*)piece + kFPiece_Uid, &uid, sizeof(uid));

    g.poop_pop_orig(piece);

    void* entry_after = nullptr;
    mem_read((const uint8_t*)piece + kFPiece_Entry, &entry_after, sizeof(entry_after));
    const uint32_t gen = g.is_client ? g.client_gen : g.generation;
    if (entry_before && !entry_after && gen > 0 && net_peer_count() > 0) {
        HousePoopPopMsg msg{};
        msg.generation = gen;
        msg.piece_uid  = uid;
        net_send_house_poop_pop(msg);
        log_line("HOUSE", "-> POOP_POP gen=%u uid=%lld", gen, (long long)uid);
    }
}

void house_on_go_on_adventure_pre(void* /*md*/, void* map_str, void* collars_vec, void* cat_ids_buf) {
    if (!g.on || g.is_client) return;
    host_release_all_captures();
    if (g.generation == 0) g.generation = 1;
    g.phase      = HostPhase::Prep;
    g.prep_stage = PrepStage::ClassChooser;

    HousePrepMsg msg{};
    msg.generation = g.generation;
    msg.active     = 1;
    msg.stage      = (uint8_t)PrepStage::ClassChooser;
    if (map_str)
        mem_read_std_string(map_str, msg.map_name, sizeof(msg.map_name));

    if (collars_vec) {
        const uint8_t* first = nullptr;
        const uint8_t* last  = nullptr;
        mem_read((const uint8_t*)collars_vec + 0, &first, sizeof(first));
        mem_read((const uint8_t*)collars_vec + 8, &last,  sizeof(last));
        if (first && last && last >= first) {
            size_t cnt = (size_t)(last - first) / 32;
            if (cnt > kMaxPrepCollars) cnt = kMaxPrepCollars;
            msg.collar_count = (uint8_t)cnt;
            for (size_t i = 0; i < cnt; ++i)
                mem_read_std_string(first + i * 32, msg.collars[i], sizeof(msg.collars[i]));
        }
    }

    if (cat_ids_buf) {
        uint32_t count = 0;
        const int64_t* ids = nullptr;
        mem_read((const uint8_t*)cat_ids_buf + 4, &count, sizeof(count));
        mem_read((const uint8_t*)cat_ids_buf + 8, &ids,   sizeof(ids));
        if (count > kMaxPrepCats) count = kMaxPrepCats;
        msg.count = (uint8_t)count;
        for (uint8_t i = 0; i < msg.count; ++i) {
            if (ids) mem_read(ids + i, &msg.cats[i].cat_id, sizeof(int64_t));
        }
    }

    g.last_prep = msg;
    g.have_prep = true;

    HousePhaseMsg pm{ g.generation, (uint8_t)HostPhase::Prep, 0 };
    net_send_house_phase(pm);
    net_send_house_prep(msg);
    log_line("HOUSE", "host entered ClassChooser (gen=%u, map=%s, cats=%u, collars=%u)",
             g.generation, msg.map_name, (unsigned)msg.count, (unsigned)msg.collar_count);
}

} // namespace mgmp
