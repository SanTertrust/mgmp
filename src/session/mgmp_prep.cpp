// mgmp_prep.cpp -- Adventure preparation spectator & transition sync
// (ClassChooser / StorageItems / ActSelection).

#include "mgmp_house_internal.h"
#include "mgmp_leave.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_resolve.h"
#include "mgmp_savefile.h"

#include <cstdio>
#include <cstring>

namespace mgmp {
namespace house_detail {
namespace {

void read_prep_equipment(const uint8_t* cat, HousePrepCat& summary) {
    size_t used = 0;
    for (size_t slot = 0; slot < 5; ++slot) {
        const uint8_t* slot_ptr = cat + kCatData_Equipment + slot * kEquipment_Size;
        int64_t uid = 0;
        mem_read(slot_ptr, &uid, sizeof(uid));
        summary.eq_uids[slot] = uid;

        char item[48]{};
        const auto* name = slot_ptr + kEquipment_Name;
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
                const int64_t want_uid = prep.cats[ci].eq_uids[found_slot];
                const bool mismatch = (want_uid != 0)
                    ? (uid != want_uid)
                    : (strcmp(cur_name, prep.cats[ci].eq_slots[found_slot]) != 0);
                if (mismatch) {
                    focus_cat(eq_cat, eq_cat == prep.focused_cat_id);
                    void* box_ptr = box;
                    g.inv_item_box_click_apply(&box_ptr);
                    log_line("HOUSE", "client StorageItems unequip '%s' (uid=%lld) from cat %lld (slot %d)",
                             cur_name, (long long)uid, (long long)eq_cat, found_slot);
                }
                break;
            }
        }
    }

