// mgmp_house.cpp -- House co-op coordinator: lifecycle, shared utilities,
// baseline/generation management, and message routing across mgmp_house_physics,
// mgmp_interstitials, mgmp_prep, and mgmp_reload.

#include "mgmp_house_internal.h"
#include "mgmp_catsync.h"
#include "mgmp_follow.h"
#include "mgmp_leave.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_resolve.h"
#include "mgmp_savefile.h"
#include "mgmp_tuning.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace mgmp {
namespace house_detail {

HouseState g;

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
    g.host_pipe_owner_peer     = kNoPeer;
    g.host_pipe_cat_id         = 0;
    g.host_pipe_comp           = nullptr;
    g.placing_pipe_peer        = kNoPeer;
    g.last_dropper_peer.clear();
    g.pipe_saved_slot          = nullptr;
    g.pipe_saved_prevcats      = 0;
    g.pipe_hidden_for_remote   = false;
    g.in_host_remote_donate    = false;
    g.client_pipe_cat_id       = 0;
    g.client_pipe_donated      = false;
    g.client_pipe_open_pending = false;
    g.in_client_donate_cat     = false;
    g.client_donate_npc        = -1;
    g.client_pipe_saved_held   = nullptr;
    g.client_pipe_spoofed_held = false;
    for (int i = 0; i < kMaxPeers; ++i)
        g.peer_cursors[i].valid = false;
}

void advance_generation(const char* why) {
    ++g.generation;
    ++g.roster_rev;
    clear_generation_state();
    log_line("HOUSE", "generation -> %u (%s)", g.generation, why ? why : "");
}

bool live_house_scene_valid() {
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

void send_baseline(uint8_t target_peer, bool with_cat_records, bool returning) {
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

namespace {

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

} // namespace
} // namespace house_detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void house_init() {
    auto& g = house_detail::g;
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
    house_detail::clear_generation_state();
    memset(g.peer_map_ready, 0, sizeof(g.peer_map_ready));
    g.known_peer_count = 0;
    if (g.on)
        log_line("HOUSE", "armed (%s)", g.is_client ? "client" : "host");
}

void house_shutdown() {
    auto& g = house_detail::g;
    house_detail::clear_generation_state();
    g.on    = false;
    g.phase = HostPhase::Outside;
}

void house_set_base(uintptr_t base) {
    using namespace house_detail;
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
    g.cat_run_away       = (FnVoidPtr)addr_of_call(C_HouseCatRunAway);
    g.drawer_do_on_open  = (FnVoidPtr)addr_of_call(C_HouseDrawerDoOnOpen);
    g.drawer_do_on_close = (FnVoidPtr)addr_of_call(C_HouseDrawerDoOnClose);
    g.drawer_refresh_nav = (FnVoidPtr)addr_of_call(C_HouseDrawerRefreshNav);
    g.drawer_close       = (FnVoidPtr)addr_of_call(C_HouseDrawerClose);
}

void house_pump() {
    using namespace house_detail;
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
        prep_host_pump(has_house, suspended);
        if (g.need_baseline && has_house && !suspended && !g.in_pass_day &&
            !g.need_day_publish && !g.need_ret_publish && !g.in_return) {
            g.need_baseline = false;
            send_baseline(kNoPeer, true);
        }

        // Deferred checkpoint publishes once SQLite transaction commits.
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
        prep_client_pump();
        if (reload_client_pump()) return;
        interstitials_client_pump();
        client_interpolate_cats();
        pipe_client_pump();
        send_cursor_sample();
    }
}

void house_on_peer_disconnect(uint8_t peer) {
    using namespace house_detail;
    if (peer < kMaxPeers) {
        g.peer_cursors[peer].valid = false;
        g.peer_map_ready[peer] = false;
        g.peer_needs_checkpoint[peer] = false;
        g.peer_map_deadline[peer] = 0;
    }
    if (g.is_client || !g.on) return;

    std::vector<int64_t> drop_ids;
    for (auto& kv : g.host_captures) {
        if (kv.second.holder_peer == peer)
            drop_ids.push_back(kv.first);
    }
    for (int64_t id : drop_ids) {
        auto it = g.host_captures.find(id);
        if (it != g.host_captures.end()) {
            log_line("HOUSE", "peer %u disconnected -- releasing cat %lld",
                     (unsigned)peer, (long long)id);
            host_release_capture(id, it->second.wx, it->second.wy, true);
        }
    }
    pipe_on_peer_disconnect(peer);
}

void house_on_message(uint8_t from, const NetMsg& m) {
    using namespace house_detail;
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
        case MSG_HOUSE_POSE:
        case MSG_HOUSE_GRAB_REQ:
        case MSG_HOUSE_GRAB_RESP:
        case MSG_HOUSE_DRAG_MOVE:
        case MSG_HOUSE_DROP_REQ:
        case MSG_HOUSE_DROP_RESP:
        case MSG_HOUSE_CURSOR:
            physics_on_message(from, m, now);
            break;
        case MSG_HOUSE_PREP:
            prep_on_message(from, m);
            break;
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
        case MSG_HOUSE_INTERSTITIALS:
        case MSG_HOUSE_POOP_POP:
        case MSG_HOUSE_FURNITURE:
            interstitials_on_message(from, m);
            break;
        case MSG_HOUSE_PIPE:
            pipe_on_message(from, m);
            break;
        default:
            break;
    }
}

HostPhase house_phase()      { return house_detail::g.phase; }
uint32_t  house_generation() {
    return house_detail::g.is_client ? house_detail::g.client_gen : house_detail::g.generation;
}
bool house_in_house_or_prep() {
    auto& g = house_detail::g;
    return g.on && (g.phase == HostPhase::House || g.phase == HostPhase::Prep);
}

bool house_clients_map_ready() {
    auto& g = house_detail::g;
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

void house_on_first_node() {
    house_detail::g.waiting_first_node = false;
    house_detail::g.need_adv_publish = false;
}

void house_status(char* out, size_t out_size) {
    auto& g = house_detail::g;
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

void house_on_house_test_init(void* /*house_test*/, bool /*arg1*/, bool /*arg2*/) {
    auto& g = house_detail::g;
    if (!g.on) return;
    if (!g.is_client) {
        if (!g.in_pass_day && !g.in_return && !g.need_ret_publish) {
            house_detail::advance_generation("HouseTest::init");
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

} // namespace mgmp
