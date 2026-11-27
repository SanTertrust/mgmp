// mgmp_house_physics.cpp -- HouseCat pose/interpolation, drag ownership,
// world-space peer cursors, and HouseCat/ClickManager/Pipe hooks.

#include "mgmp_house_internal.h"
#include "mgmp_leave.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_overlay.h"
#include "mgmp_resolve.h"
#include "mgmp_savefile.h"
#include "mgmp_tuning.h"
#include "mgmp_ui.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace mgmp {
namespace house_detail {
namespace {

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

bool is_house_pipe(const void* area) {
    if (!area) return false;
    const auto* vtable = *static_cast<const uintptr_t* const*>(area);
    return vtable[kCPA_TryPlaceSlot] == addr_of(T_HousePipePlace);
}

struct RoomCats { uint32_t capacity, count; void** data; };

RoomCats* room_cats(void* area) {
    return reinterpret_cast<RoomCats*>(static_cast<uint8_t*>(area) + kCPA_OwnedCats);
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

} // namespace

Vec2D cat_drag_anchor(const void* cat, bool flipped) {
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
                g.last_dropper_peer[cat_id] = cap.holder_peer;
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
            } else {
                g.last_dropper_peer.erase(cat_id);
            }
            if (g.current_house && g.loc_under_point) {
                // Native drop measures the click bounds without visual mirroring.
                const auto held_anchor = cat_drag_anchor(hc);
                const auto drop_anchor = cat_drag_anchor(hc, false);
                Vec2D pt{wx - held_anchor.x + drop_anchor.x, wy - held_anchor.y + drop_anchor.y};
                void* area = g.loc_under_point(g.current_house, &pt);
                if (area && g.place_cat) {
                    if (is_house_pipe(area) && cap.holder_peer != kHostPeer)
                        g.placing_pipe_peer = cap.holder_peer;
                    g.place_cat(area, hc);
                    g.placing_pipe_peer = kNoPeer;
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
    cap.wx          = wx;
    cap.wy          = wy;
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
    if (peer == kHostPeer) {
        g.local_host_held = cat_id;
        g.last_dropper_peer.erase(cat_id);
    } else {
        g.last_dropper_peer[cat_id] = peer;
    }
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

void physics_on_message(uint8_t from, const NetMsg& m, uint32_t now) {
    switch (m.type) {
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
        default:
            break;
    }
}

namespace {

void* current_global_prog() {
    const uint8_t* director = nullptr;
    void* prog = nullptr;
    if (g.director_slot && mem_read(g.director_slot, &director, sizeof(director)) && director)
        mem_read(director + kDir_GlobalProg, &prog, sizeof(prog));
    return prog;
}

bool read_npc_progress_snapshot(NpcProgressEntry out[8]) {
    void* prog = current_global_prog();
    if (!prog) return false;
    const uint8_t* base = static_cast<const uint8_t*>(prog) + kGProg_NpcProgress;
    for (int i = 0; i < 8; ++i) {
        const uint8_t* e = base + (size_t)i * kNpcProg_Stride;
        mem_read(e + kNpcProg_GoalFavor,     &out[i].goal_favor,            sizeof(int32_t));
        mem_read(e + kNpcProg_GoalComplete,  &out[i].current_goal_complete, 1);
        mem_read(e + kNpcProg_Unlocked,      &out[i].unlocked,              1);
        mem_read(e + kNpcProg_AcceptingCats, &out[i].accepting_cats,        1);
    }
    return true;
}

void apply_npc_progress_snapshot(const NpcProgressEntry in[8]) {
    void* prog = current_global_prog();
    if (!prog) return;
    uint8_t* base = static_cast<uint8_t*>(prog) + kGProg_NpcProgress;
    for (int i = 0; i < 8; ++i) {
        uint8_t* e = base + (size_t)i * kNpcProg_Stride;
        memcpy(e + kNpcProg_GoalFavor,     &in[i].goal_favor,            sizeof(int32_t));
        memcpy(e + kNpcProg_GoalComplete,  &in[i].current_goal_complete, 1);
        memcpy(e + kNpcProg_Unlocked,      &in[i].unlocked,              1);
        memcpy(e + kNpcProg_AcceptingCats, &in[i].accepting_cats,        1);
    }
}

void* find_house_component(int comp_id) {
    if (!live_house_scene_valid()) return nullptr;
    void** data = nullptr;
    uint32_t count = 0;
    if (get_cached_list(g.current_scene, comp_id, &data, &count) && count > 0 && data)
        return data[0];
    return nullptr;
}

void close_client_npc_drawer() {
    void* drawer = find_house_component(kCompId_NPCMapDrawer);
    if (!drawer) return;
    void* panel = nullptr;
    mem_read(static_cast<const uint8_t*>(drawer) + kNpcDrawer_Panel, &panel, sizeof(panel));
    if (panel && g.drawer_close) g.drawer_close(panel);
    uint8_t zero = 0;
    void* nullp = nullptr;
    uint64_t zero64 = 0;
    memcpy(static_cast<uint8_t*>(drawer) + kNpcDrawer_HasCatToDonate, &zero, 1);
    memcpy(static_cast<uint8_t*>(drawer) + kNpcDrawer_InDonateMode,   &zero, 1);
    memcpy(static_cast<uint8_t*>(drawer) + kNpcDrawer_DonatingCatPtr, &nullp, sizeof(nullp));
    memcpy(static_cast<uint8_t*>(drawer) + kNpcDrawer_DonatingCatId,  &zero64, sizeof(zero64));
}

} // namespace

void pipe_host_broadcast_progress(uint8_t target_peer) {
    if (g.is_client || !net_active()) return;
    HousePipeMsg msg{};
    msg.generation = g.generation;
    msg.action     = (uint8_t)PipeAction::SyncProgress;
    msg.has_prog   = read_npc_progress_snapshot(msg.prog) ? 1 : 0;
    if (!msg.has_prog) return;
    if (target_peer == kNoPeer) net_send_house_pipe(msg);
    else                        net_send_house_pipe_to(target_peer, msg);
}

void pipe_host_abort_remote() {
    if (g.is_client || g.host_pipe_owner_peer == kNoPeer) return;
    const uint8_t owner = g.host_pipe_owner_peer;
    const int64_t cat_id = g.host_pipe_cat_id;
    g.host_pipe_owner_peer = kNoPeer;
    g.host_pipe_cat_id     = 0;
    g.host_pipe_comp       = nullptr;
    HousePipeMsg msg{};
    msg.generation = g.generation;
    msg.action     = (uint8_t)PipeAction::Close;
    msg.cat_id     = cat_id;
    net_send_house_pipe_to(owner, msg);
}

void pipe_on_peer_disconnect(uint8_t peer) {
    if (g.is_client || g.host_pipe_owner_peer != peer) return;
    log_line("HOUSE", "pipe owner peer %u disconnected -- releasing cat %lld for ejection",
             (unsigned)peer, (long long)g.host_pipe_cat_id);
    g.host_pipe_owner_peer = kNoPeer;
    g.host_pipe_cat_id     = 0;
    g.host_pipe_comp       = nullptr;
}

void pipe_on_message(uint8_t from, const NetMsg& m) {
    const auto& pm = m.house_pipe;
    const auto act = static_cast<PipeAction>(pm.action);

    if (g.is_client) {
        if (from != kHostPeer) return;
        if (pm.generation != g.client_gen && g.client_gen != 0) return;
        if (pm.has_prog) apply_npc_progress_snapshot(pm.prog);

        if (act == PipeAction::Open) {
            g.client_pipe_cat_id       = pm.cat_id;
            g.client_pipe_donated      = false;
            g.client_pipe_open_pending = true;
            log_line("HOUSE", "<- PIPE OPEN cat=%lld", (long long)pm.cat_id);
        } else if (act == PipeAction::Close) {
            if (g.client_pipe_cat_id != 0 || g.client_pipe_open_pending) {
                g.client_pipe_cat_id       = 0;
                g.client_pipe_donated      = false;
                g.client_pipe_open_pending = false;
                close_client_npc_drawer();
            }
        }
        return;
    }

    // Host handling client pipe actions.
    if (g.phase != HostPhase::House || pm.generation != g.generation) return;
    if (from != g.host_pipe_owner_peer || pm.cat_id != g.host_pipe_cat_id) return;

    if (act == PipeAction::Cancel) {
        log_line("HOUSE", "<- PIPE CANCEL peer=%u cat=%lld (ejecting)",
                 (unsigned)from, (long long)pm.cat_id);
        g.host_pipe_owner_peer = kNoPeer;
        g.host_pipe_cat_id     = 0;
        g.host_pipe_comp       = nullptr;
        return;
    }

    if (act == PipeAction::Donate && pm.npc <= 7) {
        rebuild_actor_map();
        auto ait = g.cat_actors.find(pm.cat_id);
        const void* registry = current_cat_registry();
        void* prog = current_global_prog();
        void* hc = (ait != g.cat_actors.end()) ? ait->second : nullptr;
        void* cat = (registry && g.cat_by_id) ? g.cat_by_id(registry, pm.cat_id) : nullptr;

        g.host_pipe_owner_peer = kNoPeer;
        g.host_pipe_cat_id     = 0;
        g.host_pipe_comp       = nullptr;

        if (!hc || !cat || !prog || !g.gain_favor_orig) {
            log_line("HOUSE", "!! PIPE DONATE peer=%u cat=%lld npc=%u missing live state",
                     (unsigned)from, (long long)pm.cat_id, (unsigned)pm.npc);
            return;
        }

        log_line("HOUSE", "<- PIPE DONATE peer=%u cat=%lld npc=%u",
                 (unsigned)from, (long long)pm.cat_id, (unsigned)pm.npc);

        g.in_host_remote_donate = true;
        g.gain_favor_orig(prog, (int)pm.npc, cat, 1);
        g.in_host_remote_donate = false;

        if (pm.npc == 7) {
            if (g.cat_run_away) g.cat_run_away(hc);
        } else {
            static constexpr uint64_t kNpcDonateFlags[7] = {
                0x801ULL,   // 0: Beanies
                0x1001ULL,  // 1: Butch
                0x2001ULL,  // 2: Tink
                0x10001ULL, // 3: Frank
                0x20001ULL, // 4: Jack
                0x4001ULL,  // 5: Tracy
                0x8001ULL,  // 6: OrganGrinder
            };
            uint64_t flags = 0;
            mem_read(static_cast<const uint8_t*>(cat) + kCatData_Flags, &flags, sizeof(flags));
            flags |= kNpcDonateFlags[pm.npc];
            memcpy(static_cast<uint8_t*>(cat) + kCatData_Flags, &flags, sizeof(flags));

            uint8_t zero = 0;
            memcpy(static_cast<uint8_t*>(hc) + kHCat_Exists, &zero, 1);
            void* loc = nullptr;
            mem_read(static_cast<const uint8_t*>(hc) + kHCat_CurrentLocation, &loc, sizeof(loc));
            if (loc && g.remove_cat) g.remove_cat(loc, hc);
            if (g.comp_destroy) g.comp_destroy(hc);
        }

        // Persist the host save without forcing a full House reload on clients.
        const uint8_t* director = nullptr;
        if (g.autosave_house && g.director_slot &&
            mem_read(g.director_slot, &director, sizeof(director)) && director) {
            g.suppress_autosave_publish = true;
            g.autosave_house(const_cast<uint8_t*>(director));
            g.suppress_autosave_publish = false;
        }
        pipe_host_broadcast_progress(kNoPeer);
    }
}

void pipe_client_pump() {
    if (!g.is_client || g.phase != HostPhase::House || g.reload_stage || savefile_has_pending())
        return;
    if (!g.client_pipe_open_pending && g.client_pipe_cat_id == 0)
        return;
    if (!live_house_scene_valid()) return;

    void* drawer = find_house_component(kCompId_NPCMapDrawer);
    void* drawer_ui = find_house_component(kCompId_HouseDrawerUI);
    if (!drawer || !drawer_ui) return;

    void* panel = nullptr;
    if (!mem_read(static_cast<const uint8_t*>(drawer) + kNpcDrawer_Panel, &panel, sizeof(panel)) || !panel)
        return;

    if (g.client_pipe_open_pending) {
        if (!g.drawer_do_on_open || !g.drawer_do_on_close || !g.drawer_refresh_nav) return;
        auto ait = g.cat_actors.find(g.client_pipe_cat_id);
        if (ait == g.cat_actors.end() || !ait->second) return;
        void* hc = ait->second;

        void* cur = nullptr;
        mem_read(static_cast<const uint8_t*>(drawer_ui) + kDrawerUI_CurrentPanel, &cur, sizeof(cur));
        if (cur) {
            g.drawer_do_on_close(cur);
            void* nullp = nullptr;
            memcpy(static_cast<uint8_t*>(drawer_ui) + kDrawerUI_CurrentPanel, &nullp, sizeof(nullp));
        }

        uint64_t safe_id = 0;
        mem_read(static_cast<const uint8_t*>(hc) - 8, &safe_id, sizeof(safe_id));
        uint8_t one = 1, zero = 0;
        memcpy(static_cast<uint8_t*>(drawer) + kNpcDrawer_DonatingCatPtr,  &hc,      sizeof(hc));
        memcpy(static_cast<uint8_t*>(drawer) + kNpcDrawer_DonatingCatId,   &safe_id, sizeof(safe_id));
        memcpy(static_cast<uint8_t*>(drawer) + kNpcDrawer_HasCatToDonate,  &one,     1);
        memcpy(static_cast<uint8_t*>(drawer) + kNpcDrawer_CatStatsWasOpen, &zero,    1);

        memcpy(static_cast<uint8_t*>(drawer_ui) + kDrawerUI_CurrentPanel, &panel, sizeof(panel));
        g.drawer_refresh_nav(drawer_ui);
        g.drawer_do_on_open(panel);
        g.client_pipe_open_pending = false;
        return;
    }

    if (g.client_pipe_cat_id != 0 && !g.client_pipe_donated) {
        void* cur = nullptr;
        mem_read(static_cast<const uint8_t*>(drawer_ui) + kDrawerUI_CurrentPanel, &cur, sizeof(cur));
        if (cur != panel) {
            HousePipeMsg msg{};
            msg.generation = g.client_gen;
            msg.action     = (uint8_t)PipeAction::Cancel;
            msg.cat_id     = g.client_pipe_cat_id;
            net_send_house_pipe(msg);
            log_line("HOUSE", "-> PIPE CANCEL cat=%lld", (long long)g.client_pipe_cat_id);
            g.client_pipe_cat_id = 0;

            uint8_t zero = 0;
            void* nullp = nullptr;
            uint64_t zero64 = 0;
            memcpy(static_cast<uint8_t*>(drawer) + kNpcDrawer_HasCatToDonate, &zero, 1);
            memcpy(static_cast<uint8_t*>(drawer) + kNpcDrawer_InDonateMode,   &zero, 1);
            memcpy(static_cast<uint8_t*>(drawer) + kNpcDrawer_DonatingCatPtr, &nullp, sizeof(nullp));
            memcpy(static_cast<uint8_t*>(drawer) + kNpcDrawer_DonatingCatId,  &zero64, sizeof(zero64));
        }
    }
}

} // namespace house_detail

bool house_get_peer_cursor(uint8_t peer, float* out_nx, float* out_ny,
                           uint8_t* out_mode, bool* out_active) {
    auto& g = house_detail::g;
    if (out_active) *out_active = false;
    if (!g.on || peer >= kMaxPeers) return false;
    if (g.phase != HostPhase::House && g.phase != HostPhase::Prep) return false;
    const auto& pc = g.peer_cursors[peer];
    if (!pc.valid) return false;
    if (house_detail::now_ms() - pc.recv_ms > 2000) return false;
    if (pc.in_house == 2) {
        // Prep screen-space cursor (ClassChooser / StorageItems / ActSelection).
        if (out_nx) *out_nx = pc.nx;
        if (out_ny) *out_ny = pc.ny;
        if (out_mode) *out_mode = pc.mode;
        if (out_active) *out_active = true;
        return true;
    }
    if (!house_detail::live_house_scene_valid()) return false;
    const uint32_t span = pc.recv_ms - pc.prev_ms;
    const uint32_t delay = std::min(span, tune::kHouseInterpDelayMs);
    const uint32_t target = house_detail::now_ms() - delay;
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
                    const auto anchor = house_detail::cat_drag_anchor(actor->second);
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
    house_detail::project_world_to_norm(wx, wy, &nx, &ny);
    if (out_nx) *out_nx = nx;
    if (out_ny) *out_ny = ny;
    if (out_mode) *out_mode = pc.mode;
    if (out_active) *out_active = true;
    return true;
}

bool house_local_mouse_world(float* out_wx, float* out_wy) {
    auto& g = house_detail::g;
    if (!g.on || g.phase != HostPhase::House || !house_detail::live_house_scene_valid()) return false;
    return house_detail::house_mouse_world(out_wx, out_wy);
}

bool house_project_world(float wx, float wy, float* out_nx, float* out_ny) {
    auto& g = house_detail::g;
    if (!g.on || g.phase != HostPhase::House || !house_detail::live_house_scene_valid()) return false;
    return house_detail::project_world_to_norm(wx, wy, out_nx, out_ny);
}

bool house_allows_special_placement(void* cat) {
    auto& g = house_detail::g;
    return !g.on || (!g.is_client && !house_detail::is_remote_captured_cat(cat));
}

bool house_allows_pipe_placement(void* cat) {
    auto& g = house_detail::g;
    if (!house_allows_special_placement(cat)) return false;
    if (g.on && !g.is_client && g.host_pipe_owner_peer != kNoPeer)
        return false;
    return true;
}

void house_on_pipe_placed(void* pipe, void* cat) {
    auto& g = house_detail::g;
    if (!g.on || g.is_client || !pipe || !cat || g.phase != HostPhase::House) return;
    int64_t cat_id = 0;
    if (!mem_read(static_cast<const uint8_t*>(cat) + kHCat_CatId, &cat_id, sizeof(cat_id)) || !cat_id)
        return;

    uint8_t owner = kHostPeer;
    if (g.placing_pipe_peer != kNoPeer) {
        owner = g.placing_pipe_peer;
    } else {
        auto it = g.last_dropper_peer.find(cat_id);
        if (it != g.last_dropper_peer.end()) owner = it->second;
    }
    if (owner == kHostPeer) return;

    g.host_pipe_owner_peer = owner;
    g.host_pipe_cat_id     = cat_id;
    g.host_pipe_comp       = pipe;

    // Mark prevcats=1 so if the client cancels, HousePipe::update natively
    // ejects the cat without playing pipe_down or locking host input.
    int32_t one = 1;
    memcpy(static_cast<uint8_t*>(pipe) + kHPipe_PrevCats, &one, sizeof(one));

    HousePipeMsg msg{};
    msg.generation = g.generation;
    msg.action     = (uint8_t)PipeAction::Open;
    msg.cat_id     = cat_id;
    msg.has_prog   = house_detail::read_npc_progress_snapshot(msg.prog) ? 1 : 0;
    net_send_house_pipe_to(owner, msg);
    log_line("HOUSE", "-> PIPE OPEN peer=%u cat=%lld", (unsigned)owner, (long long)cat_id);
}

void house_on_pipe_update_pre(void* pipe) {
    auto& g = house_detail::g;
    if (!g.on || !pipe || g.phase != HostPhase::House) return;

    if (g.is_client) {
        // Spoof ClickManager::held_cat so HousePipe::update renders floating
        // FavorsCat NPC icons while the client is dragging a cat.
        int64_t held_id = g.client_held_cat ? g.client_held_cat : g.client_pending_cat;
        if (held_id != 0 && g.current_click_mgr) {
            auto ait = g.cat_actors.find(held_id);
            if (ait != g.cat_actors.end() && ait->second) {
                void* hc = ait->second;
                mem_read(static_cast<const uint8_t*>(g.current_click_mgr) + kHCatClick_HeldCat,
                         &g.client_pipe_saved_held, sizeof(g.client_pipe_saved_held));
                memcpy(static_cast<uint8_t*>(g.current_click_mgr) + kHCatClick_HeldCat,
                       &hc, sizeof(hc));
                g.client_pipe_spoofed_held = true;
            }
        }
        return;
    }

    // Host: while a remote client owns the pipe, hide slots[0] during update so
    // the host never locks input, opens NPCMapDrawer, or prematurely ejects it.
    if (g.host_pipe_owner_peer != kNoPeer && g.host_pipe_owner_peer != kHostPeer) {
        void** slots_data = nullptr;
        if (mem_read(static_cast<const uint8_t*>(pipe) + kHPipe_SlotsData, &slots_data, sizeof(slots_data)) &&
            slots_data && *slots_data) {
            g.pipe_saved_slot = *slots_data;
            mem_read(static_cast<const uint8_t*>(pipe) + kHPipe_PrevCats,
                     &g.pipe_saved_prevcats, sizeof(g.pipe_saved_prevcats));
            *slots_data = nullptr;
            int32_t zero = 0;
            memcpy(static_cast<uint8_t*>(pipe) + kHPipe_PrevCats, &zero, sizeof(zero));
            g.pipe_hidden_for_remote = true;
        }
    }
}

void house_on_pipe_update_post(void* pipe) {
    auto& g = house_detail::g;
    if (g.client_pipe_spoofed_held && g.current_click_mgr) {
        memcpy(static_cast<uint8_t*>(g.current_click_mgr) + kHCatClick_HeldCat,
               &g.client_pipe_saved_held, sizeof(g.client_pipe_saved_held));
        g.client_pipe_saved_held   = nullptr;
        g.client_pipe_spoofed_held = false;
    }
    if (g.pipe_hidden_for_remote && pipe) {
        void** slots_data = nullptr;
        if (mem_read(static_cast<const uint8_t*>(pipe) + kHPipe_SlotsData, &slots_data, sizeof(slots_data)) &&
            slots_data) {
            *slots_data = g.pipe_saved_slot;
        }
        memcpy(static_cast<uint8_t*>(pipe) + kHPipe_PrevCats,
               &g.pipe_saved_prevcats, sizeof(g.pipe_saved_prevcats));
        g.pipe_saved_slot        = nullptr;
        g.pipe_hidden_for_remote = false;
    }
}

void house_on_npc_donate_cat_pre(void* /*drawer*/) {
    auto& g = house_detail::g;
    if (!g.on || !g.is_client || g.client_pipe_cat_id == 0) return;
    g.in_client_donate_cat = true;
    g.client_donate_npc    = -1;
}

void house_on_npc_donate_cat_post(void* drawer) {
    auto& g = house_detail::g;
    if (!g.in_client_donate_cat) return;
    g.in_client_donate_cat = false;

    // Prevent client NPCMapDrawer::update from transitioning into NPCOffice.
    if (drawer) {
        int32_t none = 8;
        memcpy(static_cast<uint8_t*>(drawer) + kNpcDrawer_LastDonatedNpc, &none, sizeof(none));
    }

    if (g.client_donate_npc >= 0 && g.client_donate_npc <= 7 && g.client_pipe_cat_id != 0) {
        HousePipeMsg msg{};
        msg.generation = g.client_gen;
        msg.action     = (uint8_t)PipeAction::Donate;
        msg.npc        = (uint8_t)g.client_donate_npc;
        msg.cat_id     = g.client_pipe_cat_id;
        net_send_house_pipe(msg);
        log_line("HOUSE", "-> PIPE DONATE cat=%lld npc=%d",
                 (long long)g.client_pipe_cat_id, g.client_donate_npc);
        g.client_pipe_donated = true;
        g.client_pipe_cat_id  = 0;
    }
    g.client_donate_npc = -1;
}

void house_set_gain_favor_orig(void* fn) {
    house_detail::g.gain_favor_orig = (house_detail::FnGainFavor)fn;
}

bool house_on_gain_favor(void* /*prog*/, int npc, void* /*cat*/, int /*amount*/) {
    auto& g = house_detail::g;
    if (g.on && g.is_client && g.in_client_donate_cat) {
        g.client_donate_npc = npc;
        return false; // Host executes GainFavor authoritatively and syncs back.
    }
    return true;
}

void house_on_gain_favor_post(void* /*prog*/, int /*npc*/, void* /*cat*/, int /*amount*/) {
    auto& g = house_detail::g;
    if (g.on && !g.is_client && !g.in_host_remote_donate && g.phase == HostPhase::House)
        house_detail::pipe_host_broadcast_progress(kNoPeer);
}

bool house_on_housecat_update(void* housecat) {
    auto& g = house_detail::g;
    if (!g.on) return true;
    // Clients do not run local HouseCat AI/brain simulation; Animator + Renderer
    // still run natively from Scene's update/render lists.
    if (g.is_client) return false;
    house_detail::spoof_remote_held_begin(housecat);
    return true;
}

void house_on_housecat_update_post(void* /*housecat*/) {
    house_detail::spoof_remote_held_end();
}

bool house_on_housecat_late_update(void* /*housecat*/) {
    // Native late_update re-enables renderer visibility and can trigger swats.
    auto& g = house_detail::g;
    return !g.on || !g.is_client;
}

bool house_on_housecat_physics_update(void* physics) {
    auto& g = house_detail::g;
    if (!g.on) return true;
    if (g.is_client) return false;

    // On host, spoof ClickManager::held_cat for remotely-held cats so native
    // HouseCatPhysics zeroes velocity and tracks prev_dragging/prev_pos.
    if (physics) {
        void* hc = nullptr;
        if (mem_read((const uint8_t*)physics + kHCatPhys_Cat, &hc, sizeof(hc)) && hc) {
            if (house_detail::is_remote_captured_cat(hc)) {
                if (g.current_click_mgr) {
                    house_detail::spoof_remote_held_begin(hc);
                    return true;
                }
                house_detail::Vec2D zero{ 0.0, 0.0 };
                memcpy((uint8_t*)physics + kHCatPhys_Velocity, &zero, sizeof(zero));
                return false;
            }
        }
    }
    return true;
}

void house_on_housecat_physics_update_post(void* /*physics*/) {
    house_detail::spoof_remote_held_end();
}

bool house_on_click_manager_update(void* click_mgr) {
    auto& g = house_detail::g;
    if (!g.on || !click_mgr) return true;
    g.current_click_mgr = click_mgr;
    if (g.phase == HostPhase::Prep)
        return false;

    if (g.is_client) {
        const uint32_t t = house_detail::now_ms();
        HWND fg = GetForegroundWindow();
        DWORD fg_pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fg_pid);
        const bool focused = (fg != nullptr && fg_pid == GetCurrentProcessId());
        const bool left_down = focused && !ui_captures_input() &&
                               ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        // Handle pending release-before-grant if LBUTTON was released while
        // waiting for the host's GRAB_RESP.
        if (g.client_pending_cat != 0) {
            auto& tr = g.client_tracks[g.client_pending_cat];
            if (tr.predicted_held) {
                house_detail::sample_client_drag(tr, t);
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
            house_detail::sample_client_drag(tr, t);
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
    auto& g = house_detail::g;
    g.in_native_client_pickup = false;
    if (!g.on || !click_mgr || g.phase != HostPhase::House) return;
    void* held = nullptr;
    mem_read((const uint8_t*)click_mgr + kHCatClick_HeldCat, &held, sizeof(held));

    if (!g.is_client) {
        if (held && g.local_host_held == 0) {
            int64_t id = 0;
            if (mem_read((const uint8_t*)held + kHCat_CatId, &id, sizeof(id)) && id != 0) {
                float wx = 0.0f, wy = 0.0f;
                house_detail::house_mouse_world(&wx, &wy);
                if (!house_detail::host_try_grant_grab(kHostPeer, id, 0, wx, wy)) {
                    // Undo the native pickup on a cat already controlled by a peer.
                    auto capture = g.host_captures.find(id);
                    if (capture != g.host_captures.end()) {
                        house_detail::move_held_cat(held, capture->second.wx, capture->second.wy);
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
                house_detail::house_mouse_world(&capture->second.wx, &capture->second.wy);
            }
        } else if (!held && g.local_host_held != 0) {
            float wx = 0.0f, wy = 0.0f;
            house_detail::house_mouse_world(&wx, &wy);
            house_detail::host_release_capture(g.local_host_held, wx, wy, false);
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
                    house_detail::house_mouse_world(&wx, &wy);
                    uint32_t req_seq = g.client_req_seq++;
                    tr.predicted_held               = true;
                    tr.pending_req_seq              = req_seq;
                    tr.pending_release_before_grant = false;
                    tr.pred_wx                      = wx;
                    tr.pred_wy                      = wy;
                    tr.throw_velocity               = {};
                    tr.last_motion_ms               = house_detail::now_ms();
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
    auto& g = house_detail::g;
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
    auto& g = house_detail::g;
    if (g.on && !g.is_client && g.local_host_held != 0)
        house_detail::host_release_capture(g.local_host_held, 0.0f, 0.0f, false);
}

bool house_on_room_remove(void* area, void* cat) {
    auto& g = house_detail::g;
    if (!g.on || !g.is_client || !g.in_native_client_pickup) return true;
    if (area && cat) house_detail::remove_client_room_member(area, cat);
    return false;
}

bool house_allows_local_mutations() {
    auto& g = house_detail::g;
    return !g.on || !g.is_client;
}

} // namespace mgmp