    // Pass 2: equip missing items onto each cat's slots (matched by UID first,
    // falling back to name only if the host did not provide a UID).
    if (boxes && box_count > 0 && reg && g.cat_by_id) {
        for (uint8_t ci = 0; ci < prep.count; ++ci) {
            const int64_t cid = prep.cats[ci].cat_id;
            if (cid == 0) continue;
            const auto* cat = static_cast<const uint8_t*>(g.cat_by_id(reg, cid));
            if (!cat) continue;

            for (int s = 0; s < 5; ++s) {
                const int64_t desired_uid = prep.cats[ci].eq_uids[s];
                const char* desired = prep.cats[ci].eq_slots[s];
                if (desired_uid == 0 && !desired[0]) continue;

                const uint8_t* slot_ptr = cat + kCatData_Equipment + s * kEquipment_Size;
                int64_t cur_uid = 0;
                mem_read(slot_ptr, &cur_uid, sizeof(cur_uid));
                if (desired_uid != 0) {
                    if (cur_uid == desired_uid) continue;
                } else {
                    char cur_name[48]{};
                    mem_read_std_string(slot_ptr + kEquipment_Name, cur_name, sizeof(cur_name));
                    if (strcmp(cur_name, desired) == 0) continue;
                }

                for (uint32_t bi = 0; bi < box_count; ++bi) {
                    void* box = boxes[bi];
                    if (!box) continue;
                    int64_t uid = 0;
                    if (!mem_read((const uint8_t*)box + kInvItemBox_ItemUid, &uid, sizeof(uid)) || uid == 0)
                        continue;
                    if (g.inv_screen_item_eq(screen, uid) != -1) continue;
                    bool matched = false;
                    if (desired_uid != 0) {
                        matched = (uid == desired_uid);
                    } else {
                        const auto* eq = static_cast<const uint8_t*>(g.inv_screen_lookup(screen, uid));
                        if (!eq) continue;
                        char eq_name[48]{};
                        mem_read_std_string(eq + kEquipment_Name, eq_name, sizeof(eq_name));
                        matched = (strcmp(eq_name, desired) == 0);
                    }
                    if (matched) {
                        focus_cat(cid, cid == prep.focused_cat_id);
                        void* box_ptr = box;
                        g.inv_item_box_click_apply(&box_ptr);
                        log_line("HOUSE", "client StorageItems equip '%s' (uid=%lld) on cat %lld (slot %d)",
                                 desired, (long long)uid, (long long)cid, s);
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

} // namespace

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

void prep_host_pump(bool has_house, bool suspended) {
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
    if (g.phase == HostPhase::Prep) {
        const void* psc = nullptr;
        bool psusp = false;
        PrepStage detected = PrepStage::None;
        if (leave_find_scene("ActSelection", &psc, &psusp) && psc && !psusp)
            detected = PrepStage::ActSelection;
        else if (leave_find_scene("StorageItems", &psc, &psusp) && psc && !psusp)
            detected = PrepStage::StorageItems;
        else if (leave_find_scene("ClassChooser", &psc, &psusp) && psc && !psusp)
            detected = PrepStage::ClassChooser;

        if (detected != PrepStage::None && detected != g.prep_stage) {
            g.prep_stage = detected;
            host_poll_prep_state();
            log_line("HOUSE", "host prep stage -> %u (gen=%u)",
                     (unsigned)detected, g.generation);
        } else if (detected == PrepStage::None && has_house && !suspended) {
            // Host pressed Back out of Prep and returned to House.
            g.phase      = HostPhase::House;
            g.prep_stage = PrepStage::None;
            g.have_prep  = false;
            HousePrepMsg clear_prep{};
            clear_prep.generation = g.generation;
            clear_prep.active     = 0;
            clear_prep.stage      = (uint8_t)PrepStage::None;
            net_send_house_prep(clear_prep);
            HousePhaseMsg pm{ g.generation, (uint8_t)HostPhase::House, 0 };
            net_send_house_phase(pm);
            rebuild_actor_map();
            send_baseline(kNoPeer, true);
            log_line("HOUSE", "host backed out of Prep back to House (gen=%u)", g.generation);
        }
    }
}

void prep_client_pump() {
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

    // Client: handle forward and backward Prep scene transitions.
    if (g.reload_stage == 0 && !savefile_has_pending()) {
        const void* act = nullptr; bool act_susp = false;
        const void* st  = nullptr; bool st_susp  = false;
        const void* cc  = nullptr; bool cc_susp  = false;
        const bool has_act = leave_find_scene("ActSelection", &act, &act_susp) && act;
        const bool has_st  = leave_find_scene("StorageItems", &st, &st_susp) && st;
        const bool has_cc  = leave_find_scene("ClassChooser", &cc, &cc_susp) && cc;

        if (g.phase == HostPhase::House) {
            // Host cancelled Prep back to House: tear down any Prep scenes
            // and unsuspend House.
            if (has_act) *(uint8_t*)((uintptr_t)act + kScene_Destroying) = 1;
            if (has_st)  *(uint8_t*)((uintptr_t)st  + kScene_Destroying) = 1;
            if (has_cc)  *(uint8_t*)((uintptr_t)cc  + kScene_Destroying) = 1;
            if (has_act || has_st || has_cc) {
                g.have_prep = false;
                g.has_pending_class_chooser = false;
                g.entered_class_chooser_gen = 0;
                const void* hs = nullptr; bool hs_susp = false;
                if (leave_find_scene(kScene_House, &hs, &hs_susp) && hs)
                    *(uint8_t*)((uintptr_t)hs + kScene_Suspended) = 0;
                log_line("HOUSE", "client backed out of Prep back to House");
            }
        } else if (g.phase == HostPhase::Prep && g.have_prep) {
            const uint8_t want = g.last_prep.stage;
            if (want == (uint8_t)PrepStage::ActSelection) {
                // Forward: StorageItems -> ActSelection (suspend StorageItems
                // instead of destroying it so Back -> StorageItems is instant).
                if (has_st && !has_act) {
                    void* client_md = nullptr;
                    if (g.director_slot)
                        mem_read(g.director_slot, &client_md, sizeof(client_md));
                    void* dir = nullptr;
                    if (client_md)
                        mem_read((const uint8_t*)client_md + kDir_SceneDirector, &dir, sizeof(dir));
                    if (dir && g.dir_add_scene && g.create_entity && g.create_act_sel) {
                        *(uint8_t*)((uintptr_t)st + kScene_Suspended) = 1;
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
            } else if (want == (uint8_t)PrepStage::StorageItems) {
                // Backward: ActSelection -> StorageItems.
                if (has_act) {
                    *(uint8_t*)((uintptr_t)act + kScene_Destroying) = 1;
                    if (has_st && st_susp)
                        *(uint8_t*)((uintptr_t)st + kScene_Suspended) = 0;
                    log_line("HOUSE", "client backed out of ActSelection -> StorageItems");
                }
            } else if (want == (uint8_t)PrepStage::ClassChooser) {
                // Backward: ActSelection / StorageItems -> ClassChooser.
                if (has_act) *(uint8_t*)((uintptr_t)act + kScene_Destroying) = 1;
                if (has_st)  *(uint8_t*)((uintptr_t)st  + kScene_Destroying) = 1;
                if ((has_act || has_st) && !has_cc && g.last_prep.map_name[0]) {
                    g.entered_class_chooser_gen = 0;
                    g.pending_class_chooser     = g.last_prep;
                    g.pending_class_chooser_gen = g.client_gen;
                    g.has_pending_class_chooser = true;
                    log_line("HOUSE", "client backed out to ClassChooser");
                } else if (has_cc && cc_susp) {
                    *(uint8_t*)((uintptr_t)cc + kScene_Suspended) = 0;
                }
            }
        }
    }
}

void prep_on_message(uint8_t from, const NetMsg& m) {
    if (m.type != MSG_HOUSE_PREP || !m.house_prep) return;
    const auto& hp = *m.house_prep;
    if (!g.is_client || from != kHostPeer || hp.generation != g.client_gen)
        return;
    g.last_prep = hp;
    g.have_prep = (hp.active != 0);
    if (hp.active && hp.map_name[0] &&
        hp.stage == (uint8_t)PrepStage::ClassChooser &&
        g.entered_class_chooser_gen != hp.generation) {
        g.pending_class_chooser     = hp;
        g.pending_class_chooser_gen = hp.generation;
        g.has_pending_class_chooser = true;
    }
}

} // namespace house_detail

const HousePrepMsg* house_spectator_prep() {
    auto& g = house_detail::g;
    if (!g.on || !g.is_client || g.phase != HostPhase::Prep || !g.have_prep) return nullptr;
    if (g.last_prep.stage == (uint8_t)PrepStage::ClassChooser ||
        g.last_prep.stage == (uint8_t)PrepStage::StorageItems)
        return nullptr;
    return &g.last_prep;
}

void house_set_go_on_adventure_orig(void* fn) {
    house_detail::g.go_on_adv = (house_detail::FnGoOnAdventure)fn;
}

void house_on_go_on_adventure_cb_pre() {
    auto& g = house_detail::g;
    if (!g.on || g.is_client) return;
    house_detail::host_release_all_captures();
    if (g.phase != HostPhase::Prep) {
        g.phase = HostPhase::Prep;
        HousePhaseMsg pm{ g.generation, (uint8_t)HostPhase::Prep, 0 };
        net_send_house_phase(pm);
    }
    log_line("HOUSE", "host finished ClassChooser -> StorageItems (gen=%u)", g.generation);
}

void house_on_go_on_adventure_cb_post() {
    auto& g = house_detail::g;
    if (!g.on || g.is_client) return;
    g.prep_stage        = PrepStage::StorageItems;
    g.need_prep_publish = true;
    house_detail::host_poll_prep_state();
}

void house_on_start_adventure_cb_post() {
    auto& g = house_detail::g;
    if (!g.on || g.is_client) return;
    g.phase             = HostPhase::Adventure;
    g.prep_stage        = PrepStage::None;
    g.need_prep_publish = false;
    memset(g.peer_map_ready, 0, sizeof(g.peer_map_ready));
    memset(g.peer_map_deadline, 0, sizeof(g.peer_map_deadline));
    if (g.generation == 0) g.generation = 1;
    g.waiting_first_node = true;
    g.need_adv_publish   = true;
    log_line("HOUSE", "host started adventure -- publishing map checkpoint");
}

void house_on_go_on_adventure_pre(void* /*md*/, void* map_str, void* collars_vec, void* cat_ids_buf) {
    auto& g = house_detail::g;
    if (!g.on || g.is_client) return;
    house_detail::host_release_all_captures();
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
