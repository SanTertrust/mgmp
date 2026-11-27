// mgmp_gameui.cpp -- native Mewgenics UI overlay toolkit (see mgmp_gameui.h).

#include "mgmp_gameui.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"

#include <windows.h>
#include <cstring>
#include <cwchar>

namespace mgmp {
namespace {

using fn_add_scene        = void* (__fastcall*)(void* director, void* str_by_val);
using fn_create_entity    = void* (__fastcall*)(void* scene);
using fn_create_rc        = void* (__fastcall*)(void* scene, void* entity, int32_t* layers);
using fn_create_cam       = void* (__fastcall*)(void* scene, void* entity);
using fn_create_renderer_empty = void* (__fastcall*)(void* scene, void* entity);
using fn_renderer_init    = void  (__fastcall*)(void* renderer, void* str_by_val);
using fn_create_catparts  = void* (__fastcall*)(void* scene, void* entity, void** cat_data_ref);
using fn_find_child       = void* (__fastcall*)(void* renderer, void* str_by_val);
using fn_tb_set_text      = void  (__fastcall*)(void* tb, void* wstr_by_val, bool keep_style, bool rebatch);
using fn_tb_set_color     = void  (__fastcall*)(void* tb, const GameUiColor* color);
using fn_mc_goto_stop     = void  (__fastcall*)(void* mc, int32_t frame);
using fn_cam_get_mouse    = void* (__fastcall*)(void* cam, double* out_vec2);
using fn_str_assign       = void* (__fastcall*)(void* str, const char* s, size_t n);
using fn_wstr_assign      = void* (__fastcall*)(void* wstr, const wchar_t* s, size_t n);
using fn_str_dtor         = void  (__fastcall*)(void* str);
using fn_wstr_dtor        = void  (__fastcall*)(void* wstr);

using fn_add_menupanel    = void* (__fastcall*)(void* entity);
using fn_panel_get_btn    = void* (__fastcall*)(void* panel, void* str_by_val, bool create);
using fn_create_ab_tt     = void* (__fastcall*)(void* scene, void* entity);
using fn_create_vchar     = void* (__fastcall*)(void* scene, const void* cat_data);
using fn_vchar_add_ab     = void* (__fastcall*)(void* vchar, void* str_by_val, bool flag);
using fn_cat_basic_atk    = void* (__fastcall*)(void* cat_data, void* retstr);
using fn_setup_ab_icon    = void  (__fastcall*)(void* ability, void* mc, int32_t slot);
using fn_setup_pass_icon  = void  (__fastcall*)(void* cat_passive_by_val, void* mc, const void* cat_data, int32_t slot);
using fn_setup_item_icon  = void  (__fastcall*)(void* spawndb, const void* name_str, void* equip, void* disp_obj);
using fn_item_name        = void* (__fastcall*)(void* spawndb, void* retwstr, void* equip);
using fn_item_tooltip     = void* (__fastcall*)(void* spawndb, void* retwstr, void* equip);
using fn_passive_name     = void* (__fastcall*)(void* spawndb, void* retwstr, void* str_by_val, int32_t level, const void* cat_data, void* character);
using fn_passive_desc     = void* (__fastcall*)(void* spawndb, void* retwstr, void* str_by_val, int32_t level, const void* cat_data, void* character);
using fn_ab_display_name  = void* (__fastcall*)(void* ability, void* retwstr);
using fn_ab_tooltip       = void* (__fastcall*)(void* ability, void* retwstr);
using fn_button_enable    = void  (__fastcall*)(void* button, bool enabled);
using fn_mc_reset_goto_s  = void  (__fastcall*)(void* mc, void* str_by_val);
using fn_swf_find_child   = void* (__fastcall*)(void* container, void* str_by_val, bool flag);

struct FnTable {
    bool              checked = false;
    bool              ok      = false;
    bool              combat_bar_ok = false;
    fn_add_scene      add_scene      = nullptr;
    fn_create_entity  create_entity  = nullptr;
    fn_create_rc      create_rc      = nullptr;
    fn_create_cam     create_cam     = nullptr;
    fn_create_renderer_empty create_renderer_empty = nullptr;
    fn_renderer_init  renderer_init  = nullptr;
    fn_create_catparts create_catparts = nullptr;
    fn_find_child     find_child     = nullptr;
    fn_tb_set_text    tb_set_text    = nullptr;
    fn_tb_set_color   tb_set_color   = nullptr;
    fn_mc_goto_stop   mc_goto_stop   = nullptr;
    fn_cam_get_mouse  cam_get_mouse  = nullptr;
    fn_str_assign     str_assign     = nullptr;
    fn_wstr_assign    wstr_assign    = nullptr;
    fn_str_dtor       str_dtor       = nullptr;
    fn_wstr_dtor      wstr_dtor      = nullptr;

