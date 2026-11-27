#pragma once
// mgmp_house_internal.h -- shared state and internal helpers across the split
// House co-op modules (mgmp_house, mgmp_house_physics, mgmp_interstitials,
// mgmp_prep, mgmp_reload).

#include "mgmp_house.h"
#include "mgmp_addresses.h"
#include "mgmp_proto.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mgmp {
namespace house_detail {

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
using FnCatById               = void*(__fastcall*)(const void*, int64_t);
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
using FnGainFavor             = void(__fastcall*)(void* prog, int npc, void* cat, int amount);

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
    FnVoidPtr               cat_run_away    = nullptr;
    FnVoidPtr               drawer_do_on_open = nullptr;
    FnVoidPtr               drawer_do_on_close = nullptr;
    FnVoidPtr               drawer_refresh_nav = nullptr;
    FnVoidPtr               drawer_close    = nullptr;
    FnGainFavor             gain_favor_orig = nullptr;

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

    // Remote HousePipe state (host & client)
    uint8_t  host_pipe_owner_peer    = kNoPeer;
    int64_t  host_pipe_cat_id        = 0;
    void*    host_pipe_comp          = nullptr;
    uint8_t  placing_pipe_peer       = kNoPeer;
    std::unordered_map<int64_t, uint8_t> last_dropper_peer;
    void*    pipe_saved_slot         = nullptr;
    int32_t  pipe_saved_prevcats     = 0;
    bool     pipe_hidden_for_remote  = false;
    bool     in_host_remote_donate   = false;

    int64_t  client_pipe_cat_id      = 0;
    bool     client_pipe_donated     = false;
    bool     client_pipe_open_pending= false;
    bool     in_client_donate_cat    = false;
    int      client_donate_npc       = -1;
    void*    client_pipe_saved_held  = nullptr;
    bool     client_pipe_spoofed_held= false;

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

extern HouseState g;

// --- Shared utilities (mgmp_house.cpp) --------------------------------------
uint32_t    now_ms();
void        write_game_std_string(void* str_addr, const char* s);
bool        get_cached_list(void* scene, int comp_id, void*** out_data, uint32_t* out_count);
const void* current_cat_registry();
void        rebuild_actor_map();
void        clear_generation_state();
void        advance_generation(const char* why);
bool        live_house_scene_valid();
void        send_cat_records(const std::vector<HouseCatPose>& poses, uint16_t count, uint8_t peer);
void        send_baseline(uint8_t target_peer, bool with_cat_records = false, bool returning = false);

// --- Physics & cat drag/pose/cursor sync (mgmp_house_physics.cpp) -----------
bool read_housecat_pose(void* hc, HouseCatPose& out);
void apply_pose_to_client_actor(void* hc, const HouseCatPose& p, bool snap_zone);
void move_held_cat(void* hc, float wx, float wy);
void remove_client_room_member(void* area, void* hc);
void apply_client_room(void* hc, void* target);
Vec2D cat_drag_anchor(const void* cat, bool flipped = true);
void host_sample_and_send_poses();
void host_release_capture(int64_t cat_id, float wx, float wy, bool apply_drop, Vec2D velocity = {});
void host_release_all_captures();
bool host_try_grant_grab(uint8_t peer, int64_t cat_id, uint32_t req_seq, float wx, float wy);
void observe_pose_clock(uint32_t host_tick, uint32_t received_at);
void client_interpolate_cats();
void send_cursor_sample();
bool house_mouse_world(float* out_wx, float* out_wy);
bool project_world_to_norm(float wx, float wy, float* out_nx, float* out_ny);
void physics_on_message(uint8_t from, const NetMsg& m, uint32_t now);
void pipe_on_message(uint8_t from, const NetMsg& m);
void pipe_client_pump();
void pipe_on_peer_disconnect(uint8_t peer);
void pipe_host_broadcast_progress(uint8_t target_peer = kNoPeer);
void pipe_host_abort_remote();

// --- Interstitials & furniture layout (mgmp_interstitials.cpp) --------------
bool serialize_interstitial_queue(const void* iq, std::vector<uint8_t>& out);
bool deserialize_and_run_interstitials(const uint8_t* data, uint32_t size, const void* house_scene);
void interstitials_client_pump();
void interstitials_on_message(uint8_t from, const NetMsg& m);

// --- Adventure preparation sync (mgmp_prep.cpp) -----------------------------
void host_poll_prep_state();
void prep_host_pump(bool has_house, bool suspended);
void prep_client_pump();
void prep_on_message(uint8_t from, const NetMsg& m);

// --- Checkpoint reload & live CatData sync (mgmp_reload.cpp) ----------------
bool reload_client_pump();
void apply_client_cat_records();

} // namespace house_detail
} // namespace mgmp
