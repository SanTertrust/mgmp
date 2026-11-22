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

struct FnTable {
    bool              checked = false;
    bool              ok      = false;
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

    g_fn.ok = g_fn.add_scene && g_fn.create_entity && g_fn.create_rc &&
              g_fn.create_cam && g_fn.create_renderer_empty && g_fn.renderer_init &&
              g_fn.create_catparts &&
              g_fn.find_child && g_fn.tb_set_text && g_fn.tb_set_color &&
              g_fn.mc_goto_stop && g_fn.cam_get_mouse &&
              g_fn.str_assign && g_fn.wstr_assign && g_fn.str_dtor;
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

} // namespace mgmp