    fn_add_menupanel   add_menupanel   = nullptr;
    fn_panel_get_btn   panel_get_btn   = nullptr;
    fn_create_ab_tt    create_ab_tt    = nullptr;
    fn_create_vchar    create_vchar    = nullptr;
    fn_vchar_add_ab    vchar_add_ab    = nullptr;
    fn_cat_basic_atk   cat_basic_atk   = nullptr;
    fn_setup_ab_icon   setup_ab_icon   = nullptr;
    fn_setup_pass_icon setup_pass_icon = nullptr;
    fn_setup_item_icon setup_item_icon = nullptr;
    fn_item_name       item_name       = nullptr;
    fn_item_tooltip    item_tooltip    = nullptr;
    fn_passive_name    passive_name    = nullptr;
    fn_passive_desc    passive_desc    = nullptr;
    fn_ab_display_name ab_display_name = nullptr;
    fn_ab_tooltip      ab_tooltip      = nullptr;
    fn_button_enable   button_enable   = nullptr;
    fn_mc_reset_goto_s mc_reset_goto_s = nullptr;
    fn_swf_find_child  swf_find_child  = nullptr;
};

FnTable g_fn;

bool ensure_resolved() {
    if (g_fn.checked) return g_fn.ok;
    g_fn.checked = true;

    g_fn.add_scene       = (fn_add_scene)      addr_of_call(C_DirectorAddScene);
    g_fn.create_entity   = (fn_create_entity)  addr_of_call(C_SceneCreateEntity);
    g_fn.create_rc       = (fn_create_rc)      addr_of_call(C_CreateRenderCore);
    g_fn.create_cam      = (fn_create_cam)     addr_of_call(C_CreateCamera);
    g_fn.create_renderer_empty = (fn_create_renderer_empty)addr_of_call(C_CreateRendererEmpty);
    g_fn.renderer_init   = (fn_renderer_init)  addr_of_call(C_RendererInit);
    g_fn.create_catparts = (fn_create_catparts)addr_of_call(C_CreateCatParts);
    g_fn.find_child      = (fn_find_child)     addr_of_call(C_RendererFindChild);
    g_fn.tb_set_text     = (fn_tb_set_text)    addr_of_call(C_DynamicTextBoxSetText);
    g_fn.tb_set_color    = (fn_tb_set_color)   addr_of_call(C_DynamicTextBoxSetColor);
    g_fn.mc_goto_stop    = (fn_mc_goto_stop)   addr_of_call(C_MovieClipGotoAndStop);
    g_fn.cam_get_mouse   = (fn_cam_get_mouse)  addr_of_call(C_CameraGetMouse);
    g_fn.str_assign      = (fn_str_assign)     addr_of_call(C_StrAssign);
    g_fn.wstr_assign     = (fn_wstr_assign)    addr_of_call(C_WStrAssign);
    g_fn.str_dtor        = (fn_str_dtor)       addr_of_call(C_GameStrDtor);
    g_fn.wstr_dtor       = (fn_wstr_dtor)      addr_of_call(C_GameWStrDtor);

    g_fn.add_menupanel   = (fn_add_menupanel)  addr_of_call(C_EntityAddMenuPanel);
    g_fn.panel_get_btn   = (fn_panel_get_btn)  addr_of_call(C_MenuPanelGetButton);
    g_fn.create_ab_tt    = (fn_create_ab_tt)   addr_of_call(C_CreateAbilityTooltip);
    g_fn.create_vchar    = (fn_create_vchar)   addr_of_call(C_SceneCreateVirtualChar);
    g_fn.vchar_add_ab    = (fn_vchar_add_ab)   addr_of_call(C_VirtualCharAddAbility);
    g_fn.cat_basic_atk   = (fn_cat_basic_atk)  addr_of_call(C_CatDataGetBasicAttack);
    g_fn.setup_ab_icon   = (fn_setup_ab_icon)  addr_of_call(C_SetupAbilityIcon);
    g_fn.setup_pass_icon = (fn_setup_pass_icon)addr_of_call(C_SetupPassiveIcon);
    g_fn.setup_item_icon = (fn_setup_item_icon)addr_of_call(C_SpawnDbSetupItemIcon);
    g_fn.item_name       = (fn_item_name)      addr_of_call(C_SpawnDbItemName);
    g_fn.item_tooltip    = (fn_item_tooltip)   addr_of_call(C_SpawnDbItemTooltip);
    g_fn.passive_name    = (fn_passive_name)   addr_of_call(C_SpawnDbPassiveName);
    g_fn.passive_desc    = (fn_passive_desc)   addr_of_call(C_SpawnDbPassiveDesc);
    g_fn.ab_display_name = (fn_ab_display_name)addr_of_call(C_AbilityGetDisplayName);
    g_fn.ab_tooltip      = (fn_ab_tooltip)     addr_of_call(C_AbilityGetTooltip);
    g_fn.button_enable   = (fn_button_enable)  addr_of_call(C_ButtonEnable);
    g_fn.mc_reset_goto_s = (fn_mc_reset_goto_s)addr_of_call(C_McResetGotoStopStr);
    g_fn.swf_find_child  = (fn_swf_find_child) addr_of_call(C_SwfFindChild);

    g_fn.ok = g_fn.add_scene && g_fn.create_entity && g_fn.create_rc &&
              g_fn.create_cam && g_fn.create_renderer_empty && g_fn.renderer_init &&
              g_fn.create_catparts &&
              g_fn.find_child && g_fn.tb_set_text && g_fn.tb_set_color &&
              g_fn.mc_goto_stop && g_fn.cam_get_mouse &&
              g_fn.str_assign && g_fn.wstr_assign && g_fn.str_dtor;
    g_fn.combat_bar_ok = g_fn.ok && g_fn.wstr_dtor &&
              g_fn.add_menupanel && g_fn.panel_get_btn && g_fn.create_ab_tt &&
              g_fn.create_vchar && g_fn.vchar_add_ab && g_fn.cat_basic_atk &&
              g_fn.setup_ab_icon && g_fn.setup_pass_icon && g_fn.setup_item_icon &&
              g_fn.item_name && g_fn.item_tooltip && g_fn.passive_name &&
              g_fn.passive_desc && g_fn.ab_display_name && g_fn.ab_tooltip &&
              g_fn.button_enable && g_fn.mc_reset_goto_s &&
              g_fn.swf_find_child;
    if (!g_fn.ok)
        log_line("GAMEUI", "!! one or more native UI call targets did not resolve");
    return g_fn.ok;
}

// --- Game-heap std::string / std::wstring builders (MSVC x64 layout) --------
// Always 32 bytes: { union { buf[16]; ptr; }, size, res }.

struct GameStr {
    alignas(8) uint8_t raw[32];
};

bool init_game_str(GameStr& gs, const char* s) {
    memset(gs.raw, 0, sizeof(gs.raw));
    *(uint64_t*)(gs.raw + 24) = 15;
    if (!s || !s[0]) return true;
    __try {
        g_fn.str_assign(gs.raw, s, strlen(s));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void destroy_game_str(GameStr& gs) {
    __try {
        g_fn.str_dtor(gs.raw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool init_game_wstr(GameStr& gw, const wchar_t* ws) {
    memset(gw.raw, 0, sizeof(gw.raw));
    *(uint64_t*)(gw.raw + 24) = 7;
    if (!ws || !ws[0]) return true;
    __try {
        g_fn.wstr_assign(gw.raw, ws, wcslen(ws));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- SEH shims around game engine calls -------------------------------------

void* get_live_director() {
    const uintptr_t slot = addr_of_data(D_MewDirectorPtr);
    if (!slot) return nullptr;
    void* mewdir = nullptr;
    if (!mem_read((const void*)slot, &mewdir, sizeof(mewdir)) || !mewdir)
        return nullptr;
    void* director = nullptr;
    if (!mem_read((const uint8_t*)mewdir + kDir_SceneDirector, &director, sizeof(director)))
        return nullptr;
    return director;
}

bool scene_is_in_director(void* director, void* scene) {
    if (!director || !scene) return false;
    void** begin = nullptr;
    void** end   = nullptr;
    if (!mem_read((const uint8_t*)director + kDirector_ScenesBegin, &begin, sizeof(begin)) ||
        !mem_read((const uint8_t*)director + kDirector_ScenesEnd,   &end,   sizeof(end))   ||
        !begin || end < begin)
        return false;
    const size_t n = (size_t)(end - begin);
    if (n > 256) return false;
    for (size_t i = 0; i < n; ++i) {
        void* s = nullptr;
        if (mem_read(begin + i, &s, sizeof(s)) && s == scene) return true;
    }
    return false;
}

void* seh_add_scene(void* director, const char* name) {
    GameStr gs{};
    if (!init_game_str(gs, name)) return nullptr;
    // Director::AddScene takes std::string by value (consumes/destroys gs).
    __try {
        return g_fn.add_scene(director, gs.raw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* seh_create_entity(void* scene) {
    __try {
        return g_fn.create_entity(scene);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* seh_create_rc(void* scene, void* entity, int32_t layers) {
    __try {
        return g_fn.create_rc(scene, entity, &layers);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* seh_create_cam(void* scene, void* entity) {
    __try {
        return g_fn.create_cam(scene, entity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* seh_create_renderer(void* scene, void* entity, const char* clip_name) {
    GameStr gs{};
    if (!init_game_str(gs, clip_name)) return nullptr;
    void* r = nullptr;
    // CreateComponent<Renderer>() constructs & registers an empty Renderer;
    // Renderer::init(std::string) takes clip_name by value (consumes gs).
    __try {
        r = g_fn.create_renderer_empty(scene, entity);
        if (r) g_fn.renderer_init(r, gs.raw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = nullptr;
    }
    return r;
}

void* seh_create_catparts(void* scene, void* entity, void* cat_data) {
    void* cd = cat_data;
    __try {
        return g_fn.create_catparts(scene, entity, &cd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* seh_find_child(void* renderer, const char* child_name) {
    if (!renderer || !child_name) return nullptr;
    GameStr gs{};
    if (!init_game_str(gs, child_name)) return nullptr;
    // Renderer::FindChild takes std::string by value (consumes/destroys gs).
    __try {
        return g_fn.find_child(renderer, gs.raw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void seh_tb_set_text(void* tb, const wchar_t* text) {
    if (!tb) return;
    GameStr gw{};
    if (!init_game_wstr(gw, text ? text : L"")) return;
    // DynamicTextBox::setText takes std::wstring by value (consumes/destroys gw).
    __try {
        g_fn.tb_set_text(tb, gw.raw, false, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void seh_tb_set_color(void* tb, GameUiColor color) {
    if (!tb) return;
    __try {
        g_fn.tb_set_color(tb, &color);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void seh_mc_goto_stop(void* mc, int32_t frame) {
    if (!mc) return;
    __try {
        g_fn.mc_goto_stop(mc, frame);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool seh_cam_get_mouse(void* cam, double& mx, double& my) {
    if (!cam) return false;
    double out[2] = { 0.0, 0.0 };
    __try {
        g_fn.cam_get_mouse(cam, out);
        mx = out[0];
        my = out[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void set_renderer_layer(void* renderer, int32_t layer) {
    if (!renderer) return;
    mem_write((uint8_t*)renderer + kRenderer_RenderLayer, &layer, sizeof(layer));
}

} // namespace

// ---------------------------------------------------------------------------

bool gameui_available() {
    return ensure_resolved() && get_live_director() != nullptr;
}

bool gameui_open(GameUiOverlay& ov, const char* scene_name, double sort_depth) {
    gameui_close(ov);
    if (!ensure_resolved()) return false;

    void* director = get_live_director();
    if (!director) return false;

    void* scene = seh_add_scene(director, scene_name ? scene_name : "MGMP_Overlay");
    if (!scene) {
        log_line("GAMEUI", "!! Director::AddScene('%s') failed",
                 scene_name ? scene_name : "MGMP_Overlay");
        return false;
    }

    // Place overlay above Adventure / MapScreen.
    mem_write((uint8_t*)scene + kScene_SortDepth, &sort_depth, sizeof(sort_depth));

    // Create RenderCore (48 layers, matching YesNoPrompt::init).
    void* rc_ent = seh_create_entity(scene);
    void* rc     = rc_ent ? seh_create_rc(scene, rc_ent, 48) : nullptr;
    if (!rc) {
        uint8_t destroying = 1;
        mem_write((uint8_t*)scene + kScene_Destroying, &destroying, 1);
        log_line("GAMEUI", "!! failed to create RenderCore on overlay scene");
        return false;
    }
    int32_t default_layer = 30;
    mem_write((uint8_t*)rc + 92, &default_layer, sizeof(default_layer));

    // Create Camera in standard 1280x720 virtual UI space.
    void* cam_ent = seh_create_entity(scene);
    void* cam     = cam_ent ? seh_create_cam(scene, cam_ent) : nullptr;
    if (!cam) {
        uint8_t destroying = 1;
        mem_write((uint8_t*)scene + kScene_Destroying, &destroying, 1);
        log_line("GAMEUI", "!! failed to create Camera on overlay scene");
        return false;
    }
    double ppu = 1.0;
    int32_t ydir = 1;
    mem_write((uint8_t*)cam + kCamera_PixelsPerUnit, &ppu,  sizeof(ppu));
    mem_write((uint8_t*)cam + kCamera_YDirection,    &ydir, sizeof(ydir));
    void* cam_tr = nullptr;
    if (mem_read((const uint8_t*)cam + kCamera_Transform, &cam_tr, sizeof(cam_tr)) && cam_tr) {
        double pos[3] = { kGameUiWidth * 0.5, kGameUiHeight * 0.5, 0.0 };
        mem_write((uint8_t*)cam_tr + kTransform_Pos, pos, sizeof(pos));
    }

    // Block controls on underlying scenes (skipping Director::shared and our own).
    ov.blocked_count = 0;
    void* shared_scene = nullptr;
    mem_read((const uint8_t*)director + kDirector_Shared, &shared_scene, sizeof(shared_scene));

    void** begin = nullptr;
    void** end   = nullptr;
    if (mem_read((const uint8_t*)director + kDirector_ScenesBegin, &begin, sizeof(begin)) &&
        mem_read((const uint8_t*)director + kDirector_ScenesEnd,   &end,   sizeof(end))   &&
        begin && end >= begin) {
        const size_t n = (size_t)(end - begin);
        if (n <= 256) {
            for (size_t i = 0; i < n && ov.blocked_count < GameUiOverlay::kMaxBlockedScenes; ++i) {
                void* s = nullptr;
                if (!mem_read(begin + i, &s, sizeof(s)) || !s) continue;
                if (s == scene || s == shared_scene) continue;
                uint8_t destroying = 0, prev_block = 0;
                if (mem_read((const uint8_t*)s + kScene_Destroying, &destroying, 1) && destroying)
                    continue;
                if (mem_read((const uint8_t*)s + kScene_ControlsPrevented, &prev_block, 1) && prev_block)
                    continue;
                uint8_t one = 1;
                if (mem_write((uint8_t*)s + kScene_ControlsPrevented, &one, 1))
                    ov.blocked[ov.blocked_count++] = s;
            }
        }
    }

    ov.director = director;
    ov.scene    = scene;
    ov.camera   = cam;
    ov.open     = true;
    return true;
}

void gameui_close(GameUiOverlay& ov) {
    if (!ov.open) return;

    void* director = get_live_director();
    if (director && director == ov.director) {
        // Restore controls_prevented on still-live scenes we blocked.
        for (uint32_t i = 0; i < ov.blocked_count; ++i) {
            void* s = ov.blocked[i];
            if (!s || !scene_is_in_director(director, s)) continue;
            uint8_t zero = 0;
            mem_write((uint8_t*)s + kScene_ControlsPrevented, &zero, 1);
        }
        // Mark our overlay scene for native teardown.
        if (ov.scene && scene_is_in_director(director, ov.scene)) {
            uint8_t one = 1;
            mem_write((uint8_t*)ov.scene + kScene_Destroying, &one, 1);
        }
    }

    ov.director      = nullptr;
    ov.scene         = nullptr;
    ov.camera        = nullptr;
    ov.blocked_count = 0;
    ov.open          = false;
}

bool gameui_is_alive(const GameUiOverlay& ov) {
    if (!ov.open || !ov.scene || !ov.director) return false;
    void* director = get_live_director();
    if (!director || director != ov.director) return false;
    if (!scene_is_in_director(director, ov.scene)) return false;
    uint8_t destroying = 0;
    if (mem_read((const uint8_t*)ov.scene + kScene_Destroying, &destroying, 1) && destroying)
        return false;
    return true;
}

bool gameui_add_clip(GameUiOverlay& ov, const char* clip_name,
                     double x, double y, double sx, double sy,
                     int32_t layer, double alpha, GameUiClip& out) {
    out = {};
    if (!gameui_is_alive(ov) || !clip_name) return false;
    void* ent = seh_create_entity(ov.scene);
    if (!ent) return false;
    void* rend = seh_create_renderer(ov.scene, ent, clip_name);
    if (!rend) return false;

    set_renderer_layer(rend, layer);
    gameui_set_alpha(rend, alpha);

    void* tr = nullptr;
    mem_read((const uint8_t*)rend + kRenderer_Transform, &tr, sizeof(tr));
    if (tr) {
        gameui_set_pos(tr, x, y, 0.0);
        gameui_set_scale(tr, sx, sy);
    }

    out.entity    = ent;
    out.renderer  = rend;
    out.transform = tr;
    return true;
}

bool gameui_add_label(GameUiOverlay& ov, const wchar_t* text,
                      double x, double y, double scale, int32_t layer,
                      GameUiColor color, GameUiLabel& out) {
    out = {};
    GameUiClip clip{};
    if (!gameui_add_clip(ov, "HouseNametag", x, y, scale, scale, layer, 1.0, clip))
        return false;

    void* tb = seh_find_child(clip.renderer, "catname");
    if (tb) {
        seh_tb_set_color(tb, color);
        seh_tb_set_text(tb, text ? text : L"");
    }

    out.entity    = clip.entity;
    out.renderer  = clip.renderer;
    out.transform = clip.transform;
    out.textbox   = tb;
    return true;
}

bool gameui_add_cat(GameUiOverlay& ov, void* cat_data,
                    double x, double y, double scale, int32_t layer,
                    GameUiCat& out) {
    out = {};
    if (!cat_data) return false;
    GameUiClip clip{};
    if (!gameui_add_clip(ov, "CatTest", x, y, scale, scale, layer, 1.0, clip))
        return false;

    // Stop CatTest's root timeline on "idleF" so the nested idle MovieClip
    // loops cleanly instead of cycling through every animation state at 60 fps.
    if (!gameui_goto_label(clip.renderer, nullptr, "idleF"))
        gameui_goto_frame(clip.renderer, nullptr, 0);

    void* cp = seh_create_catparts(ov.scene, clip.entity, cat_data);
    if (!cp) return false;

    out.entity    = clip.entity;
    out.renderer  = clip.renderer;
    out.catparts  = cp;
    out.transform = clip.transform;
    return true;
}

void gameui_set_pos(void* transform, double x, double y, double sort_depth) {
    if (!transform) return;
    double pos[3] = { x, y, 0.0 };
    mem_write((uint8_t*)transform + kTransform_Pos, pos, sizeof(pos));
    mem_write((uint8_t*)transform + kTransform_SortDepth, &sort_depth, sizeof(sort_depth));
    mem_write((uint8_t*)transform + kTransform_PrevSortDepth, &sort_depth, sizeof(sort_depth));
}

void gameui_set_scale(void* transform, double sx, double sy) {
    if (!transform) return;
    double sc[2] = { sx, sy };
    mem_write((uint8_t*)transform + kTransform_Scale, sc, sizeof(sc));
}

void gameui_set_visible(void* renderer, bool visible) {
    if (!renderer) return;
    uint8_t v = visible ? 1 : 0;
    mem_write((uint8_t*)renderer + kRenderer_Visible, &v, 1);
}

void gameui_set_alpha(void* renderer, double alpha) {
    if (!renderer) return;
    mem_write((uint8_t*)renderer + kRenderer_Alpha, &alpha, sizeof(alpha));
}

void gameui_set_child_visible(void* renderer, const char* child_name, bool visible) {
    void* obj = seh_find_child(renderer, child_name);
    if (!obj) return;
    uint8_t flags = 0;
    if (!mem_read((const uint8_t*)obj + kDispObj_Flags, &flags, 1)) return;
    if (visible) flags |= 0x20u;
    else         flags &= (uint8_t)~0x20u;
    mem_write((uint8_t*)obj + kDispObj_Flags, &flags, 1);
}

void gameui_set_child_text(void* renderer, const char* child_name, const wchar_t* text) {
    void* tb = seh_find_child(renderer, child_name);
    if (tb) seh_tb_set_text(tb, text ? text : L"");
}

void gameui_set_child_color(void* renderer, const char* child_name, GameUiColor color) {
    void* tb = seh_find_child(renderer, child_name);
    if (tb) seh_tb_set_color(tb, color);
}

void gameui_goto_frame(void* renderer, const char* child_name, int32_t frame) {
    void* target = nullptr;
    if (child_name && child_name[0]) {
        target = seh_find_child(renderer, child_name);
    } else if (renderer) {
        mem_read((const uint8_t*)renderer + kRenderer_Graphics, &target, sizeof(target));
    }
    if (target) seh_mc_goto_stop(target, frame);
}

bool gameui_goto_label(void* renderer, const char* child_name, const char* label) {
    if (!label || !label[0]) return false;
    void* target = nullptr;
    if (child_name && child_name[0]) {
        target = seh_find_child(renderer, child_name);
    } else if (renderer) {
        mem_read((const uint8_t*)renderer + kRenderer_Graphics, &target, sizeof(target));
    }
    if (!target) return false;

    // MovieClip+208 -> MovieClipDefinition; +32 is std::unordered_map<std::string, int>
    // whose MSVC x64 list head is at +40 and element count is at +48.
    // Each list node is { Node* next (+0), Node* prev (+8), std::string key (+16), int32_t frame (+48) }.
    void* def = nullptr;
    if (!mem_read((const uint8_t*)target + 208, &def, sizeof(def)) || !def)
        return false;
    void* head = nullptr;
    uint64_t count = 0;
    if (!mem_read((const uint8_t*)def + 40, &head, sizeof(head)) || !head)
        return false;
    if (!mem_read((const uint8_t*)def + 48, &count, sizeof(count)) || count == 0 || count > 8192)
        return false;

    void* cur = nullptr;
    if (!mem_read(head, &cur, sizeof(cur))) return false;

    char key[128] = {};
    for (uint64_t i = 0; i < count && cur && cur != head; ++i) {
        if (gameui_read_string((const uint8_t*)cur + 16, key, sizeof(key)) &&
            strcmp(key, label) == 0) {
            int32_t frame = 0;
            if (mem_read((const uint8_t*)cur + 48, &frame, sizeof(frame))) {
                seh_mc_goto_stop(target, frame);
                return true;
            }
            return false;
        }
        void* next = nullptr;
        if (!mem_read(cur, &next, sizeof(next))) break;
        cur = next;
    }
    return false;
}

void gameui_set_label_text(GameUiLabel& lbl, const wchar_t* text) {
    if (lbl.textbox) seh_tb_set_text(lbl.textbox, text ? text : L"");
}

void gameui_set_label_color(GameUiLabel& lbl, GameUiColor color) {
    if (lbl.textbox) seh_tb_set_color(lbl.textbox, color);
}

bool gameui_get_mouse(const GameUiOverlay& ov, double& mx, double& my) {
    if (!gameui_is_alive(ov) || !ov.camera) return false;
    return seh_cam_get_mouse(ov.camera, mx, my);
}

bool gameui_read_string(const void* game_str, char* out, uint32_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!game_str) return false;
    uint64_t len = 0, capacity = 0;
    if (!mem_read((const uint8_t*)game_str + 16, &len, sizeof(len))) return false;
    if (!mem_read((const uint8_t*)game_str + 24, &capacity, sizeof(capacity))) return false;
    if (len > 4096) return false;
    const uint8_t* chars = (const uint8_t*)game_str;
    if (capacity > 15) {
        void* p = nullptr;
        if (!mem_read(game_str, &p, sizeof(p)) || !p) return false;
        chars = (const uint8_t*)p;
    }
    uint32_t n = (uint32_t)len;
    if (n > cap - 1) n = cap - 1;
    if (n && !mem_read(chars, out, n)) { out[0] = '\0'; return false; }
    out[n] = '\0';
    return true;
}

bool gameui_read_wstring(const void* game_wstr, wchar_t* out, uint32_t cap) {
    if (!out || cap == 0) return false;
    out[0] = L'\0';
    if (!game_wstr) return false;
    uint64_t len = 0, capacity = 0;
    if (!mem_read((const uint8_t*)game_wstr + 16, &len, sizeof(len))) return false;
    if (!mem_read((const uint8_t*)game_wstr + 24, &capacity, sizeof(capacity))) return false;
    if (len > 4096) return false;
    const uint8_t* chars = (const uint8_t*)game_wstr;
    if (capacity > 7) {
        void* p = nullptr;
        if (!mem_read(game_wstr, &p, sizeof(p)) || !p) return false;
        chars = (const uint8_t*)p;
    }
    uint32_t n = (uint32_t)len;
    if (n > cap - 1) n = cap - 1;
    if (n && !mem_read(chars, out, n * sizeof(wchar_t))) { out[0] = L'\0'; return false; }
    out[n] = L'\0';
    return true;
}

// --- Native battle CombatMenu bar (abilities, passives, equipment) ----------

namespace {

struct GameCatPassive {
    GameStr name;
    int32_t level = 1;
    int32_t pad   = 0;
};

void* get_live_spawndb() {
    const uintptr_t slot = addr_of_data(D_SpawnDatabasePtr);
    if (!slot) return nullptr;
    void* db = nullptr;
    if (!mem_read((const void*)slot, &db, sizeof(db))) return nullptr;
    return db;
}

void destroy_game_wstr(void* gw) {
    if (!gw || !g_fn.wstr_dtor) return;
    __try {
        g_fn.wstr_dtor(gw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void assign_str_field(void* dst_str, const char* s) {
    if (!dst_str || !g_fn.str_assign) return;
    __try {
        g_fn.str_assign(dst_str, s ? s : "", s ? strlen(s) : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void assign_wstr_field(void* dst_wstr, const wchar_t* ws) {
    if (!dst_wstr || !g_fn.wstr_assign) return;
    __try {
        g_fn.wstr_assign(dst_wstr, ws ? ws : L"", ws ? wcslen(ws) : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void move_wstr_into_field(void* dst_wstr, GameStr& src_wstr) {
    if (!dst_wstr) {
        destroy_game_wstr(src_wstr.raw);
        return;
    }
    destroy_game_wstr(dst_wstr);
    memcpy(dst_wstr, src_wstr.raw, sizeof(src_wstr.raw));
    memset(src_wstr.raw, 0, sizeof(src_wstr.raw));
    *(uint64_t*)(src_wstr.raw + 24) = 7;
}

void* seh_panel_get_button(void* panel, const char* name) {
    if (!panel || !name) return nullptr;
    GameStr gs{};
    if (!init_game_str(gs, name)) return nullptr;
    // MenuPanel::get_button takes std::string by value (callee destroys gs).
    __try {
        return g_fn.panel_get_btn(panel, gs.raw, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void seh_mc_reset_goto(void* mc, const char* label) {
    if (!mc || !label) return;
    GameStr gs{};
    if (!init_game_str(gs, label)) return;
    __try {
        g_fn.mc_reset_goto_s(mc, gs.raw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void seh_mc_goto_str(void* mc, const char* label) {
    seh_mc_reset_goto(mc, label);
}

void* seh_swf_find_child(void* container, const char* name) {
    if (!container || !name) return nullptr;
    GameStr gs{};
    if (!init_game_str(gs, name)) return nullptr;
    __try {
        return g_fn.swf_find_child(container, gs.raw, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* seh_vchar_add_ability(void* vchar, const char* name) {
    if (!vchar || !name || !name[0] || strcmp(name, "None") == 0) return nullptr;
    GameStr gs{};
    if (!init_game_str(gs, name)) return nullptr;
    __try {
        return g_fn.vchar_add_ab(vchar, gs.raw, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void clear_combat_slot(void* btn) {
    if (!btn) return;
    void* clip = nullptr;
    mem_read((const uint8_t*)btn + kBtn_Clip, &clip, sizeof(clip));
    if (clip) seh_mc_reset_goto(clip, "missing");

    void* nullp = nullptr;
    uint32_t zero_u32 = 0;
    int32_t  zero_i32 = 0;
    int32_t  disabled = 4;
    mem_write((uint8_t*)btn + kBtn_Userdata,           &nullp,    sizeof(nullp));
    mem_write((uint8_t*)btn + kBtn_UserdataType,       &zero_i32, sizeof(zero_i32));
    mem_write((uint8_t*)btn + kBtn_ExtraUserdataCount, &zero_u32, sizeof(zero_u32));
    assign_str_field((uint8_t*)btn + kBtn_StateFrames + 4 * 32, "missing");
    assign_wstr_field((uint8_t*)btn + kBtn_Label,   L"");
    assign_wstr_field((uint8_t*)btn + kBtn_Tooltip, L"");
    mem_write((uint8_t*)btn + kBtn_State, &disabled, sizeof(disabled));
}

void set_button_hoverable_disabled(void* btn) {
    if (!btn) return;
    __try {
        g_fn.button_enable(btn, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    assign_str_field((uint8_t*)btn + kBtn_StateFrames + 4 * 32, "disabled");
    assign_str_field((uint8_t*)btn + kBtn_StateFrames + 2 * 32, "over");
}

void set_dispobj_visible(void* obj, bool visible) {
    if (!obj) return;
    uint8_t flags = 0;
    if (!mem_read((const uint8_t*)obj + kDispObj_Flags, &flags, 1)) return;
    if (visible) flags |= 0x20u;
    else         flags &= (uint8_t)~0x20u;
    mem_write((uint8_t*)obj + kDispObj_Flags, &flags, 1);
}

void set_dispobj_matrix(void* obj, float sx, float sy, float tx, float ty) {
    if (!obj) return;
    // glaiel::swf::DisplayObject::Matrix3x2 at +0x60: { sx, sy, rx, ry, tx, ty }.
    const float m[6] = { sx, sy, 0.0f, 0.0f, tx, ty };
    mem_write((uint8_t*)obj + 0x60, m, sizeof(m));
}

void position_button_clip(void* btn, bool visible, float scale, float x, float y) {
    if (!btn) return;
    void* clip = nullptr;
    if (!mem_read((const uint8_t*)btn + kBtn_Clip, &clip, sizeof(clip)) || !clip)
        return;
    set_dispobj_visible(clip, visible);
    if (visible)
        set_dispobj_matrix(clip, scale, scale, x, y);
    else
        set_dispobj_matrix(clip, 0.01f, 0.01f, -9999.0f, -9999.0f);
}

bool populate_ability_slot(void* btn, void* ability, int32_t slot_idx) {
    clear_combat_slot(btn);
    if (!btn || !ability) return false;
    void* clip = nullptr;
    if (!mem_read((const uint8_t*)btn + kBtn_Clip, &clip, sizeof(clip)) || !clip)
        return false;

    seh_mc_goto_str(clip, "up");

    GameStr name_w{}, tip_w{};
    memset(name_w.raw, 0, sizeof(name_w.raw));
    *(uint64_t*)(name_w.raw + 24) = 7;
    memset(tip_w.raw, 0, sizeof(tip_w.raw));
    *(uint64_t*)(tip_w.raw + 24) = 7;

    bool got_name = false, got_tip = false;
    __try {
        g_fn.ab_display_name(ability, name_w.raw);
        got_name = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        g_fn.ab_tooltip(ability, tip_w.raw);
        got_tip = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (got_name) move_wstr_into_field((uint8_t*)btn + kBtn_Label, name_w);
    if (got_tip)  move_wstr_into_field((uint8_t*)btn + kBtn_Tooltip, tip_w);

    set_button_hoverable_disabled(btn);

    __try {
        g_fn.setup_ab_icon(ability, clip, slot_idx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return true;
}

bool populate_passive_slot(void* btn, void* spawndb, void* cat_data,
                           const char* pname, int32_t level, int32_t slot_idx) {
    clear_combat_slot(btn);
    if (!btn || !spawndb || !cat_data || !pname || !pname[0] || strcmp(pname, "None") == 0)
        return false;
    void* clip = nullptr;
    if (!mem_read((const uint8_t*)btn + kBtn_Clip, &clip, sizeof(clip)) || !clip)
        return false;

    seh_mc_goto_str(clip, "up");

    GameStr name_w{}, tip_w{};
    memset(name_w.raw, 0, sizeof(name_w.raw));
    *(uint64_t*)(name_w.raw + 24) = 7;
    memset(tip_w.raw, 0, sizeof(tip_w.raw));
    *(uint64_t*)(tip_w.raw + 24) = 7;

    GameStr arg1{};
    if (init_game_str(arg1, pname)) {
        bool ok = false;
        __try {
            g_fn.passive_name(spawndb, name_w.raw, arg1.raw, level, cat_data, nullptr);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (ok) move_wstr_into_field((uint8_t*)btn + kBtn_Label, name_w);
    }

    GameStr arg2{};
    if (init_game_str(arg2, pname)) {
        bool ok = false;
        __try {
            g_fn.passive_desc(spawndb, tip_w.raw, arg2.raw, level, cat_data, nullptr);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (ok) move_wstr_into_field((uint8_t*)btn + kBtn_Tooltip, tip_w);
    }

    set_button_hoverable_disabled(btn);

    GameCatPassive cp{};
    if (init_game_str(cp.name, pname)) {
        cp.level = level;
        // SetupPassiveIcon takes CatPassive by value (callee destroys cp.name).
        __try {
            g_fn.setup_pass_icon(&cp, clip, cat_data, slot_idx);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return true;
}

bool populate_item_slot(void* btn, void* spawndb, uint8_t* equip) {
    clear_combat_slot(btn);
    if (!btn || !spawndb || !equip) return false;

    int64_t uid = -1;
    char ename[96] = {};
    if (!mem_read(equip + kEquipment_Uid, &uid, sizeof(uid)) || uid == -1)
        return false;
    if (!gameui_read_string(equip + kEquipment_Name, ename, sizeof(ename)) || !ename[0])
        return false;

    void* clip = nullptr;
    if (!mem_read((const uint8_t*)btn + kBtn_Clip, &clip, sizeof(clip)) || !clip)
        return false;

    seh_mc_goto_str(clip, "up");

    // Set normal equipped shell frame (3), hide quest background, draw item icon.
    if (void* shell = seh_swf_find_child(clip, "shell"))
        seh_mc_goto_stop(shell, 3);
    if (void* questbg = seh_swf_find_child(clip, "questbg"))
        set_dispobj_visible(questbg, false);
    if (void* item_mc = seh_swf_find_child(clip, "item")) {
        __try {
            g_fn.setup_item_icon(spawndb, equip + kEquipment_Name, equip, item_mc);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    GameStr name_w{}, tip_w{};
    memset(name_w.raw, 0, sizeof(name_w.raw));
    *(uint64_t*)(name_w.raw + 24) = 7;
    memset(tip_w.raw, 0, sizeof(tip_w.raw));
    *(uint64_t*)(tip_w.raw + 24) = 7;

    bool got_name = false, got_tip = false;
    __try {
        g_fn.item_name(spawndb, name_w.raw, equip);
        got_name = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        g_fn.item_tooltip(spawndb, tip_w.raw, equip);
        got_tip = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (got_name) move_wstr_into_field((uint8_t*)btn + kBtn_Label, name_w);
    if (got_tip)  move_wstr_into_field((uint8_t*)btn + kBtn_Tooltip, tip_w);

    set_button_hoverable_disabled(btn);
    return true;
}

} // namespace

void* gameui_add_ability_tooltip(GameUiOverlay& ov) {
    if (!gameui_is_alive(ov) || !ensure_resolved() || !g_fn.combat_bar_ok)
        return nullptr;
    void* tt_ent = seh_create_entity(ov.scene);
    if (!tt_ent) return nullptr;
    __try {
        return g_fn.create_ab_tt(ov.scene, tt_ent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool gameui_add_cat_kit_icons(GameUiOverlay& ov, void* cat_data,
                              double cx, double cy, double card_scale,
                              GameUiCombatBar& out, GameUiCatKit& kit) {
    out = {};
    if (!gameui_is_alive(ov) || !cat_data || !ensure_resolved() || !g_fn.combat_bar_ok)
        return false;

    void* ent = seh_create_entity(ov.scene);
    if (!ent) return false;
    void* ren = seh_create_renderer(ov.scene, ent, "CombatMenu");
    if (!ren) return false;

    // Stop CombatMenu's root timeline immediately so child button clips stay
    // fixed on a single frame and never loop/recreate at 60 fps.
    if (!gameui_goto_label(ren, nullptr, "morepassives"))
        gameui_goto_frame(ren, nullptr, 0);

    set_renderer_layer(ren, 32);

    void* tr = nullptr;
    mem_read((const uint8_t*)ren + kRenderer_Transform, &tr, sizeof(tr));

    void* panel = nullptr;
    __try {
        panel = g_fn.add_menupanel(ent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        panel = nullptr;
    }
    if (!panel) return false;

    out.entity    = ent;
    out.renderer  = ren;
    out.transform = tr;
    out.panel     = panel;

    out.btn_attack = seh_panel_get_button(panel, "bAttack");
    out.btn_move   = seh_panel_get_button(panel, "bMove");
    const char* spell_names[4]   = { "bSpell0", "bSpell1", "bSpell2", "bSpell3" };
    const char* passive_names[4] = { "bPassive0", "bPassive1", "bPassive2", "bPassive3" };
    const char* item_names[5]    = { "bItem0", "bItem1", "bItem2", "bItem3", "bItem4" };
    for (int i = 0; i < 4; ++i) out.btn_spell[i]   = seh_panel_get_button(panel, spell_names[i]);
    for (int i = 0; i < 4; ++i) out.btn_passive[i] = seh_panel_get_button(panel, passive_names[i]);
    for (int i = 0; i < 5; ++i) out.btn_item[i]    = seh_panel_get_button(panel, item_names[i]);

    // Hide the battle frame/background and unused battle-only slots.
    const char* hide_names[] = {
        "bg", "hpregen", "manaregen",
        "bEnd", "bBonusSpell", "bPassive4", "bPassive5", "bPassive6"
    };
    for (const char* hn : hide_names)
        gameui_set_child_visible(ren, hn, false);

    // Build VirtualCharacter + Ability* handles for this cat.
    void* spawndb = get_live_spawndb();
    if (!kit.built) {
        kit.built = true;
        __try {
            kit.vchar = g_fn.create_vchar(ov.scene, cat_data);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            kit.vchar = nullptr;
        }
        if (kit.vchar) {
            GameStr atk_s{};
            memset(atk_s.raw, 0, sizeof(atk_s.raw));
            *(uint64_t*)(atk_s.raw + 24) = 15;
            bool got_atk = false;
            __try {
                g_fn.cat_basic_atk(cat_data, atk_s.raw);
                got_atk = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (got_atk) {
                char atk_name[96] = {};
                if (gameui_read_string(atk_s.raw, atk_name, sizeof(atk_name)))
                    kit.ab_attack = seh_vchar_add_ability(kit.vchar, atk_name);
                destroy_game_str(atk_s);
            }

            char move_name[96] = {};
            if (gameui_read_string((const uint8_t*)cat_data + kCatData_Abilities + kCatAbilities_Move,
                                   move_name, sizeof(move_name)))
                kit.ab_move = seh_vchar_add_ability(kit.vchar, move_name);

            for (int i = 0; i < 4; ++i) {
                char sp_name[96] = {};
                if (gameui_read_string((const uint8_t*)cat_data + kCatData_Abilities + kCatAbilities_Spells + i * 32,
                                       sp_name, sizeof(sp_name)))
                    kit.ab_spell[i] = seh_vchar_add_ability(kit.vchar, sp_name);
            }
        }
    }

    // Populate slots and collect only non-empty buttons for compact placement.
    void* active_btns[6]  = {};
    int   active_count    = 0;
    auto push_ab = [&](void* btn, void* ab, int32_t slot) {
        if (populate_ability_slot(btn, ab, slot)) active_btns[active_count++] = btn;
        else position_button_clip(btn, false, 0.01f, -9999.0f, -9999.0f);
    };
    push_ab(out.btn_attack, kit.ab_attack, 10);
    push_ab(out.btn_move,   kit.ab_move,   9);
    for (int i = 0; i < 4; ++i)
        push_ab(out.btn_spell[i], kit.ab_spell[i], i);

    void* passive_btns[4] = {};
    int   passive_count   = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* p = (const uint8_t*)cat_data + kCatData_Abilities + kCatAbilities_Passives + i * kCatPassive_Size;
        char pname[96] = {};
        int32_t level = 1;
        gameui_read_string(p, pname, sizeof(pname));
        mem_read(p + kCatPassive_Level, &level, sizeof(level));
        if (populate_passive_slot(out.btn_passive[i], spawndb, cat_data, pname, level, i))
            passive_btns[passive_count++] = out.btn_passive[i];
        else
            position_button_clip(out.btn_passive[i], false, 0.01f, -9999.0f, -9999.0f);
    }

    void* item_btns[5] = {};
    int   item_count   = 0;
    for (int i = 0; i < (int)kCatData_EquipmentSlots; ++i) {
        uint8_t* eq = (uint8_t*)cat_data + kCatData_Equipment + i * kEquipment_Size;
        if (populate_item_slot(out.btn_item[i], spawndb, eq))
            item_btns[item_count++] = out.btn_item[i];
        else
            position_button_clip(out.btn_item[i], false, 0.01f, -9999.0f, -9999.0f);
    }

    // Arrange icons tightly around the cat card:
    //   Left column  : active abilities (attack, move, spells)
    //   Right column : passives
    //   Bottom row   : equipped gear (head, face, neck, weapon, trinket)
    const float  icon_s  = static_cast<float>(0.50 * card_scale);
    const double left_x  = cx - 118.0 * card_scale;
    const double right_x = cx + 118.0 * card_scale;
    const double ab_step = 38.0 * card_scale;
    const double pa_step = 44.0 * card_scale;
    const double eq_step = 44.0 * card_scale;
    const double eq_y    = cy + 162.0 * card_scale;

    for (int i = 0; i < active_count; ++i) {
        const double y = cy + (i - (active_count - 1) * 0.5) * ab_step;
        position_button_clip(active_btns[i], true, icon_s, (float)left_x, (float)y);
    }
    for (int i = 0; i < passive_count; ++i) {
        const double y = cy + (i - (passive_count - 1) * 0.5) * pa_step;
        position_button_clip(passive_btns[i], true, icon_s, (float)right_x, (float)y);
    }
    for (int i = 0; i < item_count; ++i) {
        const double x = cx + (i - (item_count - 1) * 0.5) * eq_step;
        position_button_clip(item_btns[i], true, icon_s, (float)x, (float)eq_y);
    }

    return true;
}

bool gameui_add_combat_bar(GameUiOverlay& ov, GameUiCombatBar& out) {
    out = {};
    if (!gameui_is_alive(ov) || !ensure_resolved() || !g_fn.combat_bar_ok)
        return false;

    void* ent = seh_create_entity(ov.scene);
    if (!ent) return false;
    void* ren = seh_create_renderer(ov.scene, ent, "CombatMenu");
    if (!ren) return false;

    // Stop CombatMenu's root timeline on a fixed layout frame ("morepassives"
    // shows 4 spells + 4 passives + 5 equipment slots) so it doesn't loop
    // through all layout frames at 60 fps and reset child button clips.
    if (!gameui_goto_label(ren, nullptr, "morepassives"))
        gameui_goto_frame(ren, nullptr, 0);

    set_renderer_layer(ren, 41);

    void* tr = nullptr;
    mem_read((const uint8_t*)ren + kRenderer_Transform, &tr, sizeof(tr));

    void* panel = nullptr;
    __try {
        panel = g_fn.add_menupanel(ent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        panel = nullptr;
    }
    if (!panel) return false;

    void* tt_ent = seh_create_entity(ov.scene);
    void* tt_comp = nullptr;
    if (tt_ent) {
        __try {
            tt_comp = g_fn.create_ab_tt(ov.scene, tt_ent);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            tt_comp = nullptr;
        }
    }

    out.entity       = ent;
    out.renderer     = ren;
    out.transform    = tr;
    out.panel        = panel;
    out.tooltip_comp = tt_comp;

    out.btn_attack = seh_panel_get_button(panel, "bAttack");
    out.btn_move   = seh_panel_get_button(panel, "bMove");
    const char* spell_names[4]   = { "bSpell0", "bSpell1", "bSpell2", "bSpell3" };
    const char* passive_names[4] = { "bPassive0", "bPassive1", "bPassive2", "bPassive3" };
    const char* item_names[5]    = { "bItem0", "bItem1", "bItem2", "bItem3", "bItem4" };
    for (int i = 0; i < 4; ++i) out.btn_spell[i]   = seh_panel_get_button(panel, spell_names[i]);
    for (int i = 0; i < 4; ++i) out.btn_passive[i] = seh_panel_get_button(panel, passive_names[i]);
    for (int i = 0; i < 5; ++i) out.btn_item[i]    = seh_panel_get_button(panel, item_names[i]);

    // Hide battle-only / unused slots.
    const char* hide_names[] = { "bEnd", "bBonusSpell", "bPassive4", "bPassive5", "bPassive6" };
    for (const char* hn : hide_names)
        gameui_set_child_visible(ren, hn, false);

    return true;
}

void gameui_combat_bar_show_cat(GameUiOverlay& ov, GameUiCombatBar& bar,
                                void* cat_data, GameUiCatKit& kit) {
    if (!gameui_is_alive(ov) || !bar.panel || !cat_data || !g_fn.combat_bar_ok)
        return;

    void* spawndb = get_live_spawndb();

    // Lazily create VirtualCharacter + Ability* handles once per cat card.
    if (!kit.built) {
        kit.built = true;
        __try {
            kit.vchar = g_fn.create_vchar(ov.scene, cat_data);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            kit.vchar = nullptr;
        }
        if (kit.vchar) {
            // Basic attack (respects weapon overrides).
            GameStr atk_s{};
            memset(atk_s.raw, 0, sizeof(atk_s.raw));
            *(uint64_t*)(atk_s.raw + 24) = 15;
            bool got_atk = false;
            __try {
                g_fn.cat_basic_atk(cat_data, atk_s.raw);
                got_atk = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (got_atk) {
                char atk_name[96] = {};
                if (gameui_read_string(atk_s.raw, atk_name, sizeof(atk_name)))
                    kit.ab_attack = seh_vchar_add_ability(kit.vchar, atk_name);
                destroy_game_str(atk_s);
            }

            // Move ability.
            char move_name[96] = {};
            if (gameui_read_string((const uint8_t*)cat_data + kCatData_Abilities + kCatAbilities_Move,
                                   move_name, sizeof(move_name)))
                kit.ab_move = seh_vchar_add_ability(kit.vchar, move_name);

            // 4 active spells.
            for (int i = 0; i < 4; ++i) {
                char sp_name[96] = {};
                if (gameui_read_string((const uint8_t*)cat_data + kCatData_Abilities + kCatAbilities_Spells + i * 32,
                                       sp_name, sizeof(sp_name)))
                    kit.ab_spell[i] = seh_vchar_add_ability(kit.vchar, sp_name);
            }
        }
    }

    populate_ability_slot(bar.btn_attack, kit.ab_attack, 10);
    populate_ability_slot(bar.btn_move,   kit.ab_move,   9);
    for (int i = 0; i < 4; ++i)
        populate_ability_slot(bar.btn_spell[i], kit.ab_spell[i], i);

    for (int i = 0; i < 4; ++i) {
        const uint8_t* p = (const uint8_t*)cat_data + kCatData_Abilities + kCatAbilities_Passives + i * kCatPassive_Size;
        char pname[96] = {};
        int32_t level = 1;
        gameui_read_string(p, pname, sizeof(pname));
        mem_read(p + kCatPassive_Level, &level, sizeof(level));
        populate_passive_slot(bar.btn_passive[i], spawndb, cat_data, pname, level, i);
    }

    for (int i = 0; i < (int)kCatData_EquipmentSlots; ++i) {
        uint8_t* eq = (uint8_t*)cat_data + kCatData_Equipment + i * kEquipment_Size;
        populate_item_slot(bar.btn_item[i], spawndb, eq);
    }
}

} // namespace mgmp
