#pragma once
// mgmp_gameui -- reusable native Mewgenics UI overlay toolkit.
//
// Builds interactive overlays directly on top of the game's own Scene /
// RenderCore / Camera / Renderer (SWF) / CatParts / DynamicTextBox engine
// instead of ImGui. Coordinates live in the standard 1280x720 virtual UI space
// (Camera at (640, 360), pixels_per_unit = 1.0, y_direction = 1), so layout
// and mouse hit-testing automatically match the game's aspect ratio and
// letterboxing at any window resolution or DPI scale.

#include <cstdint>

namespace mgmp {

struct GameUiColor {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct GameUiOverlay {
    static constexpr uint32_t kMaxBlockedScenes = 16;

    void*    director  = nullptr;
    void*    scene     = nullptr;
    void*    camera    = nullptr;
    void*    blocked[kMaxBlockedScenes] = {};
    uint32_t blocked_count = 0;
    bool     open      = false;
};

struct GameUiClip {
    void* entity    = nullptr;
    void* renderer  = nullptr;
    void* transform = nullptr;
};

struct GameUiLabel {
    void* entity    = nullptr;
    void* renderer  = nullptr;
    void* transform = nullptr;
    void* textbox   = nullptr;   // cached swf::DynamicTextBox* ("catname")
};

struct GameUiCat {
    void* entity    = nullptr;
    void* renderer  = nullptr;
    void* catparts  = nullptr;
    void* transform = nullptr;
};

// Virtual UI canvas dimensions (matching YesNoPrompt / native menus).
constexpr double kGameUiWidth  = 1280.0;
constexpr double kGameUiHeight = 720.0;

// True when all native engine call targets resolved and MewDirector is live.
bool gameui_available();

// Open a native overlay Scene on top of the current game scenes and block
// input on underlying scenes until gameui_close() is called.
bool gameui_open(GameUiOverlay& ov, const char* scene_name,
                 double sort_depth = 999990.0);

// Close the overlay Scene, mark it for destruction, and restore input on the
// underlying scenes that were blocked by gameui_open().
void gameui_close(GameUiOverlay& ov);

// True if ov.open is set and ov.scene is still alive in Director's scene list.
bool gameui_is_alive(const GameUiOverlay& ov);

// Spawn any SWF clip from swfs/ui.swf (or loaded SWFs) on the overlay scene.
bool gameui_add_clip(GameUiOverlay& ov, const char* clip_name,
                     double x, double y, double sx, double sy,
                     int32_t layer, double alpha, GameUiClip& out);

// Spawn a native game-font text label (backed by "HouseNametag" -> "catname").
bool gameui_add_label(GameUiOverlay& ov, const wchar_t* text,
                      double x, double y, double scale, int32_t layer,
                      GameUiColor color, GameUiLabel& out);

// Spawn a live CatParts visual for a CatData* ("CatTest" + CatParts).
bool gameui_add_cat(GameUiOverlay& ov, void* cat_data,
                    double x, double y, double scale, int32_t layer,
                    GameUiCat& out);

// Transform / visibility / text / frame helpers.
void gameui_set_pos(void* transform, double x, double y, double sort_depth = 0.0);
void gameui_set_scale(void* transform, double sx, double sy);
void gameui_set_visible(void* renderer, bool visible);
void gameui_set_alpha(void* renderer, double alpha);
void gameui_set_child_visible(void* renderer, const char* child_name, bool visible);
void gameui_set_child_text(void* renderer, const char* child_name, const wchar_t* text);
void gameui_set_child_color(void* renderer, const char* child_name, GameUiColor color);
void gameui_goto_frame(void* renderer, const char* child_name, int32_t frame);
bool gameui_goto_label(void* renderer, const char* child_name, const char* label);

void gameui_set_label_text(GameUiLabel& lbl, const wchar_t* text);
void gameui_set_label_color(GameUiLabel& lbl, GameUiColor color);

// Mouse position in the overlay's 1280x720 virtual UI coordinate space.
bool gameui_get_mouse(const GameUiOverlay& ov, double& mx, double& my);

// Axis-aligned rectangle hit-test centered at (cx, cy) with size (w, h).
inline bool gameui_hit_rect(double mx, double my,
                            double cx, double cy, double w, double h) {
    return mx >= cx - w * 0.5 && mx <= cx + w * 0.5 &&
           my >= cy - h * 0.5 && my <= cy + h * 0.5;
}

// Guarded readers for MSVC std::string / std::wstring inside game structs.
bool gameui_read_string(const void* game_str, char* out, uint32_t cap);
bool gameui_read_wstring(const void* game_wstr, wchar_t* out, uint32_t cap);

} // namespace mgmp
