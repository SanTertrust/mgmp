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
#include <new>
#include <unordered_map>
#include <vector>

namespace mgmp {
namespace {

struct alignas(16) Vec2D { double x = 0.0, y = 0.0; };
struct Vec3D { double x = 0.0, y = 0.0, z = 0.0; };

using FnSceneUpdateCachedList = void(__fastcall*)(void* scene, int comp_id);
using FnHouseLocUnderPoint    = void*(__fastcall*)(void* house, Vec2D* pt);
using FnPlaceCat              = void*(__fastcall*)(void* area, void* housecat);
using FnRemoveCat             = int64_t(__fastcall*)(void* area, void* housecat);
using FnCameraGetMouse        = Vec2D*(__fastcall*)(void* cam, Vec2D* out);
using FnCameraMatrix          = float*(__fastcall*)(void* cam, float* out_mat);
using FnGameStrAlloc          = void*(__fastcall*)(size_t);
using FnGameStrDtor           = void(__fastcall*)(void*);
using FnComponentDestroy      = void(__fastcall*)(void* comp);
using FnSceneCreateEntity     = void*(__fastcall*)(void* scene);
using FnSceneCreateMewDir     = void*(__fastcall*)(void* scene, void* entity, void* std_str);
using FnMewDirContinueSave    = void(__fastcall*)(void* mew_dir);
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

struct InterpSample {
    uint32_t host_tick_ms = 0;
    uint32_t recv_ms      = 0;
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
};

struct HostCapture {
    int64_t  cat_id       = 0;
    uint8_t  holder_peer  = kNoPeer;
    uint32_t capture_id   = 0;
    uint32_t last_seq     = 0;
    float    wx           = 0.0f;
    float    wy           = 0.0f;
    float    vx           = 0.0f;
    float    vy           = 0.0f;
    uint32_t last_move_ms = 0;
    int32_t  orig_depth   = 0;
};

struct PeerCursorState {
    bool     valid    = false;
    uint32_t gen      = 0;
    float    wx       = 0.0f;
    float    wy       = 0.0f;
    float    nx       = 0.0f;
    float    ny       = 0.0f;
    uint8_t  mode     = 0;
    uint32_t recv_ms  = 0;
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

    // Lifecycle & phase
    HostPhase phase            = HostPhase::Outside;
    uint32_t  generation       = 0;
    uint32_t  roster_rev       = 1;
    bool      in_pass_day      = false;
    bool      need_day_publish = false;
    bool      need_ret_publish = false;
    bool      need_adv_publish = false;
    bool      need_baseline    = false;
    bool      suppress_local_drag = false;

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
    uint32_t last_cursor_ms   = 0;
    int64_t  local_host_held  = 0;
    std::vector<int64_t> prev_actor_ids;
    std::vector<uint8_t> pending_interstitial_blob;
    bool                 has_pending_interstitial_blob = false;
    std::unordered_map<int64_t, HostCapture> host_captures;
    std::unordered_map<int64_t, HouseCatPose> last_sent_poses;

    // Preparation spectator state
    HousePrepMsg last_prep{};
    bool         have_prep = false;
    bool         peer_map_ready[kMaxPeers] = {};

