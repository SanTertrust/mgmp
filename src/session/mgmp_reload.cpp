// mgmp_reload.cpp -- Client 2-stage ContinueSave checkpoint reload, live
// CatData record application, and House/Return/PassDay checkpoint triggers.

#include "mgmp_house_internal.h"
#include "mgmp_catsync.h"
#include "mgmp_follow.h"
#include "mgmp_invsync.h"
#include "mgmp_leave.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_runhist.h"
#include "mgmp_savefile.h"

#include <cstring>

namespace mgmp {
namespace house_detail {
namespace {

// True while any glaiel::Transition component is live in the "Transition"
// scene (e.g. MapScreen::EnterNode's HOME cutscene transition whose callback
// lambda captures MewDirector* and MapScreen*). Destroying MewDirector while a
// Transition is still ticking causes a use-after-free in DoTransitionCallback.
bool has_active_transition() {
    const void* ts = nullptr;
    bool susp = false;
    if (!leave_find_scene("Transition", &ts, &susp) || !ts) return false;
    void** list = nullptr;
    uint32_t count = 0;
    return get_cached_list(const_cast<void*>(ts), kCompId_Transition, &list, &count) && count > 0;
}

} // namespace

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

bool reload_client_pump() {
    // Client: reload the mirror in-place when a fresh House/Prep/Adventure
    // save arrives while in House or any Prep scene.
    if (g.reload_stage == 0 && savefile_has_pending() && !has_active_transition()) {
        // Both standalone shops and NPC offices can run Shop::update, which
        // reads the inventory owned by MewDirector. Leave the checkpoint queued
        // until the game has removed those scenes.
        if (leave_scene_attached("Shop") || leave_scene_attached("NPCOffice")) return false;

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
                        return true;
                    }
                }
            }
        }
    } else if (g.reload_stage == 1) {
        for (const char* name : HouseState::reload_scenes)
            if (leave_scene_attached(name)) return true;
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
        if (g.reload_stage != 0) return true;
    }
    return false;
}

} // namespace house_detail

bool house_catchup(uint8_t peer) {
    auto& g = house_detail::g;
    if (!g.on || g.is_client || !house_in_house_or_prep())
        return false;

    if (g.need_adv_publish || g.need_prep_publish || g.need_day_publish || g.need_ret_publish) {
        g.peer_needs_checkpoint[peer] = true;
        log_line("HOUSE", "deferring catch-up for peer %u until checkpoint commits", (unsigned)peer);
        return true;
    }
    g.peer_needs_checkpoint[peer] = false;

    HousePhaseMsg pm{ g.generation, (uint8_t)g.phase, 0 };
    net_send_house_phase_to(peer, pm);

    if (savefile_publish_active(peer, /*fresh=*/false, /*flush_adventure=*/false))
        log_line("HOUSE", "catch-up sent save checkpoint to peer %u (gen=%u phase=%u)",
                 (unsigned)peer, g.generation, (unsigned)g.phase);

    if (g.phase == HostPhase::House) {
        house_detail::send_baseline(peer, true);
        house_detail::pipe_host_broadcast_progress(peer);
        // Inform late joiner of any currently active captures.
        for (auto& kv : g.host_captures) {
            HouseGrabRespMsg r{};
            r.generation  = g.generation;
            r.cat_id      = kv.second.cat_id;
            r.holder_peer = kv.second.holder_peer;
            r.capture_id  = kv.second.capture_id;
            r.accepted    = 1;
            r.wx          = kv.second.wx;
            r.wy          = kv.second.wy;
            net_send_house_grab_resp_to(peer, r);
        }
    } else if (g.phase == HostPhase::Prep) {
        house_detail::host_poll_prep_state();
        if (g.have_prep) {
            // Re-send full ClassChooser entry info so late joiner opens the same prep screen.
            HousePrepMsg hp = g.last_prep;
            net_send_house_prep_to(peer, hp);
        }
    }
    return true;
}

void house_on_map_ready() {
    auto& g = house_detail::g;
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

void house_set_autosave_orig(void* fn) {
    house_detail::g.autosave_house = (house_detail::FnVoidPtr)fn;
}

void house_on_autosave_house() {
    auto& g = house_detail::g;
    if (!g.on || g.is_client) return;
    if (g.phase != HostPhase::House) return;
    if (g.in_pass_day || g.in_return || g.suppress_autosave_publish ||
        g.need_day_publish || g.need_ret_publish || g.need_adv_publish || g.need_prep_publish)
        return;
    g.need_roster_publish = true;
    log_line("HOUSE", "host AutoSaveHouse -> scheduling checkpoint publish");
}

void house_on_pass_day_cb_pre() {
    auto& g = house_detail::g;
    if (!g.on || g.is_client) return;
    g.in_pass_day = true;
    house_detail::pipe_host_abort_remote();
    house_detail::host_release_all_captures();
}

void house_on_pass_day_cb_post() {
    auto& g = house_detail::g;
    if (!g.on || g.is_client) return;
    g.in_pass_day = false;
    house_detail::advance_generation("House::PassDay");
    g.phase            = HostPhase::House;
    g.need_day_publish = true;
}

void house_on_return_house_pre(bool success) {
    auto& g = house_detail::g;
    if (!g.on || g.is_client) return;
    g.in_return = true;
    house_detail::advance_generation(success ? "ReturnToHouseSuccess" : "ReturnToHouseFail");
    g.phase = HostPhase::House;
    g.waiting_first_node = false;
    g.need_adv_publish = false;
    g.need_prep_publish = false;
    // Reset per-run follow state so the next adventure starts clean.
    follow_forget();
    lockstep_leave_run();
}

void house_on_return_house_post(bool /*success*/) {
    auto& g = house_detail::g;
    if (!g.on || g.is_client) return;
    g.in_return = false;
    g.need_ret_publish = true;
}

void house_on_return_cat_from_adv(void* /*house*/, int64_t cat_id) {
    auto& g = house_detail::g;
    if (!g.on || g.is_client) return;
    house_detail::rebuild_actor_map();
    const auto found = g.cat_actors.find(cat_id);
    HouseCatPose pose{};
    if (found != g.cat_actors.end() && house_detail::read_housecat_pose(found->second, pose)) {
        if (!g.return_first_tick) g.return_first_tick = house_detail::now_ms();
        g.returning_first_poses.emplace(cat_id, pose);
    }
    log_line("HOUSE", "returning cat %lld spawned in House", (long long)cat_id);
}

} // namespace mgmp
