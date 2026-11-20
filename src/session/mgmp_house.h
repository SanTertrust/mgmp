#pragma once
// mgmp_house -- host-authoritative House co-op (2-4 players).
//
// Coordinates:
//   1. House lifecycle, phase tracking (Outside / House / Prep / Adventure),
//      and scene generation numbers.
//   2. HouseCat enumeration, baseline/pose replication, and bounded
//      interpolation on clients (suppressing client AI/physics/overlaps while
//      keeping native Animator and Renderer alive).
//   3. Per-cat drag ownership arbitration (host-authoritative, max 1 held cat
//      per peer, simultaneous drags across peers) + world-space peer cursors.
//   4. Adventure preparation spectator sync, adventure start checkpoint +
//      client map-readiness gate, and return-to-house transition/reset.

#include <cstddef>
#include <cstdint>

namespace mgmp {

struct NetMsg;

enum class HostPhase : uint8_t {
    Outside   = 0,
    House     = 1,
    Prep      = 2,
    Adventure = 3,
};

void house_init();
void house_shutdown();
void house_set_base(uintptr_t base);

// Called every frame on the game thread from lockstep_pump (or session_update).
void house_pump();

// Route any MSG_HOUSE_* message received from `from`.
void house_on_message(uint8_t from, const NetMsg& m);

// Called from session_on_hello when a peer joins or reconnects.
// Returns true if the host is in House/Prep and handled catch-up through the
// House path (checkpoint + baseline + prep), false if Adventure catch-up
// should run instead.
bool house_catchup(uint8_t peer);

// Called when a peer leaves or disconnects so its held captures are released.
void house_on_peer_disconnect(uint8_t peer);

// Phase / generation / readiness queries.
HostPhase   house_phase();
uint32_t    house_generation();
bool        house_in_house_or_prep();
bool        house_clients_map_ready();
void        house_status(char* out, size_t out_size);

// Peer cursor projection for mgmp_overlay.cpp when in House.
bool house_get_peer_cursor(uint8_t peer,
                           float* out_nx,
                           float* out_ny,
                           uint8_t* out_mode,
                           bool* out_active);

// --- Hooks installed by mgmp_hooks.cpp (gated on tune::kHouseCoop) ----------

void house_on_house_test_init(void* house_test, bool arg1, bool arg2);
bool house_on_housecat_update(void* housecat);
void house_on_housecat_update_post(void* housecat);
bool house_on_housecat_late_update(void* housecat);
bool house_on_housecat_physics_update(void* physics);
void house_on_housecat_physics_update_post(void* physics);
bool house_on_click_manager_update(void* click_mgr);
void house_on_click_manager_update_post(void* click_mgr);
bool house_on_click_manager_drop(void* click_mgr);
void house_on_pass_day_cb_pre();
void house_on_pass_day_cb_post();
void house_on_go_on_adventure_cb_pre();
void house_on_go_on_adventure_cb_post();
void house_on_start_adventure_cb_post();
void house_on_return_house_pre(bool success);
void house_on_return_house_post(bool success);
void house_on_return_cat_from_adv(void* house, int64_t cat_id);

void house_set_do_interstitials_orig(void* fn);
void house_on_do_interstitials(void* queue);

void house_set_go_on_adventure_orig(void* fn);
void house_on_go_on_adventure_pre(void* md, void* map_str, void* collars_vec, void* cat_ids_buf);

} // namespace mgmp