    // Client state
    uint32_t client_gen           = 0;
    uint32_t client_req_seq       = 1;
    uint32_t client_move_seq      = 1;
    int64_t  client_held_cat      = 0;
    uint32_t client_held_cap_id   = 0;
    uint32_t client_pending_req   = 0;
    int64_t  client_pending_cat   = 0;
    bool     client_waiting_house = false;
    uint32_t client_last_drag_ms  = 0;
    std::unordered_map<int64_t, ClientCatTrack> client_tracks;
    std::vector<HouseCatPose> queued_baseline_cats;
    uint32_t                  queued_baseline_gen = 0;
    bool                      has_queued_baseline = false;
    std::vector<uint8_t>      queued_interstitial_blob;
    uint32_t                  queued_interstitial_gen = 0;
    bool                      has_queued_interstitial = false;
    int                       reload_stage = 0; // 0 = idle, 1 = destroyed old MewDirector, waiting 1 frame
    void*                     cached_director = nullptr;

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
    if (mem_read((const uint8_t*)hc + kHCat_CurrentLocation, &loc, sizeof(loc)) && loc)
        mem_read_std_string((const uint8_t*)loc + kCPA_RoomName, out.zone, sizeof(out.zone));

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

void apply_pose_to_client_actor(void* hc, const HouseCatPose& p, bool snap_zone) {
    if (!hc) return;

    void* tr = nullptr;
    if (mem_read((const uint8_t*)hc + kHCat_Transform, &tr, sizeof(tr)) && tr) {
        Vec3D pos{ (double)p.x, (double)p.y, (double)p.z };
        memcpy((uint8_t*)tr + kTransform_Pos, &pos, sizeof(pos));
        Vec2D flip{ (double)(p.flip < 0 ? -1.0 : 1.0), 1.0 };
        memcpy((uint8_t*)tr + kTransform_Flip, &flip, sizeof(flip));
    }

    int32_t depth = p.depth;
    memcpy((uint8_t*)hc + kHCat_Depth, &depth, sizeof(depth));

    uint8_t no_overlap = 0;
    memcpy((uint8_t*)hc + kHCat_CheckOverlaps, &no_overlap, 1);

    uint8_t rag = (p.holder_peer != kNoPeer || p.anim_state == 9) ? 1 : 0;
    memcpy((uint8_t*)hc + kHCat_Ragdolling, &rag, 1);

    void* rend = nullptr;
    if (mem_read((const uint8_t*)hc + kHCat_Renderer, &rend, sizeof(rend)) && rend) {
        uint8_t vis = p.visible ? 1 : 0;
        memcpy((uint8_t*)rend + kRenderer_Visible, &vis, 1);
    }

    void* anim = nullptr;
    if (mem_read((const uint8_t*)hc + kHCat_Animator, &anim, sizeof(anim)) && anim) {
        uint8_t* req = (uint8_t*)anim + kAnim_RequestedState;
        int32_t aflip = p.flip < 0 ? -1 : 1;
        memcpy(req + kAnimState_State, &p.anim_state, sizeof(p.anim_state));
        memcpy(req + kAnimState_Flip, &aflip, sizeof(aflip));
        if (p.anim[0])
            write_game_std_string(req + kAnimState_Anim, p.anim);
        if (p.substate[0])
            write_game_std_string(req + kAnimState_Substate, p.substate);
    }

    if (snap_zone) {
        void* cur_loc = nullptr;
        mem_read((const uint8_t*)hc + kHCat_CurrentLocation, &cur_loc, sizeof(cur_loc));
        void* target_loc = p.zone[0] ? find_zone_by_name(p.zone) : nullptr;
        if (target_loc != cur_loc) {
            if (cur_loc && g.remove_cat)
                g.remove_cat(cur_loc, hc);
            if (target_loc && g.place_cat)
                g.place_cat(target_loc, hc);
            else if (!target_loc) {
                void* null_ptr = nullptr;
                memcpy((uint8_t*)hc + kHCat_CurrentLocation, &null_ptr, sizeof(null_ptr));
            }
        }
    }
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
    g.local_host_held    = 0;
    g.client_held_cat    = 0;
    g.client_held_cap_id = 0;
    g.client_pending_req = 0;
    g.client_pending_cat = 0;
    g.client_tracks.clear();
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
    float m[16] = {};
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

void send_baseline(uint8_t target_peer) {
    if (g.is_client || !net_active()) return;
    rebuild_actor_map();

    std::vector<HouseCatPose> list;
    list.reserve(g.cat_actors.size());
    for (auto& kv : g.cat_actors) {
        HouseCatPose p{};
        if (read_housecat_pose(kv.second, p))
            list.push_back(p);
    }

    uint8_t night = 0;
    if (g.current_house)
        mem_read((const uint8_t*)g.current_house + kHouse_Night, &night, 1);

    HouseBaselineMsg msg{};
    msg.generation   = g.generation;
    msg.roster_rev   = g.roster_rev;
    msg.host_tick_ms = now_ms();
    msg.phase        = (uint8_t)g.phase;
    msg.night        = night ? 1 : 0;
    msg.count        = (uint16_t)(list.size() > kMaxHouseCats ? kMaxHouseCats : list.size());
    msg.cats         = list.empty() ? nullptr : list.data();

    if (target_peer == kNoPeer)
        net_send_house_baseline(msg);
    else
        net_send_house_baseline_to(target_peer, msg);

    log_line("HOUSE", "-> BASELINE gen=%u cats=%u phase=%u (%s)",
             msg.generation, (unsigned)msg.count, (unsigned)msg.phase,
             target_peer == kNoPeer ? "broadcast" : "peer");
}

bool pose_changed(const HouseCatPose& a, const HouseCatPose& b) {
    if (fabsf(a.x - b.x) > 0.005f || fabsf(a.y - b.y) > 0.005f || fabsf(a.z - b.z) > 0.005f)
        return true;
    if (a.flip != b.flip || a.depth != b.depth || a.visible != b.visible ||
        a.anim_state != b.anim_state)
        return true;
    if (strcmp(a.anim, b.anim) != 0 || strcmp(a.substate, b.substate) != 0 ||
        strcmp(a.zone, b.zone) != 0)
        return true;
    return false;
}

void host_sample_and_send_poses() {
    if (g.phase != HostPhase::House || !net_active()) return;
    rebuild_actor_map();
    if (!g.current_house) return;

    uint32_t t = now_ms();
    bool do_full = (t - g.last_full_ms >= tune::kHouseFullPoseMs);
    bool do_move = (t - g.last_move_ms >= tune::kHouseMovePoseMs);
    if (!do_full && !do_move) return;

    // Detect mid-day cat additions or removals (e.g. flushing a retiree down
    // the NPC pipe) and broadcast a fresh BASELINE so the client removes them.
    std::vector<int64_t> cur_ids;
    cur_ids.reserve(g.cat_actors.size());
    for (const auto& kv : g.cat_actors) cur_ids.push_back(kv.first);
    std::sort(cur_ids.begin(), cur_ids.end());
    if (cur_ids != g.prev_actor_ids) {
        g.prev_actor_ids = cur_ids;
        if (!g.need_day_publish && !g.need_ret_publish)
            send_baseline(kNoPeer);
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
            if (it == g.last_sent_poses.end() || pose_changed(it->second, p)) {
                out.push_back(p);
                g.last_sent_poses[p.cat_id] = p;
            }
        }
    }

    if (do_full) g.last_full_ms = t;
    if (do_move) g.last_move_ms = t;
    if (out.empty()) return;

    HousePoseMsg msg{};
    msg.generation   = g.generation;
    msg.host_tick_ms = t;
    msg.full         = do_full ? 1 : 0;
    msg.count        = (uint16_t)(out.size() > kMaxHouseCats ? kMaxHouseCats : out.size());
    msg.cats         = out.data();
    net_send_house_pose(msg);
}

void update_capture_velocity(HostCapture& cap, float wx, float wy, uint32_t now) {
    uint32_t dt_ms = now - cap.last_move_ms;
    if (cap.last_move_ms != 0 && dt_ms > 0 && dt_ms < 200) {
        double dt = (double)dt_ms * 0.001;
        double vx = ((double)wx - (double)cap.wx) / dt * 0.5;
        double vy = ((double)wy - (double)cap.wy) / dt * 0.5;
        double len2 = vx * vx + vy * vy;
        if (len2 > 400.0) {
            double s = 20.0 / std::sqrt(len2);
            vx *= s;
            vy *= s;
        }
        cap.vx = (float)vx;
        cap.vy = (float)vy;
    } else if (dt_ms >= 200) {
        cap.vx = 0.0f;
        cap.vy = 0.0f;
    }
    cap.wx = wx;
    cap.wy = wy;
    cap.last_move_ms = now;
}

void host_release_capture(int64_t cat_id, float wx, float wy, bool apply_drop) {
    auto it = g.host_captures.find(cat_id);
    if (it == g.host_captures.end()) return;
    HostCapture cap = it->second;
    g.host_captures.erase(it);
    if (cap.holder_peer == kHostPeer && g.local_host_held == cat_id)
        g.local_host_held = 0;

    char zone[48] = {};
    bool accepted = false;
    auto ait = g.cat_actors.find(cat_id);
    if (ait != g.cat_actors.end() && ait->second) {
        void* hc = ait->second;
        memcpy((uint8_t*)hc + kHCat_Depth, &cap.orig_depth, sizeof(cap.orig_depth));

        if (apply_drop) {
            void* tr = nullptr;
            if (mem_read((const uint8_t*)hc + kHCat_Transform, &tr, sizeof(tr)) && tr) {
                Vec3D pos{ (double)wx, (double)wy, 0.0 };
                memcpy((uint8_t*)tr + kTransform_Pos, &pos, sizeof(pos));
            }
            if (cap.holder_peer != kHostPeer) {
                uint32_t now = now_ms();
                if (now - cap.last_move_ms >= 120) {
                    cap.vx = 0.0f;
                    cap.vy = 0.0f;
                } else if (wx != cap.wx || wy != cap.wy) {
                    update_capture_velocity(cap, wx, wy, now);
                }
                void* phys = nullptr;
                if (mem_read((const uint8_t*)hc + kHCat_Physics, &phys, sizeof(phys)) && phys) {
                    Vec2D vel{ (double)cap.vx, (double)cap.vy };
                    Vec2D ppos{ (double)wx, (double)wy };
                    uint8_t zero = 0;
                    memcpy((uint8_t*)phys + kHCatPhys_Velocity, &vel, sizeof(vel));
                    memcpy((uint8_t*)phys + kHCatPhys_PrevPos, &ppos, sizeof(ppos));
                    memcpy((uint8_t*)phys + kHCatPhys_PrevDragging, &zero, 1);
                }
                uint8_t rag = 1;
                memcpy((uint8_t*)hc + kHCat_Ragdolling, &rag, 1);
            }
            if (g.current_house && g.loc_under_point) {
                Vec2D pt{ (double)wx, (double)wy };
                void* area = g.loc_under_point(g.current_house, &pt);
                if (area && g.place_cat) {
                    g.place_cat(area, hc);
                    mem_read_std_string((const uint8_t*)area + kCPA_RoomName, zone, sizeof(zone));
                    accepted = true;
                }
            }
        }
    }

    ++g.stat_drops;
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
}

void host_release_all_captures() {
    std::vector<int64_t> ids;
    ids.reserve(g.host_captures.size());
    for (auto& kv : g.host_captures) ids.push_back(kv.first);
    for (int64_t id : ids) {
        auto it = g.host_captures.find(id);
        if (it != g.host_captures.end())
            host_release_capture(id, it->second.wx, it->second.wy, false);
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
    HostCapture cap{};
    cap.cat_id      = cat_id;
    cap.holder_peer = peer;
    cap.capture_id  = g.next_capture_id++;
    cap.last_seq    = 0;
    cap.wx           = wx;
    cap.wy           = wy;
    cap.last_move_ms = now_ms();
    mem_read((const uint8_t*)hc + kHCat_Depth, &cap.orig_depth, sizeof(cap.orig_depth));

    // Preserve native pickup semantics: remove from room and raise depth.
    void* cur_loc = nullptr;
    if (mem_read((const uint8_t*)hc + kHCat_CurrentLocation, &cur_loc, sizeof(cur_loc)) &&
        cur_loc && g.remove_cat) {
        g.remove_cat(cur_loc, hc);
        void* null_ptr = nullptr;
        memcpy((uint8_t*)hc + kHCat_CurrentLocation, &null_ptr, sizeof(null_ptr));
    }
    int32_t raised_depth = cap.orig_depth + 100000;
    memcpy((uint8_t*)hc + kHCat_Depth, &raised_depth, sizeof(raised_depth));

    g.host_captures[cat_id] = cap;
    if (peer == kHostPeer) g.local_host_held = cat_id;
    ++g.stat_grabs_granted;

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
    msg.generation = g.generation;
    msg.active     = 1;
    msg.count      = (uint8_t)count;
    for (uint8_t i = 0; i < msg.count; ++i) {
        int64_t cid = 0;
        if (ids) mem_read(ids + i, &cid, sizeof(cid));
        msg.cats[i].cat_id = cid;
        // Cat class is at CatData + kCatData_Class (std::string)
        // We keep eq_summary concise; full CatData syncs on StartAdventure.
        if (reg && cid != 0) {
            using FnById = void*(__fastcall*)(const void*, int64_t);
            FnById by_id = (FnById)addr_of_call(C_CatDataById);
            if (by_id) {
                void* cd = by_id(reg, cid);
                if (cd)
                    mem_read_std_string((const uint8_t*)cd + kCatData_Class,
                                        msg.cats[i].cat_class, sizeof(msg.cats[i].cat_class));
            }
        }
    }

    if (!g.have_prep || memcmp(&msg, &g.last_prep, sizeof(msg)) != 0) {
        g.last_prep = msg;
        g.have_prep = true;
        net_send_house_prep(msg);
    }
}

void client_interpolate_cats() {
    if (!g.is_client || g.phase != HostPhase::House) return;
    uint32_t now = now_ms();
    rebuild_actor_map();

    // Apply any queued baseline once House actors exist.
    if (g.has_queued_baseline && g.queued_baseline_gen == g.client_gen && !g.cat_actors.empty()) {
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
        }
        std::sort(keep_ids.begin(), keep_ids.end());
        for (auto it = g.cat_actors.begin(); it != g.cat_actors.end(); ) {
            if (!std::binary_search(keep_ids.begin(), keep_ids.end(), it->first)) {
                void* hc = it->second;
                if (hc) {
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

    for (auto& kv : g.cat_actors) {
        int64_t id = kv.first;
        void*   hc = kv.second;
        auto tit = g.client_tracks.find(id);
        if (tit == g.client_tracks.end()) continue;
        ClientCatTrack& tr = tit->second;

        // Local predicted/granted drag follows local mouse every frame.
        if (tr.predicted_held || (g.client_held_cat == id && g.client_held_cap_id != 0)) {
            float wx = tr.pred_wx, wy = tr.pred_wy;
            if (house_mouse_world(&wx, &wy)) {
                tr.pred_wx = wx;
                tr.pred_wy = wy;
            }
            HouseCatPose p = tr.has_last ? tr.last_applied : HouseCatPose{};
            p.cat_id      = id;
            p.holder_peer = net_self();
            p.anim_state  = 9; // ragdoll state while held
            strcpy_s(p.anim, "_DEFAULT");
            p.x           = tr.pred_wx;
            p.y           = tr.pred_wy;
            p.z           = 0.0f;
            apply_pose_to_client_actor(hc, p, false);
            continue;
        }

        if (tr.samples.empty()) continue;

        // Interpolate using local receive-time offset by kHouseInterpDelayMs.
        uint32_t target_ms = now > tune::kHouseInterpDelayMs ? (now - tune::kHouseInterpDelayMs) : 0;
        const InterpSample* s0 = &tr.samples.front();
        const InterpSample* s1 = &tr.samples.back();

        if (tr.samples.size() == 1 || target_ms <= s0->recv_ms) {
            apply_pose_to_client_actor(hc, s0->pose, true);
            tr.last_applied = s0->pose;
            tr.has_last = true;
        } else if (target_ms >= s1->recv_ms) {
            uint32_t dt_ms = target_ms - s1->recv_ms;
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
                if (tr.samples[i].recv_ms <= target_ms && target_ms <= tr.samples[i + 1].recv_ms) {
                    s0 = &tr.samples[i];
                    s1 = &tr.samples[i + 1];
                    break;
                }
            }
            uint32_t span = s1->recv_ms - s0->recv_ms;
            float alpha = span > 0 ? (float)(target_ms - s0->recv_ms) / (float)span : 1.0f;
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
        while (tr.samples.size() > 2 && tr.samples[1].recv_ms <= target_ms)
            tr.samples.erase(tr.samples.begin());
    }
}

void send_cursor_sample() {
    if (!net_active() || g.phase != HostPhase::House) return;
    uint32_t t = now_ms();
    if (t - g.last_cursor_ms < tune::kHouseCursorMs) return;
    g.last_cursor_ms = t;

    float wx = 0.0f, wy = 0.0f;
    if (!house_mouse_world(&wx, &wy)) return;
    float nx = 0.0f, ny = 0.0f;
    project_world_to_norm(wx, wy, &nx, &ny);

    HouseCursorMsg m{};
    m.generation = g.is_client ? g.client_gen : g.generation;
    m.wx         = wx;
    m.wy         = wy;
    m.nx         = nx;
    m.ny         = ny;
    m.mode       = 0;
    m.in_house   = 1;
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

bool deserialize_and_run_interstitials(const uint8_t* data, uint32_t size) {
    if (!data || !size || !g.do_interstitials || !g.inter_ctor || !g.inter_dtor ||
        !g.vec_inter_empl || !g.vec_str_push || !g.str_assign || !g.wstr_assign ||
        !g.podbuf_resize || !g.cat_ctor || !g.cat_dtor || !g.vec_cat_push ||
        !g.serialize_cat || !g.bs_dtor || !g.ofstream_ctor || !g.str_dtor)
        return false;

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
    g.do_interstitials(iq);
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
    g.roster_rev = 1;
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
}

void house_pump() {
    if (!g.on) return;
    check_peer_departures();

    if (!g.is_client) {
        // Detect House scene liveness when host starts session already in House.
        const void* hscene = nullptr;
        bool suspended = false;
        bool has_house = leave_find_scene(kScene_House, &hscene, &suspended);
        if (has_house && !suspended && !g.in_pass_day && g.phase == HostPhase::Outside) {
            g.phase = HostPhase::House;
            if (g.generation == 0) g.generation = 1;
            rebuild_actor_map();
            log_line("HOUSE", "host detected live House scene (gen=%u, cats=%zu)",
                     g.generation, g.cat_actors.size());
            send_baseline(kNoPeer);
        }
        if (g.need_baseline && has_house && !suspended && !g.in_pass_day) {
            g.need_baseline = false;
            send_baseline(kNoPeer);
        }

        // Deferred checkpoint publishes once SQLite transaction commits.
        // For PassDay, EndSave has already committed before DoInterstitialsInternal,
        // so we can publish immediately (even while House is suspended for breeding).
        if (g.need_day_publish && has_house && !g.in_pass_day) {
            if (savefile_publish_active(kNoPeer, /*fresh=*/true, /*flush_adventure=*/false)) {
                g.need_day_publish = false;
                if (g.has_pending_interstitial_blob) {
                    HouseInterstitialsMsg im{};
                    im.generation = g.generation;
                    im.size       = (uint32_t)g.pending_interstitial_blob.size();
                    im.data       = g.pending_interstitial_blob.data();
                    net_send_house_interstitials(im);
                    g.has_pending_interstitial_blob = false;
                    g.pending_interstitial_blob.clear();
                }
                catsync_forget();
                catsync_publish("house day change");
                send_baseline(kNoPeer);
            }
        }
        if (g.need_ret_publish && has_house && !suspended) {
            if (savefile_publish_active(kNoPeer, /*fresh=*/true, /*flush_adventure=*/false)) {
                g.need_ret_publish = false;
                catsync_forget();
                catsync_publish("return to house");
                send_baseline(kNoPeer);
            }
        }
        if (g.need_adv_publish) {
            if (savefile_publish_active(kNoPeer, /*fresh=*/true, /*flush_adventure=*/true)) {
                g.need_adv_publish = false;
                HousePhaseMsg pm{ g.generation, (uint8_t)HostPhase::Adventure, 0 };
                net_send_house_phase(pm);
            }
        }

        host_sample_and_send_poses();
        host_poll_prep_state();
        send_cursor_sample();
    } else {
        // Client: reload mgmp_coop.sav in-place when a fresh House save arrives
        // (e.g. after PassDay or ReturnToHouse), so new kittens & removed cats match.
        if (g.reload_stage == 0 && savefile_has_pending()) {
            const void* ss = nullptr;
            bool ss_susp = false;
            const void* hs = nullptr;
            bool hs_susp = false;
            if (!leave_find_scene("SaveSelection", &ss, &ss_susp) &&
                leave_find_scene(kScene_House, &hs, &hs_susp) && hs) {
                void* old_md = nullptr;
                if (g.director_slot)
                    mem_read(g.director_slot, &old_md, sizeof(old_md));
                if (old_md && g.comp_destroy && g.create_entity && g.create_mew_dir && g.continue_save) {
                    void* dir = nullptr;
                    mem_read((const uint8_t*)old_md + kComp_Director, &dir, sizeof(dir));
                    if (dir) {
                        g.cached_director = dir;
                        *(uint8_t*)((uintptr_t)hs + kScene_Destroying) = 1;
                        g.comp_destroy(old_md);
                        g.cat_actors.clear();
                        g.client_tracks.clear();
                        g.current_house = nullptr;
                        g.current_scene = nullptr;
                        g.current_click_mgr = nullptr;
                        g.reload_stage = 1;
                        log_line("HOUSE", "reloading House from host save checkpoint");
                        return;
                    }
                }
            }
        } else if (g.reload_stage == 1) {
            if (savefile_write_pending_coop() && g.cached_director &&
                g.create_entity && g.create_mew_dir && g.continue_save) {
                void* shared = nullptr;
                mem_read((const uint8_t*)g.cached_director + kDirector_Shared, &shared, sizeof(shared));
                if (shared && !*(uint8_t*)((uintptr_t)shared + kScene_DoingDestruction)) {
                    void* ent = g.create_entity(shared);
                    uint8_t s_name[32];
                    memset(s_name, 0, sizeof(s_name));
                    const char* kName = "mgmp_coop.sav";
                    uint64_t len = 13, cap = 15;
                    memcpy(s_name, kName, len + 1);
                    memcpy(s_name + 16, &len, 8);
                    memcpy(s_name + 24, &cap, 8);
                    void* new_md = g.create_mew_dir(shared, ent, s_name);
                    g.continue_save(new_md);
                    g.reload_stage = 0;
                    log_line("HOUSE", "House checkpoint reloaded via MewDirector::ContinueSave");
                }
            }
            return;
        }

        // Client: once the new House scene is loaded, kick off any queued
        // night/breeding interstitials for this generation.
        if (g.has_queued_interstitial && g.queued_interstitial_gen == g.client_gen &&
            g.reload_stage == 0 && !savefile_has_pending()) {
            const void* hs = nullptr;
            bool hs_susp = false;
            if (leave_find_scene(kScene_House, &hs, &hs_susp) && hs) {
                *(uint8_t*)((uintptr_t)hs + kScene_Suspended) = 1;
                deserialize_and_run_interstitials(g.queued_interstitial_blob.data(),
                                                  (uint32_t)g.queued_interstitial_blob.size());
                g.has_queued_interstitial = false;
                g.queued_interstitial_blob.clear();
            }
        }

        // Client: notify host once Map scene is loaded after Prep -> Adventure.
        if (g.phase == HostPhase::Adventure) {
            const void* mscene = nullptr;
            bool msusp = false;
            if (leave_find_scene("Map", &mscene, &msusp) && !msusp) {
                static uint32_t s_sent_ready_gen = 0;
                if (s_sent_ready_gen != g.client_gen) {
                    s_sent_ready_gen = g.client_gen;
                    HousePhaseMsg pm{ g.client_gen, (uint8_t)HostPhase::Adventure, 1 };
                    net_send_house_phase(pm);
                }
            }
        }
        client_interpolate_cats();
        send_cursor_sample();
    }
}

bool house_catchup(uint8_t peer) {
    if (!g.on || g.is_client) return false;
    if (g.phase != HostPhase::House && g.phase != HostPhase::Prep) return false;

    if (!savefile_publish_active(peer, /*fresh=*/false, /*flush_adventure=*/false))
        log_line_lvl(LogLevel::Warn, "HOUSE",
                     "!! could not publish active House save to peer %u (tx in flight?)",
                     (unsigned)peer);

    catsync_forget();
    catsync_publish("house join catchup");
    send_baseline(peer);
    if (g.phase == HostPhase::Prep && g.have_prep)
        net_send_house_prep_to(peer, g.last_prep);
    return true;
}

void house_on_peer_disconnect(uint8_t peer) {
    if (peer < kMaxPeers) {
        g.peer_cursors[peer].valid = false;
        g.peer_map_ready[peer]     = false;
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
            if (!g.is_client) break;
            const auto& b = m.house_baseline;
            if (b.generation != g.client_gen) {
                g.client_gen = b.generation;
                clear_generation_state();
            }
            g.phase = (HostPhase)b.phase;
            g.queued_baseline_cats.clear();
            for (uint16_t i = 0; i < b.count; ++i)
                g.queued_baseline_cats.push_back(b.cats[i]);
            g.queued_baseline_gen = b.generation;
            g.has_queued_baseline = true;
            log_line("HOUSE", "<- BASELINE gen=%u cats=%u phase=%u",
                     b.generation, (unsigned)b.count, (unsigned)b.phase);
            break;
        }
        case MSG_HOUSE_POSE: {
            if (!g.is_client) break;
            const auto& hp = m.house_pose;
            if (hp.generation != g.client_gen) break;
            for (uint16_t i = 0; i < hp.count; ++i) {
                const auto& p = hp.cats[i];
                auto& tr = g.client_tracks[p.cat_id];
                if (tr.samples.size() > 16)
                    tr.samples.erase(tr.samples.begin());
                tr.samples.push_back(InterpSample{ hp.host_tick_ms, now, p });
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
            if (!g.is_client) break;
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
                                              tr.pending_release_wx, tr.pending_release_wy };
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
                update_capture_velocity(it->second, mv.wx, mv.wy, now);
                auto ait = g.cat_actors.find(mv.cat_id);
                if (ait != g.cat_actors.end() && ait->second) {
                    void* tr = nullptr;
                    if (mem_read((const uint8_t*)ait->second + kHCat_Transform, &tr, sizeof(tr)) && tr) {
                        Vec3D pos{ (double)mv.wx, (double)mv.wy, 0.0 };
                        memcpy((uint8_t*)tr + kTransform_Pos, &pos, sizeof(pos));
                    }
                }
            }
            break;
        }
        case MSG_HOUSE_DROP_REQ: {
            if (g.is_client) break;
            const auto& dr = m.house_drop_req;
            if (dr.generation != g.generation) break;
            auto it = g.host_captures.find(dr.cat_id);
            if (it == g.host_captures.end()) break;
            if (it->second.holder_peer != from || it->second.capture_id != dr.capture_id)
                break;
            host_release_capture(dr.cat_id, dr.wx, dr.wy, true);
            break;
        }
        case MSG_HOUSE_DROP_RESP: {
            if (!g.is_client) break;
            const auto& dr = m.house_drop_resp;
            if (dr.generation != g.client_gen) break;
            auto& tr = g.client_tracks[dr.cat_id];
            tr.holder_peer = kNoPeer;
            tr.capture_id  = 0;
            tr.predicted_held = false;
            if (g.client_held_cat == dr.cat_id) {
                g.client_held_cat    = 0;
                g.client_held_cap_id = 0;
            }
            HouseCatPose p = tr.has_last ? tr.last_applied : HouseCatPose{};
            p.cat_id = dr.cat_id;
            p.x      = dr.wx;
            p.y      = dr.wy;
            p.z      = 0.0f;
            strncpy_s(p.zone, dr.zone, _TRUNCATE);
            auto ait = g.cat_actors.find(dr.cat_id);
            if (ait != g.cat_actors.end())
                apply_pose_to_client_actor(ait->second, p, true);
            tr.samples.clear();
            tr.last_applied = p;
            tr.has_last     = true;
            break;
        }
        case MSG_HOUSE_CURSOR: {
            if (from >= kMaxPeers) break;
            const auto& c = m.house_cursor;
            uint32_t expected_gen = g.is_client ? g.client_gen : g.generation;
            if (c.generation != expected_gen) break;
            auto& pc = g.peer_cursors[from];
            pc.valid   = (c.in_house != 0);
            pc.gen     = c.generation;
            pc.wx      = c.wx;
            pc.wy      = c.wy;
            pc.nx      = c.nx;
            pc.ny      = c.ny;
            pc.mode    = c.mode;
            pc.recv_ms = now;
            break;
        }
        case MSG_HOUSE_PREP: {
            if (!g.is_client) break;
            g.last_prep = m.house_prep;
            g.have_prep = (m.house_prep.active != 0);
            break;
        }
        case MSG_HOUSE_PHASE: {
            const auto& pm = m.house_phase;
            if (!g.is_client) {
                if (pm.phase == (uint8_t)HostPhase::Adventure && (pm.flags & 1) && from < kMaxPeers) {
                    g.peer_map_ready[from] = true;
                    log_line("HOUSE", "peer %u map ready", (unsigned)from);
                }
            } else {
                g.phase = (HostPhase)pm.phase;
                if (g.phase == HostPhase::Prep) {
                    g.client_held_cat    = 0;
                    g.client_held_cap_id = 0;
                    g.client_tracks.clear();
                } else if (g.phase == HostPhase::Adventure) {
                    // Clear House actor references before adventure transition.
                    g.cat_actors.clear();
                    g.current_house = nullptr;
                    g.current_scene = nullptr;
                }
            }
            break;
        }
        case MSG_HOUSE_INTERSTITIALS: {
            if (!g.is_client) break;
            const auto& im = m.house_interstitials;
            if (im.data && im.size > 0) {
                g.queued_interstitial_blob.assign(im.data, im.data + im.size);
                g.queued_interstitial_gen = im.generation;
                g.has_queued_interstitial = true;
                log_line("HOUSE", "<- INTERSTITIALS gen=%u bytes=%u",
                         im.generation, im.size);
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
    if (!g.on || g.is_client) return true;
    uint8_t ids[kMaxPeers] = {};
    if (!net_peer_ids(ids, kMaxPeers)) return true;
    uint8_t n = net_peer_count();
    for (uint8_t i = 0; i < n; ++i) {
        if (ids[i] == kHostPeer) continue;
        if (!g.peer_map_ready[ids[i]]) return false;
    }
    return true;
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

bool house_get_peer_cursor(uint8_t peer, float* out_nx, float* out_ny,
                           uint8_t* out_mode, bool* out_active) {
    if (out_active) *out_active = false;
    if (!g.on || peer >= kMaxPeers) return false;
    if (g.phase != HostPhase::House && g.phase != HostPhase::Prep) return false;
    const auto& pc = g.peer_cursors[peer];
    if (!pc.valid) return false;
    if (now_ms() - pc.recv_ms > 2000) return false;
    float nx = pc.nx, ny = pc.ny;
    project_world_to_norm(pc.wx, pc.wy, &nx, &ny);
    if (out_nx) *out_nx = nx;
    if (out_ny) *out_ny = ny;
    if (out_mode) *out_mode = pc.mode;
    if (out_active) *out_active = true;
    return true;
}

// ---------------------------------------------------------------------------
// Game Hook Callbacks
// ---------------------------------------------------------------------------

void house_on_house_test_init(void* /*house_test*/, bool /*arg1*/, bool /*arg2*/) {
    if (!g.on) return;
    if (!g.is_client) {
        if (!g.in_pass_day) {
            advance_generation("HouseTest::init");
            g.need_baseline = true;
        }
        g.phase = HostPhase::House;
    } else {
        g.cat_actors.clear();
        g.current_house = nullptr;
        g.current_scene = nullptr;
        g.phase = HostPhase::House;
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

bool house_on_housecat_late_update(void* housecat) {
    if (!g.on) return true;
    if (g.is_client && housecat) {
        // Ensure client never triggers local cat-to-cat swats/overlaps.
        uint8_t zero = 0;
        memcpy((uint8_t*)housecat + kHCat_CheckOverlaps, &zero, 1);
    }
    return true;
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
        // Handle pending release-before-grant if LBUTTON was released while
        // waiting for the host's GRAB_RESP.
        if (g.client_pending_cat != 0 && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
            auto& tr = g.client_tracks[g.client_pending_cat];
            if (tr.predicted_held) {
                tr.predicted_held = false;
                tr.pending_release_before_grant = true;
                house_mouse_world(&tr.pending_release_wx, &tr.pending_release_wy);
            }
        }
        // If client currently has an active granted capture, send coalesced drag moves.
        if (g.client_held_cat != 0 && g.client_held_cap_id != 0) {
            uint32_t t = now_ms();
            float wx = 0.0f, wy = 0.0f;
            if (house_mouse_world(&wx, &wy) && (t - g.client_last_drag_ms >= tune::kHouseCursorMs)) {
                g.client_last_drag_ms = t;
                HouseDragMoveMsg mv{};
                mv.generation = g.client_gen;
                mv.cat_id     = g.client_held_cat;
                mv.capture_id = g.client_held_cap_id;
                mv.seq        = g.client_move_seq++;
                mv.wx         = wx;
                mv.wy         = wy;
                mv.flip       = 1;
                net_send_house_drag_move(mv);
            }
            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                HouseDropReqMsg dr{};
                dr.generation = g.client_gen;
                dr.cat_id     = g.client_held_cat;
                dr.capture_id = g.client_held_cap_id;
                dr.seq        = g.client_move_seq++;
                dr.wx         = wx;
                dr.wy         = wy;
                net_send_house_drop_req(dr);
                auto& tr = g.client_tracks[g.client_held_cat];
                tr.predicted_held    = false;
                g.client_held_cat    = 0;
                g.client_held_cap_id = 0;
            }
        }
    }
    return true;
}

void house_on_click_manager_update_post(void* click_mgr) {
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
                    // Cat is already held by a remote peer: reject host's native grab.
                    void* null_ptr = nullptr;
                    memcpy((uint8_t*)click_mgr + kHCatClick_HeldCat, &null_ptr, sizeof(null_ptr));
                }
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
    if (g.local_host_held != 0) {
        float wx = 0.0f, wy = 0.0f;
        house_mouse_world(&wx, &wy);
        host_release_capture(g.local_host_held, wx, wy, false);
    }
    return true;
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
    g.phase = HostPhase::Prep;
    HousePhaseMsg pm{ g.generation, (uint8_t)HostPhase::Prep, 0 };
    net_send_house_phase(pm);
    log_line("HOUSE", "host entered adventure preparation (gen=%u)", g.generation);
}

void house_on_go_on_adventure_cb_post() {
    if (!g.on || g.is_client) return;
    host_poll_prep_state();
}

void house_on_start_adventure_cb_post() {
    if (!g.on || g.is_client) return;
    g.phase = HostPhase::Adventure;
    memset(g.peer_map_ready, 0, sizeof(g.peer_map_ready));
    g.need_adv_publish = true;
    log_line("HOUSE", "host started adventure -- publishing map checkpoint");
}

void house_on_return_house_pre(bool success) {
    if (!g.on || g.is_client) return;
    advance_generation(success ? "ReturnToHouseSuccess" : "ReturnToHouseFail");
    g.phase = HostPhase::House;
    // Reset per-run follow state so the next adventure starts clean.
    follow_forget();
}

void house_on_return_house_post(bool /*success*/) {
    if (!g.on || g.is_client) return;
    g.need_ret_publish = true;
}

void house_on_return_cat_from_adv(void* /*house*/, int64_t cat_id) {
    if (!g.on || g.is_client) return;
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

} // namespace mgmp
