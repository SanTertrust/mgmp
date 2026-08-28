// mgmp_overlay.cpp -- see mgmp_overlay.h for why this draws in screen space
// with GL rather than through the game's immediate-mode board UI.
#include "mgmp_overlay.h"

#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_gpak.h"
#include "mgmp_cursor.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_ui.h"

#include <windows.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// EVERYTHING THIS MODULE SAYS WHEN IT IS WORKING IS TRACE.
//
// The peer pointer either draws or it does not, and the working case had
// fifteen lines describing window sizes, letterbox bars, NDC quad corners,
// texture ink boxes and unpack state -- every one of them written while this
// was being built, none of them readable by a player, and together they were
// most of what the panel showed during a healthy session.
//
// They stay in the FILE, because they are exactly what a "why is nothing
// visible" report needs and they cost nothing to keep: log_line_lvl only sets
// the ring entry's severity, and every line still reaches disk. What changes is
// that the panel's default view no longer shows them.
//
// The failures are deliberately NOT routed through here. They keep log_line and
// their `!!` prefix, so the classifier goes on grading them Warn or Error --
// a shader that will not compile or a texture that uploads to nothing is the
// one thing in this module worth interrupting someone for.
template <class... A>
void trace(const char* fmt, A... a) {
    log_line_lvl(LogLevel::Trace, "OVERLAY", fmt, a...);
}

// --- the slice of OpenGL this needs -----------------------------------------
//
// Declared by hand rather than pulled from a GL header: everything past 1.1 has
// to be resolved through wglGetProcAddress anyway, so a header would buy only
// the enum names, and the ones used here are spelled out below.
typedef unsigned int  GLenum;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned char GLboolean;
typedef char          GLchar;
typedef float         GLfloat;
typedef ptrdiff_t     GLsizeiptr;

constexpr GLenum GL_TRIANGLES          = 0x0004;
constexpr GLenum GL_BLEND              = 0x0BE2;
constexpr GLenum GL_SRC_ALPHA          = 0x0302;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA= 0x0303;
constexpr GLenum GL_DEPTH_TEST         = 0x0B71;
constexpr GLenum GL_CULL_FACE          = 0x0B44;
constexpr GLenum GL_SCISSOR_TEST       = 0x0C11;
constexpr GLenum GL_VIEWPORT           = 0x0BA2;
constexpr GLenum GL_ARRAY_BUFFER       = 0x8892;
constexpr GLenum GL_STATIC_DRAW        = 0x88E4;
constexpr GLenum GL_FRAGMENT_SHADER    = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER      = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS     = 0x8B81;
constexpr GLenum GL_LINK_STATUS        = 0x8B82;
constexpr GLenum GL_FLOAT              = 0x1406;
constexpr GLenum GL_FALSE              = 0;
constexpr GLenum GL_CURRENT_PROGRAM    = 0x8B8D;
constexpr GLenum GL_ARRAY_BUFFER_BINDING = 0x8894;
constexpr GLenum GL_VERTEX_ARRAY_BINDING = 0x85B5;
constexpr GLenum GL_FRAMEBUFFER        = 0x8D40;
constexpr GLenum GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum GL_TEXTURE_2D         = 0x0DE1;
constexpr GLenum GL_TEXTURE0           = 0x84C0;
constexpr GLenum GL_TEXTURE_BINDING_2D = 0x8069;
constexpr GLenum GL_ACTIVE_TEXTURE     = 0x84E0;
constexpr GLenum GL_RGBA               = 0x1908;
constexpr GLenum GL_UNSIGNED_BYTE      = 0x1401;
constexpr GLenum GL_LINEAR             = 0x2601;
constexpr GLenum GL_CLAMP_TO_EDGE      = 0x812F;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_WRAP_S     = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T     = 0x2803;
constexpr GLenum GL_DYNAMIC_DRAW       = 0x88E8;

// Everything the upload has to neutralise before it can trust glTexImage2D --
// see the note in load_art.
constexpr GLenum GL_PIXEL_UNPACK_BUFFER         = 0x88EC;
constexpr GLenum GL_PIXEL_UNPACK_BUFFER_BINDING = 0x88EF;
constexpr GLenum GL_UNPACK_SWAP_BYTES    = 0x0CF0;
constexpr GLenum GL_UNPACK_LSB_FIRST     = 0x0CF1;
constexpr GLenum GL_UNPACK_ROW_LENGTH    = 0x0CF2;
constexpr GLenum GL_UNPACK_SKIP_ROWS     = 0x0CF3;
constexpr GLenum GL_UNPACK_SKIP_PIXELS   = 0x0CF4;
constexpr GLenum GL_UNPACK_ALIGNMENT     = 0x0CF5;
constexpr GLenum GL_UNPACK_SKIP_IMAGES   = 0x806D;
constexpr GLenum GL_UNPACK_IMAGE_HEIGHT  = 0x806E;

struct GL {
    // GL 1.1, straight out of opengl32.dll.
    void     (__stdcall* Enable)(GLenum);
    void     (__stdcall* Disable)(GLenum);
    GLboolean(__stdcall* IsEnabled)(GLenum);
    void     (__stdcall* GetIntegerv)(GLenum, GLint*);
    void     (__stdcall* BlendFunc)(GLenum, GLenum);
    void     (__stdcall* DrawArrays)(GLenum, GLint, GLsizei);
    void     (__stdcall* Viewport)(GLint, GLint, GLsizei, GLsizei);
    GLenum   (__stdcall* GetError)();
    void     (__stdcall* GenTextures)(GLsizei, GLuint*);
    void     (__stdcall* BindTexture)(GLenum, GLuint);
    void     (__stdcall* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                                     GLenum, GLenum, const void*);
    void     (__stdcall* TexParameteri)(GLenum, GLenum, GLint);
    void     (__stdcall* PixelStorei)(GLenum, GLint);
    void     (__stdcall* GetTexImage)(GLenum, GLint, GLenum, GLenum, void*);

    // Everything below is 2.0+/3.0+ and must come from wglGetProcAddress.
    GLuint (__stdcall* CreateShader)(GLenum);
    void   (__stdcall* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    void   (__stdcall* CompileShader)(GLuint);
    void   (__stdcall* GetShaderiv)(GLuint, GLenum, GLint*);
    void   (__stdcall* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    void   (__stdcall* DeleteShader)(GLuint);
    GLuint (__stdcall* CreateProgram)();
    void   (__stdcall* AttachShader)(GLuint, GLuint);
    void   (__stdcall* LinkProgram)(GLuint);
    void   (__stdcall* GetProgramiv)(GLuint, GLenum, GLint*);
    void   (__stdcall* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    void   (__stdcall* UseProgram)(GLuint);
    GLint  (__stdcall* GetUniformLocation)(GLuint, const GLchar*);
    void   (__stdcall* Uniform2f)(GLint, GLfloat, GLfloat);
    void   (__stdcall* Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    void   (__stdcall* GenBuffers)(GLsizei, GLuint*);
    void   (__stdcall* BindBuffer)(GLenum, GLuint);
    void   (__stdcall* BufferData)(GLenum, GLsizeiptr, const void*, GLenum);
    void   (__stdcall* GenVertexArrays)(GLsizei, GLuint*);
    void   (__stdcall* BindVertexArray)(GLuint);
    void   (__stdcall* EnableVertexAttribArray)(GLuint);
    void   (__stdcall* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    GLboolean(__stdcall* IsProgram)(GLuint);
    void   (__stdcall* BindFramebuffer)(GLenum, GLuint);
    void   (__stdcall* ActiveTexture)(GLenum);
    void   (__stdcall* Uniform1i)(GLint, GLint);
    void   (__stdcall* Uniform1f)(GLint, GLfloat);
    void   (__stdcall* BufferSubData)(GLenum, GLsizeiptr, GLsizeiptr, const void*);
    void   (__stdcall* BindAttribLocation)(GLuint, GLuint, const GLchar*);
} gl;

typedef void* (__stdcall* fn_wglGetProcAddress)(const char*);

// SDL_GL_SwapWindow(SDL_Window*) -> bool. Ours replaces the jump-table entry
// and calls whatever was there before, which is the real implementation.
typedef int (__cdecl* fn_sdl_swap)(void* window);

// A NOTE ON WHERE THE DRAWABLE SIZE COMES FROM, because two obvious sources
// were both wrong.
//
// SDL_GetWindowSize cost a crash: SDL is built with its dynamic-API shim, so
// the address behind an SDL_* thunk statically is a DEFAULT stub that re-reads
// its OWN table slot -- resolving one by reading a neighbouring slot yields a
// stub belonging to a DIFFERENT function whose prologue is identical. It passed
// the pinned-build check, was called with (window, &w, &h), and threw
// std::bad_alloc from inside the game's allocator on the first frame.
//
// GetClientRect(WindowFromDC(wglGetCurrentDC())) cost a silent miss: it
// reported 958x1120 against a real framebuffer of 958x539, so the overlay set a
// viewport twice as tall as the buffer and every arrow landed off the bottom
// edge. Whatever window that DC resolves to, it is not the one being presented.
//
// What is right is the viewport the game has ALREADY SET, read with
// glGetIntegerv(GL_VIEWPORT). It is the drawable by definition -- it is what
// the frame in front of you was rendered through -- it needs no SDL and no
// window handle, and taking it means the overlay never calls glViewport at all.


// --- the cursor art --------------------------------------------------------
//
// The game's OWN textures, read out of resources.gpak at runtime rather than
// embedded: textures/cursor/<state>.png, the same files SetCursor elects
// between. So a peer's pointer is pixel-for-pixel the pointer they are looking
// at, and a game update that redraws the cursor updates ours with it.
//
// The hand-built arrow this replaced was a fair approximation and still visibly
// not the same shape. There was never a good reason for it beyond "decoding a
// PNG is work" -- which stb_image answers in one header.
//
// STATE ORDER IS THE WIRE FORMAT. A peer sends an index into this table, so
// reordering it is a protocol change even though no byte moves. Adding to the
// END is safe; an index we do not know draws `default`.
struct CursorArt {
    const char* state;      // the name SetCursor uses, and the .png stem
    float hot_x, hot_y;     // hotspot in texels -- see below
};

// hotspots.gon lists only the EXCEPTIONS: default, grab, grabr and the four pet
// frames. Everything else shares default's (34,7), which the pixels agree with
// -- attack, spell, move, invalid, examine, question and heal all have their
// alpha bounding box starting at exactly (29,2), i.e. they are the same arrow
// with a different badge hanging off it.
constexpr float kHotDefaultX = 34.0f, kHotDefaultY = 7.0f;

const CursorArt kArt[] = {
    { "default",           kHotDefaultX, kHotDefaultY },   // 0
    { "btn_over",          kHotDefaultX, kHotDefaultY },   // 1
    { "attack",            kHotDefaultX, kHotDefaultY },   // 2
    { "attack_hastargets", kHotDefaultX, kHotDefaultY },   // 3
    { "spell",             kHotDefaultX, kHotDefaultY },   // 4
    { "spell_hastargets",  kHotDefaultX, kHotDefaultY },   // 5
    { "move",              kHotDefaultX, kHotDefaultY },   // 6
    { "move_hastargets",   kHotDefaultX, kHotDefaultY },   // 7
    { "invalid",           kHotDefaultX, kHotDefaultY },   // 8
    { "heal",              kHotDefaultX, kHotDefaultY },   // 9
    { "heal_hastargets",   kHotDefaultX, kHotDefaultY },   // 10
    { "examine",           kHotDefaultX, kHotDefaultY },   // 11
    { "question",          kHotDefaultX, kHotDefaultY },   // 12
    { "grab",              17.0f,        58.0f        },   // 13  (hotspots.gon)
    { "grabr",             110.0f,       58.0f        },   // 14  (hotspots.gon)
    { "pet_frame1",        32.0f,        52.0f        },   // 15  (hotspots.gon)
};
constexpr uint8_t kArtCount = (uint8_t)(sizeof(kArt) / sizeof(kArt[0]));

// One loaded texture. Loaded LAZILY, on the first frame a peer actually shows
// that state: a session that never hovers a button never reads btn_over.png,
// and a failed load is remembered so a missing file is not re-read every frame.
struct Art {
    GLuint tex = 0;
    float  tw = 128, th = 128;      // the image, for turning texels into UVs
    bool   tried = false;
    // The tight alpha bounding box, in texels. The PNGs are 128x128 with a lot
    // of transparent padding, so drawing the full square would make the visible
    // glyph a fraction of the requested size and put the hotspot in the wrong
    // place. Cropping to the ink is what makes net_cursor_px mean what it says.
    float x0 = 0, y0 = 0, x1 = 128, y1 = 128;
};

const char* kVertexSrc =
    "#version 150\n"
    "in vec2 aPos;\n"            // ALREADY IN NDC -- see below
    "in vec2 aUV;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "  vUV = aUV;\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

// The vertex shader takes NDC DIRECTLY, and the texel -> NDC arithmetic
// happens on the CPU.
//
// It used to pass a hotspot origin and a scale as uniforms, which is the
// obvious shape and cost a debugging session: the untextured test box drew
// correctly with uOrigin 0 / uScale 1 while every real cursor -- textured
// or not -- drew nothing, which narrows the fault to those two uniforms and
// nothing else. Rather than keep bisecting a two-line transform, the
// transform moved to where it can be logged, asserted and read: six
// vertices of plain NDC, computed in C++.
//
// It is also strictly less work per frame -- the same arithmetic once on
// the CPU instead of once per vertex -- and it leaves the shader with
// nothing that can be wrong except the sampler.

// The art is white with a black outline, so a straight multiply tints the fill
// and leaves the outline black -- exactly the look wanted, and it needs no
// separate outline pass. Alpha carries the shape.
const char* kFragmentSrc =
    "#version 150\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec4 uColour;\n"
    // 0 draws a flat quad, ignoring the sampler. It is for the test
    // marker: "the overlay draws nothing" and "the overlay draws a
    // texture that samples to zero alpha" look identical on screen and
    // have nothing in common as bugs.
    "uniform float uUseTex;\n"
    "out vec4 oColour;\n"
    "void main() {\n"
    "  vec4 t = mix(vec4(1.0), texture(uTex, vUV), uUseTex);\n"
    "  oColour = vec4(t.rgb * uColour.rgb, t.a * uColour.a);\n"
    "}\n";

// --- peer colours, matching mgmp_cursor's reticle ---------------------------
//
// Deliberately the same table: the pointer and the reticle are two views of one
// peer, and a partner whose arrow is gold but whose square is cyan would read
// as two different people.
const float kPeerRGB[kMaxPeers][3] = {
    { 1.00f, 0.80f, 0.20f },
    { 0.30f, 0.80f, 1.00f },
    { 0.45f, 1.00f, 0.45f },
    { 1.00f, 0.40f, 0.85f },
};

constexpr uint64_t kStaleMs = 3000;   // as in mgmp_cursor: a quiet peer is gone

struct Peer {
    // Where the peer says it is, and where we are currently drawing it. The
    // two are separate because CURSOR is throttled to at most one message every
    // 50 ms and heartbeats at 500 ms, so the reported position steps a few
    // times a second while we draw at whatever the frame rate is. Following the
    // target directly renders that stepping faithfully, which reads as a peer
    // whose hand is shaking.
    float    nx = 0, ny = 0;         // the target, straight off the wire
    float    cx = 0, cy = 0;         // what is actually drawn, chasing it
    uint8_t  mode = 0;               // which cursor art, an index into kArt
    bool     placed = false;         // cx/cy are meaningful (no lerp from 0,0)
    uint8_t  owns_turn = 0;
    uint64_t at = 0;
    bool     have = false;
};

struct State {
    bool on       = false;
    bool resolved = false;      // the GL entry points are all present
    bool built    = false;      // program/VAO/VBO exist in the CURRENT context
    bool said_fail = false;

    // Said-once markers. Between them these answer "which half is missing"
    // without a debugger: the hook firing, a peer's position arriving, and the
    // draw actually being reached are three separate things and any one of them
    // failing looks identical on screen.
    bool said_swap  = false;
    bool said_msg   = false;
    bool said_draw  = false;
    // The derived content rectangle disagreed with the game's own viewport --
    // said once, because it is either always true for this window or never.
    bool said_aspect = false;
    bool said_quiet = false;

    uintptr_t base = 0;

    // The jump-table slot we took over, and what it held before. Restoring the
    // saved value on shutdown is the whole of "uninstall".
    void**      swap_slot = nullptr;
    fn_sdl_swap swap_prev = nullptr;

    GLuint prog = 0, vao = 0, vbo = 0;
    GLint  u_colour = -1, u_tex = -1, u_usetex = -1;

    Art art[kArtCount];

    Peer peers[kMaxPeers];

    float   nx = 0, ny = 0;
    // The window in the MOUSE's own units, asked of SDL every frame. See
    // mouse_space() -- this is the divisor the fraction is taken against, and
    // getting it from the viewport instead is what put every pointer at half
    // height on a scaled display.
    int     mouse_w = 0, mouse_h = 0;
    // The last shape described in the log, so a resize says so and nothing else
    // does. -1 means "nothing said yet".
    int     said_w = -1, said_h = -1, said_vw = -1, said_vh = -1;
    // A bounded trace of the fraction, both ends. Off unless net_cursor_trace
    // is set, capped so a session cannot fill a log with mouse movement, and
    // it prints the RAW inputs next to the result -- the whole question is
    // which of the three numbers the factor enters at.
    uint32_t traced_local = 0, traced_peer = 0;
    float    last_traced_y = -9;
    uint8_t mode = 0;
    bool    have_local = false;
    // One bit per kArt entry: which cursor states this session has already
    // named in the log. A bitmask rather than a "changed since last frame"
    // test because the states TOGGLE -- hovering one button walks
    // btn_over/default/btn_over/... for as long as the mouse sits there, and
    // logging every transition buried real lines under hundreds of them. The
    // question was always which states a session reaches, and that is answered
    // once per state.
    uint32_t seen_states = 0;

    // For the smoothing step. QPC rather than GetTickCount64 because the gap
    // between two swaps is a frame, and a 16 ms tick has no resolution to spare
    // at that scale.
    int64_t qpc_freq = 0;
    int64_t qpc_last = 0;

    uint32_t drawn = 0;

    // Test-mode only: draw the cursor quad without sampling, to separate a bad
    // sampler from a badly placed quad.
    bool debug_flat = false;
    int  said_quad  = 0;      // logs the first few quads, not just the first:
                              // the test draws two and only comparing them
                              // says whether the difference is the geometry.
} g;

const Config& cfg() { return config(); }

int __cdecl overlay_swap_detour(void* window);

// How far to move a peer's drawn position toward its reported one this frame.
//
// Exponential smoothing with a time constant, NOT a fixed per-frame fraction:
// `1 - exp(-dt/tau)` gives the same visible easing at 30 fps and 240 fps,
// whereas a constant like 0.2-per-frame is four times faster on the faster
// machine. Two peers running at different frame rates is the normal case here
// -- one measured battle ran 23,211 frames against 12,230 -- so a
// framerate-dependent smoother would look like a different feature on each
// screen.
float smoothing_alpha(double dt) {
    const double tau = (double)tune::kCursorSmoothMs / 1000.0;
    if (tau <= 0.0 || dt <= 0.0) return 1.0f;      // smoothing off: snap
    if (dt > 0.25) return 1.0f;                    // a long stall (a load, a
                                                   // breakpoint) should catch
                                                   // up, not glide across
    return (float)(1.0 - exp(-dt / tau));
}

double alpha_for(bool owns_turn) {
    uint32_t pct = owns_turn ? tune::kCursorAlpha : tune::kCursorAlphaDim;
    if (pct > 100) pct = 100;
    return (double)pct / 100.0;
}

// --- resolving GL -----------------------------------------------------------

bool resolve_gl() {
    HMODULE ogl = GetModuleHandleA("opengl32.dll");
    if (!ogl) ogl = LoadLibraryA("opengl32.dll");
    if (!ogl) { log_line("OVERLAY", "!! opengl32.dll is not loaded"); return false; }

    auto wgl = (fn_wglGetProcAddress)GetProcAddress(ogl, "wglGetProcAddress");
    if (!wgl) { log_line("OVERLAY", "!! wglGetProcAddress did not resolve"); return false; }

    // GL 1.1 lives in the DLL's export table; everything newer is an extension
    // entry point that only exists once a context is current -- which it is,
    // because this runs inside the swap.
    struct Bind { void** slot; const char* name; bool modern; };
    const Bind binds[] = {
        { (void**)&gl.Enable,        "glEnable",        false },
        { (void**)&gl.Disable,       "glDisable",       false },
        { (void**)&gl.IsEnabled,     "glIsEnabled",     false },
        { (void**)&gl.GetIntegerv,   "glGetIntegerv",   false },
        { (void**)&gl.BlendFunc,     "glBlendFunc",     false },
        { (void**)&gl.DrawArrays,    "glDrawArrays",    false },
        { (void**)&gl.Viewport,      "glViewport",      false },
        { (void**)&gl.GetError,      "glGetError",      false },
        { (void**)&gl.GenTextures,   "glGenTextures",   false },
        { (void**)&gl.BindTexture,   "glBindTexture",   false },
        { (void**)&gl.TexImage2D,    "glTexImage2D",    false },
        { (void**)&gl.TexParameteri, "glTexParameteri", false },
        { (void**)&gl.PixelStorei,   "glPixelStorei",   false },
        { (void**)&gl.GetTexImage,   "glGetTexImage",   false },

        { (void**)&gl.CreateShader,  "glCreateShader",  true },
        { (void**)&gl.ShaderSource,  "glShaderSource",  true },
        { (void**)&gl.CompileShader, "glCompileShader", true },
        { (void**)&gl.GetShaderiv,   "glGetShaderiv",   true },
        { (void**)&gl.GetShaderInfoLog, "glGetShaderInfoLog", true },
        { (void**)&gl.DeleteShader,  "glDeleteShader",  true },
        { (void**)&gl.CreateProgram, "glCreateProgram", true },
        { (void**)&gl.AttachShader,  "glAttachShader",  true },
        { (void**)&gl.LinkProgram,   "glLinkProgram",   true },
        { (void**)&gl.GetProgramiv,  "glGetProgramiv",  true },
        { (void**)&gl.GetProgramInfoLog, "glGetProgramInfoLog", true },
        { (void**)&gl.UseProgram,    "glUseProgram",    true },
        { (void**)&gl.GetUniformLocation, "glGetUniformLocation", true },
        { (void**)&gl.Uniform2f,     "glUniform2f",     true },
        { (void**)&gl.Uniform4f,     "glUniform4f",     true },
        { (void**)&gl.GenBuffers,    "glGenBuffers",    true },
        { (void**)&gl.BindBuffer,    "glBindBuffer",    true },
        { (void**)&gl.BufferData,    "glBufferData",    true },
        { (void**)&gl.GenVertexArrays, "glGenVertexArrays", true },
        { (void**)&gl.BindVertexArray, "glBindVertexArray", true },
        { (void**)&gl.EnableVertexAttribArray, "glEnableVertexAttribArray", true },
        { (void**)&gl.VertexAttribPointer, "glVertexAttribPointer", true },
        { (void**)&gl.IsProgram,     "glIsProgram",     true },
        { (void**)&gl.BindFramebuffer, "glBindFramebuffer", true },
        { (void**)&gl.ActiveTexture, "glActiveTexture", true },
        { (void**)&gl.Uniform1i,     "glUniform1i",     true },
        { (void**)&gl.Uniform1f,     "glUniform1f",     true },
        { (void**)&gl.BufferSubData, "glBufferSubData", true },
        { (void**)&gl.BindAttribLocation, "glBindAttribLocation", true },
    };

    for (const Bind& b : binds) {
        void* p = b.modern ? wgl(b.name) : (void*)GetProcAddress(ogl, b.name);
        // A modern entry point can also live in the DLL on some drivers, and a
        // 1.1 one never lives behind wglGetProcAddress -- so fall back one way
        // only, and report the name that actually failed.
        if (!p && b.modern) p = (void*)GetProcAddress(ogl, b.name);
        if (!p) {
            log_line("OVERLAY", "!! %s did not resolve -- the peer pointer is OFF", b.name);
            return false;
        }
        *b.slot = p;
    }
    return true;
}

GLuint compile(GLenum kind, const char* src, const char* label) {
    GLuint sh = gl.CreateShader(kind);
    if (!sh) return 0;
    gl.ShaderSource(sh, 1, &src, nullptr);
    gl.CompileShader(sh);
    GLint ok = 0;
    gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[512] = {};
        gl.GetShaderInfoLog(sh, sizeof(info) - 1, nullptr, info);
        log_line("OVERLAY", "!! %s shader did not compile: %s", label, info);
        gl.DeleteShader(sh);
        return 0;
    }
    return sh;
}

// Builds the program, VAO and VBO in whatever context is current.
//
// Called again whenever `built` has been invalidated, which is how a context
// recreated by ApplicationBase::RefreshWindow (a resolution change) is
// survived: GL names are per-context, so the old ones become meaningless rather
// than wrong, and glIsProgram is what notices.
bool build() {
    GLuint vs = compile(GL_VERTEX_SHADER, kVertexSrc, "vertex");
    if (!vs) return false;
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentSrc, "fragment");
    if (!fs) { gl.DeleteShader(vs); return false; }

    g.prog = gl.CreateProgram();
    gl.AttachShader(g.prog, vs);
    gl.AttachShader(g.prog, fs);
    // BEFORE the link, and not optional: the VAO below hard-codes attribute 0
    // as position and 1 as UV, and "the compiler assigns them in declaration
    // order" is a convention, not a rule. Binding them costs two calls and
    // removes a failure that would show as a quad in the wrong place or a
    // texture sampled with position coordinates.
    gl.BindAttribLocation(g.prog, 0, "aPos");
    gl.BindAttribLocation(g.prog, 1, "aUV");
    gl.LinkProgram(g.prog);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);

    GLint ok = 0;
    gl.GetProgramiv(g.prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char info[512] = {};
        gl.GetProgramInfoLog(g.prog, sizeof(info) - 1, nullptr, info);
        log_line("OVERLAY", "!! program did not link: %s", info);
        g.prog = 0;
        return false;
    }

    g.u_colour = gl.GetUniformLocation(g.prog, "uColour");
    g.u_tex    = gl.GetUniformLocation(g.prog, "uTex");
    g.u_usetex = gl.GetUniformLocation(g.prog, "uUseTex");

    GLint prev_vao = 0, prev_vbo = 0;
    gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);

    // Six vertices of {x, y, u, v}, rewritten per draw: the quad's corners
    // depend on the crop box of whichever cursor state is being drawn, and
    // there are at most three of them on screen. A buffer update per pointer is
    // cheaper than a uniform scheme that would have to encode the same four
    // numbers anyway.
    gl.GenVertexArrays(1, &g.vao);
    gl.GenBuffers(1, &g.vbo);
    gl.BindVertexArray(g.vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, g.vbo);
    gl.BufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                           (const void*)(2 * sizeof(float)));

    gl.BindVertexArray((GLuint)prev_vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_vbo);

    trace("peer pointer ready -- GL program %u, textured quad",
          (unsigned)g.prog);
    return true;
}

// --- the art ----------------------------------------------------------------

// Loads textures/cursor/<state>.png into a GL texture, once, and measures the
// tight alpha box so the padding around the glyph can be cropped away.
//
// Failure is remembered rather than retried: a state whose file is missing
// would otherwise re-read a 5 GB archive's index every single frame.
bool load_art(uint8_t mode) {
    Art& a = g.art[mode];
    if (a.tried) return a.tex != 0;
    a.tried = true;

    char name[128];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "textures/cursor/%s.png", kArt[mode].state);

    uint8_t* png = nullptr;
    uint32_t png_size = 0;
    if (!gpak_read(name, &png, &png_size)) return false;

    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load_from_memory(png, (int)png_size, &w, &h, &ch, 4);
    free(png);
    if (!px || w <= 0 || h <= 0) {
        log_line("OVERLAY", "!! %s did not decode (%s)", name, stbi_failure_reason());
        if (px) stbi_image_free(px);
        return false;
    }

    // The tight box of anything visible. The threshold is deliberately low --
    // these are antialiased edges, and cropping at alpha > 128 would shave the
    // outline off.
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y) {
        const stbi_uc* row = px + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            if (row[x * 4 + 3] > 8) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < x0 || y1 < y0) { x0 = y0 = 0; x1 = w - 1; y1 = h - 1; }

    GLint prev_tex = 0;
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

    // WE ARE UPLOADING INTO SOMEONE ELSE'S GL STATE, AND glTexImage2D DOES NOT
    // COMPLAIN ABOUT THE TWO WAYS THAT GOES WRONG.
    //
    // 1. If a buffer is bound to GL_PIXEL_UNPACK_BUFFER -- which is exactly
    //    what an engine streaming textures through a PBO leaves bound -- then
    //    the `px` argument stops being a pointer and becomes an OFFSET into
    //    that buffer. The upload then succeeds, from the wrong memory, and
    //    glGetError stays clean. The result is a texture that samples to zero
    //    alpha: a quad drawn perfectly, containing nothing.
    // 2. The UNPACK_* pixel-store state is global, and a stale ROW_LENGTH,
    //    ALIGNMENT or SKIP_* shears or shifts the image with, again, no error.
    //
    // Both are invisible from anything else the overlay logs, which is how the
    // untextured green arrow could draw while the textured cyan one drew
    // nothing at all. So: unbind the PBO, put every unpack switch back to its
    // documented default, upload, then hand the state back exactly as found.
    GLint prev_pbo = 0;
    gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev_pbo);
    if (prev_pbo) gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    struct Store { GLenum e; GLint def; GLint prev; };
    Store store[] = {
        { GL_UNPACK_SWAP_BYTES,   0, 0 },
        { GL_UNPACK_LSB_FIRST,    0, 0 },
        { GL_UNPACK_ROW_LENGTH,   0, 0 },
        { GL_UNPACK_SKIP_ROWS,    0, 0 },
        { GL_UNPACK_SKIP_PIXELS,  0, 0 },
        { GL_UNPACK_ALIGNMENT,    4, 0 },   // 4 is right for tightly packed RGBA8
        { GL_UNPACK_SKIP_IMAGES,  0, 0 },
        { GL_UNPACK_IMAGE_HEIGHT, 0, 0 },
    };
    bool store_dirty = false;
    for (Store& s : store) {
        gl.GetIntegerv(s.e, &s.prev);
        if (s.prev != s.def) { gl.PixelStorei(s.e, s.def); store_dirty = true; }
    }

    gl.GetError();                       // start from a clean slate
    gl.GenTextures(1, &a.tex);
    gl.BindTexture(GL_TEXTURE_2D, a.tex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Clamped, not wrapped: the quad samples the crop box exactly, and a
    // filtered edge sample that wrapped would pull in the far side of the image.
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const GLenum uerr = gl.GetError();

    // The alpha under the hotspot, purely as evidence that the decode produced
    // pixels rather than a plausible-looking wall of zeroes.
    const int hx = (int)kArt[mode].hot_x, hy = (int)kArt[mode].hot_y;
    const int probe = (hx >= 0 && hx < w && hy >= 0 && hy < h)
                    ? (int)px[((size_t)hy * w + hx) * 4 + 3] : -1;

    // READ THE TEXTURE BACK, once per state. "The decoded buffer had alpha 255
    // at the hotspot" and "the TEXTURE has alpha 255 at the hotspot" are
    // different claims, and only the second one is about what the sampler will
    // see -- the whole class of bug above lives in the gap between them. The
    // GPU round trip is a stall, which is why it happens once per cursor state
    // at load time and never in the draw path.
    int readback = -1;
    if (!uerr && a.tex) {
        // The unpack state is neutral but PACK is a different set of switches;
        // a full-image read at alignment 4 with a 4-byte-per-pixel format is
        // immune to every one of them, which is why this reads the whole level
        // rather than trying to address a single texel.
        stbi_uc* back = (stbi_uc*)malloc((size_t)w * h * 4);
        if (back) {
            memset(back, 0xCD, (size_t)w * h * 4);   // so "unwritten" is visible
            gl.GetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);
            if (gl.GetError() == 0 && hx >= 0 && hx < w && hy >= 0 && hy < h)
                readback = (int)back[((size_t)hy * w + hx) * 4 + 3];
            free(back);
        }
    }

    gl.BindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    if (store_dirty)
        for (const Store& s : store)
            if (s.prev != s.def) gl.PixelStorei(s.e, s.prev);
    if (prev_pbo) gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)prev_pbo);

    stbi_image_free(px);

    if (uerr || a.tex == 0) {
        log_line("OVERLAY", "!! %s upload failed -- GL error 0x%04X, texture name %u",
                 name, (unsigned)uerr, (unsigned)a.tex);
        a.tex = 0;
        return false;
    }
    if (prev_pbo || store_dirty) {
        trace("the game had left unpack state set (pbo %d, store %s)"
              " -- neutralised for the upload",
              (int)prev_pbo, store_dirty ? "dirty" : "clean");
    }
    // A readback that disagrees with the decode is THE diagnosis, not a hint:
    // the pixels existed and the texture does not have them.
    if (readback != probe) {
        log_line("OVERLAY", "!! %s uploaded but did not read back -- decoded alpha %d"
                            " at the hotspot, texture has %d",
                 name, probe, readback);
    }

    a.x0 = (float)x0;       a.y0 = (float)y0;
    a.x1 = (float)(x1 + 1); a.y1 = (float)(y1 + 1);
    a.tw = (float)w;        a.th = (float)h;

    trace("loaded %s -- %dx%d tex %u, ink (%d,%d)-(%d,%d),"
          " hotspot (%.0f,%.0f) alpha there %d, read back %d",
          name, w, h, (unsigned)a.tex, x0, y0, x1, y1,
          kArt[mode].hot_x, kArt[mode].hot_y, probe, readback);
    return true;
}

// --- the local mouse --------------------------------------------------------

// The game caches SDL_GetMouseState's answer as two doubles. Reading the cache
// rather than calling SDL again means we see exactly the position the game is
// acting on this frame, including the frames where it deliberately reuses the
// previous one instead of asking.
// --- how big the window is, in the units the MOUSE is measured in -----------
//
// Read out of the SDL_DYNAPI jump table rather than called by address: the
// address behind an SDL_* thunk statically is the DEFAULT stub, the one
// SDL_InitDynamicAPI replaces, so it is the single address guaranteed NOT to be
// the function. The slot holds the real one. See kRva_SdlGetWindowSizeSlot.
typedef int (__cdecl* fn_sdl_get_window_size)(void* window, int* w, int* h);

fn_sdl_get_window_size sdl_size_fn(uint32_t rva) {
    if (!g.base) return nullptr;
    void* p = nullptr;
    if (!mem_read((const void*)(g.base + rva), &p, sizeof(p)) || !p) return nullptr;
    return (fn_sdl_get_window_size)p;
}

// The framebuffer, straight from SDL. This is what NDC has to be mapped onto,
// and it is NOT necessarily the viewport that happens to be bound when the swap
// comes round -- see overlay_on_swap.
bool drawable_size(void* window, int& w, int& h) {
    w = h = 0;
    if (!window) return false;
    static fn_sdl_get_window_size get_px = nullptr;
    static bool looked = false;
    if (!looked) { looked = true; get_px = sdl_size_fn(kRva_SdlGetWindowSizePxSlot); }
    if (!get_px) return false;
    get_px(window, &w, &h);
    return w > 0 && h > 0;
}

// The window in mouse units, with the viewport as the fallback and as the
// sanity check. `vw`/`vh` are the framebuffer, for the ratio in the log.
bool mouse_space(void* window, int vw, int vh, int& mw, int& mh) {
    mw = vw; mh = vh;
    if (!window) return false;

    static fn_sdl_get_window_size get_size = nullptr;
    static fn_sdl_get_window_size get_px   = nullptr;
    static bool looked = false;
    if (!looked) {
        looked   = true;
        get_size = sdl_size_fn(kRva_SdlGetWindowSizeSlot);
        get_px   = sdl_size_fn(kRva_SdlGetWindowSizePxSlot);
    }
    if (!get_size) return false;

    int lw = 0, lh = 0;
    get_size(window, &lw, &lh);
    // A size that is absurd, or wildly out of proportion with the framebuffer,
    // means we are not reading what we think we are -- and the old behaviour is
    // wrong by a factor, where a bad pointer is wrong by a crash. Take the
    // viewport and say nothing more about it.
    if (lw <= 0 || lh <= 0) return false;
    if (vw > 0 && vh > 0) {
        const double rx = (double)vw / (double)lw, ry = (double)vh / (double)lh;
        if (rx < 0.2 || rx > 8.0 || ry < 0.2 || ry > 8.0) return false;
    }

    mw = lw; mh = lh;

    // Said on CHANGE, not once: a resize is exactly the event that breaks this
    // arithmetic, so a log that can only describe the window's first shape is
    // silent about the case worth reading.
    if (g.said_w != lw || g.said_h != lh || g.said_vw != vw || g.said_vh != vh) {
        int pw = 0, ph = 0;
        if (get_px) get_px(window, &pw, &ph);
        trace("window %dx%d logical, %dx%d in pixels; content"
              " rectangle (the game's viewport) %dx%d -- letterbox"
              " bars %d wide, %d tall",
              lw, lh, pw, ph, vw, vh,
              pw > vw ? (pw - vw) / 2 : 0, ph > vh ? (ph - vh) / 2 : 0);
        g.said_w = lw; g.said_h = lh; g.said_vw = vw; g.said_vh = vh;
        // A resize is the event worth tracing, so give it a fresh budget --
        // otherwise the cap is spent on the shape the window started in.
        g.traced_local = 0;
        g.traced_peer  = 0;
        g.last_traced_y = -9;
    }
    return true;
}

bool read_mouse(double& x, double& y) {
    if (!g.base) return false;
    double xy[2] = { 0, 0 };
    const uintptr_t cache = addr_of_data(D_MouseCache);
    if (!cache) return false;
    if (!mem_read((const void*)cache, xy, sizeof(xy))) return false;
    x = xy[0]; y = xy[1];
    return true;
}

// One peer's pointer: the cropped glyph, positioned so its HOTSPOT lands on the
// reported point and sized so its ink is net_cursor_px tall.
// --- which cursor the game is showing US ------------------------------------

// The state name the engine published for this frame, at ApplicationBase+3392.
//
// glaiel::SetCursor(std::string state, int priority) @ 0x1409B09B0 keeps the
// highest-priority claim on the Cursor singleton, and the Cursor's own
// late-update @ 0x1409B0900 copies the winner here and resets the priority so
// the next frame re-elects from scratch. Reading that published copy needs no
// hook and does not care which of the ~18 callers won.
//
// It lives in the swap rather than in the battle HUD tick on purpose: the
// cursor changes on menus and the map too, and the pointer is drawn there.
bool read_cursor_state(char out[32]) {
    out[0] = 0;
    if (!g.base) return false;

    const uintptr_t app_slot = addr_of_data(D_ApplicationBase);
    if (!app_slot) return false;
    const void* app = nullptr;
    if (!mem_read((const void*)app_slot, &app, sizeof(app)) || !app)
        return false;

    const uint8_t* str = (const uint8_t*)app + kApp_CursorState;
    uint64_t size = 0, cap = 0;
    if (!mem_read(str + 16, &size, sizeof(size))) return false;
    if (!mem_read(str + 24, &cap,  sizeof(cap)))  return false;
    if (size > 31 || cap < size) return false;   // not a string; do not guess

    const void* chars = str;
    if (cap > 15 && !mem_read(str, &chars, sizeof(chars))) return false;
    if (!mem_read(chars, out, (size_t)size)) return false;
    out[size] = 0;
    return true;
}

// Exact match against the table, because the table IS the shipped file names.
// A state we do not carry art for falls back to the plain arrow rather than to
// nothing -- the peer is still pointing at something.
uint8_t mode_for_state(const char* state) {
    for (uint8_t i = 0; i < kArtCount; ++i)
        if (strcmp(state, kArt[i].state) == 0) return i;
    return 0;
}

void draw_cursor(uint8_t mode, float nx, float ny, const float rgb[3], float alpha,
                 int win_w, int win_h) {
    // The state-specific art is off while the aiming drift is isolated: see
    // tune::kPeerCursorArt. `mode` still crosses the wire and is still stored,
    // so this is a switch rather than a removal.
    if (!tune::kPeerCursorArt) mode = 0;
    if (mode >= kArtCount) mode = 0;
    if (!load_art(mode)) {
        if (mode == 0) return;                 // no fallback left
        mode = 0;                              // an unknown state still points
        if (!load_art(0)) return;
    }
    const Art& a = g.art[mode];

    // Window fraction -> NDC. The y flip is here and only here: everything
    // above this line is in screen convention (y down), everything below is GL.
    const float ox = nx * 2.0f - 1.0f;
    const float oy = 1.0f - ny * 2.0f;

    // Texels -> NDC, sized so the INK is the configured height. Using the ink
    // rather than the 128x128 image is the difference between a cursor the
    // requested size and one about a third of it: the art is mostly padding.
    // ...and SCALED WITH THE CONTENT RECTANGLE, so the pointer grows with the
    // window the way the game's own cursor does. The content is always the same
    // aspect -- that is what the letterbox bars are for -- so its height is the
    // whole scale factor, and net_cursor_px is the size at net_cursor_ref_h.
    // A fixed pixel size would have the arrow shrink to a speck as the window
    // grew, which is the one thing it must not do on a shared screen.
    // A FIXED REFERENCE, NOT THIS STATE'S OWN INK HEIGHT.
    //
    // It used to be `a.y1 - a.y0`, and that made the pointer's geometry depend
    // on which cursor the peer was showing: every aiming state is 11-19% taller
    // than `default` because the badge hangs below the arrow (the table in
    // mgmp_tuning.h has the measurements), so k dropped and the whole glyph
    // contracted toward the hotspot the instant the peer started aiming.
    //
    // Dividing by a constant instead means the arrow is the same size in every
    // state and the badge hangs off it, which is what it does in the game.
    const float ink_h = tune::kCursorInkRefH;
    const float ref   = (float)tune::kCursorRefH;
    const float grow  = (ref > 1.0f && win_h > 0) ? (float)win_h / ref : 1.0f;
    const float k     = (float)tune::kCursorPx * grow
                      / (ink_h > 1.0f ? ink_h : 1.0f);
    const float sx    =  k * 2.0f / (float)win_w;
    const float sy    = -k * 2.0f / (float)win_h;   // screen y is down

    // Corners in texels relative to the HOTSPOT, then straight into NDC, so the
    // hotspot lands on the reported point and the glyph hangs off it wherever
    // the artist put it.
    const float lx = ox + (a.x0 - kArt[mode].hot_x) * sx;
    const float rx = ox + (a.x1 - kArt[mode].hot_x) * sx;
    const float ty = oy + (a.y0 - kArt[mode].hot_y) * sy;
    const float by = oy + (a.y1 - kArt[mode].hot_y) * sy;
    const float u0 = a.x0 / a.tw, u1 = a.x1 / a.tw;
    const float v0 = a.y0 / a.th, v1 = a.y1 / a.th;

    const float quad[6][4] = {
        { lx, ty, u0, v0 }, { rx, ty, u1, v0 }, { rx, by, u1, v1 },
        { lx, ty, u0, v0 }, { rx, by, u1, v1 }, { lx, by, u0, v1 },
    };
    gl.BufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);

    if (g.said_quad < 3) {
        trace("quad in NDC: x %.4f..%.4f  y %.4f..%.4f  (uv %.3f..%.3f,"
              " %.3f..%.3f) from %dx%d, ink %.0fx%.0f, k %.4f,"
              " tex %u, %s",
              lx, rx, by, ty, u0, u1, v0, v1, win_w, win_h,
              a.x1 - a.x0, ink_h, k, (unsigned)a.tex,
              g.debug_flat ? "FLAT" : "textured");
        ++g.said_quad;
    }

    gl.BindTexture(GL_TEXTURE_2D, a.tex);
    gl.Uniform1f(g.u_usetex, g.debug_flat ? 0.0f : 1.0f);
    // The art is white with a black outline, so this multiply tints the fill
    // and leaves the outline black. Alpha is applied in the shader, which is
    // why the colour is NOT premultiplied here.
    gl.Uniform4f(g.u_colour, rgb[0], rgb[1], rgb[2], alpha);
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
}

// The replacement SDL_GL_SwapWindow. Draws first, then hands the frame to the
// implementation we displaced -- after that call the buffer we drew into has
// been presented and reused, so there is no "after" to draw in.
int __cdecl overlay_swap_detour(void* window) {
    overlay_on_swap(window);
    // The debug panel draws after the peer pointers and so sits on top of them,
    // which is the right way round: the panel is for the person at this
    // keyboard and must never be occluded by somebody else's arrow. It is
    // deliberately not gated on g.on -- the peer cursors and the panel are
    // independent switches, and the panel's most useful moment is before there
    // is any session at all.
    ui_on_swap(window);
    return g.swap_prev ? g.swap_prev(window) : 0;
}

} // namespace

// ---------------------------------------------------------------------------

void overlay_set_base(uintptr_t base) {
    g.base = base;

    void** slot = (void**)(base + kRva_SdlSwapSlot);
    void*  prev = nullptr;
    if (!mem_read(slot, &prev, sizeof(prev)) || !prev) {
        log_line("OVERLAY", "!! the SDL swap slot at rva %08X does not hold a function"
                            " -- the peer pointer is OFF", kRva_SdlSwapSlot);
        return;
    }

    // .data is already writable, but say so explicitly rather than depending on
    // a section flag staying what it is today.
    DWORD old_prot = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
        log_line("OVERLAY", "!! could not make the SDL swap slot writable -- the peer"
                            " pointer is OFF");
        return;
    }
    g.swap_prev = (fn_sdl_swap)prev;
    g.swap_slot = slot;
    *slot = (void*)&overlay_swap_detour;
    VirtualProtect(slot, sizeof(void*), old_prot, &old_prot);

    trace("swap taken over: slot %08X was %p, now ours", kRva_SdlSwapSlot, prev);
}

void overlay_init() {
    for (auto& p : g.peers) p = Peer{};
    g.built = false;
    g.prog = g.vao = g.vbo = 0;
    g.have_local = false;
    g.said_fail = false;
    g.drawn = 0;

    g.on = tune::kCursors && tune::kCursorGl && g.swap_slot != nullptr;
    if (tune::kCursors && tune::kCursorGl && !g.swap_slot)
        // NOT routed through trace(): this is the feature turning itself off,
        // and it is the one thing here a player would want to be told. The `!!`
        // is what the classifier grades on.
        log_line("OVERLAY", "!! peer pointer unavailable -- the swap was not "
                            "taken over, so you will not see the other player's "
                            "cursor");
    else if (g.on)
        trace("peer pointer on -- %u px arrows drawn in screen space "
              "before the swap", tune::kCursorPx);
}

void overlay_shutdown() {
    // The jump-table slot is deliberately NOT put back here.
    //
    // Install happens once per process (overlay_set_base) and a session can end
    // and begin again inside one; restoring here would make the second session
    // silently pointerless, which is the asymmetry that is easy to write and
    // hard to notice. The detour costs one indirect call when the overlay is
    // off, because overlay_on_swap returns immediately on !g.on.
    //
    // This does mean the slot must outlive the module. It does: the mod is
    // injected and never unloaded, and there is no FreeLibrary path. If one is
    // ever added, restoring the slot is the first thing it has to do.

    // The GL objects are deliberately NOT deleted. Shutdown can run from any
    // thread, and deleting a GL name without the context current is undefined;
    // leaking one program, one VAO and one buffer for the remaining life of the
    // process is the cheaper mistake by a wide margin.
    g.on = false;
    g.built = false;
    for (auto& p : g.peers) p = Peer{};
}

bool overlay_local_pointer(float& nx, float& ny, uint8_t& mode) {
    if (!g.have_local) return false;
    nx = g.nx; ny = g.ny; mode = g.mode;
    return true;
}

void overlay_on_swap(void* window) {
    if (!g.on || !window) return;

    if (!g.said_swap) {
        trace("first swap seen (window %p) -- the hook is live", window);
        g.said_swap = true;
    }

    // GL first, because the drawable size comes from GL and the mouse fraction
    // is computed against it.
    if (!g.resolved) {
        g.resolved = resolve_gl();
        if (!g.resolved) { g.on = false; return; }   // resolve_gl said why
    }

    // --- the drawable, from the VIEWPORT the game itself set ----------------
    //
    // NOT from GetClientRect(WindowFromDC(wglGetCurrentDC())), which was the
    // first attempt and was wrong: it reported 958x1120 against a real
    // framebuffer of 958x539, so the overlay set a viewport twice as tall as
    // the buffer and pushed every arrow off the bottom edge. Whatever window
    // that DC resolves to, it is not the one being presented.
    //
    // The viewport the game has already set IS the drawable, by definition --
    // it is what the frame you are looking at was rendered through. Taking it
    // means the overlay never calls glViewport at all, which removes both the
    // wrong-size bug and one piece of state to restore.
    GLint vp[4] = {};
    gl.GetIntegerv(GL_VIEWPORT, vp);

    // ...AND ITS SIZE IS THE CONTENT RECTANGLE, BUT ITS POSITION IS NOT.
    //
    // Resize the window away from the game's aspect and it LETTERBOXES: it
    // renders a fixed-aspect image into a centred sub-rectangle and leaves
    // black bars outside. That content rectangle -- not the OS window -- is
    // the region two peers actually have in common, so it is what the pointer
    // fraction must be measured against at BOTH ends.
    //
    // THE VIEWPORT IS NOT THE SOURCE OF THIS RECTANGLE ANY MORE. It was, and it
    // was wrong in a way that hid itself perfectly in testing.
    //
    // It already lied about its ORIGIN: measured on a 958x1120 window the game
    // reports `0,0 958x539` -- the right size, but an origin of (0,0), which in
    // GL's bottom-left convention is the bottom of the window rather than the
    // centre. That much was known, and the fix was to keep the size and centre
    // it.
    //
    // What was NOT known is that the SIZE is not reliable either. Which pass is
    // bound at swap time is not ours to decide: when it is the game's
    // fixed-aspect offscreen pass the size is the content rectangle, and when it
    // is the full-window composite the size is the whole drawable. In the second
    // case `w,h` became `pw,ph`, `cx,cy` became 0,0, and the letterbox
    // correction vanished without a word.
    //
    // THAT IS INVISIBLE WHILE BOTH PEERS RUN THE SAME WINDOW SIZE, because both
    // then measure the pointer against the same wrong rectangle and the error
    // cancels exactly. It appears only when the two aspects differ, as a pointer
    // that gains speed on the short axis and strays into the black bars -- and
    // the bars are the proof, because the viewport is what clip space maps onto,
    // so a pointer drawn in a bar means the viewport WAS the whole window.
    //
    // Derived arithmetic instead: the largest rectangle of the game's fixed
    // aspect, centred in the drawable. Identical on both peers, independent of
    // GL state, and the same answer every frame.
    int pw = 0, ph = 0;
    if (!drawable_size(window, pw, ph)) { pw = vp[2]; ph = vp[3]; }

    int w = pw, h = ph;
    if (pw > 0 && ph > 0) {
        // Wider than the content aspect -> bars left and right; taller -> bars
        // top and bottom. The comparison is in integers to avoid a rounding
        // difference deciding which branch two peers take.
        if ((int64_t)pw * tune::kContentAspectH > (int64_t)ph * tune::kContentAspectW) {
            h = ph;
            w = (int)(((int64_t)ph * tune::kContentAspectW) / tune::kContentAspectH);
        } else {
            w = pw;
            h = (int)(((int64_t)pw * tune::kContentAspectH) / tune::kContentAspectW);
        }
    }
    if (w <= 0 || w > pw) w = pw;
    if (h <= 0 || h > ph) h = ph;
    const int cx = (pw - w) / 2;          // content origin from the LEFT
    const int cy = (ph - h) / 2;          // content origin from the TOP

    // CROSS-CHECKED, NOT TRUSTED. Said once, and only when the game's own
    // viewport size disagrees with the derived rectangle by more than rounding.
    // If tune::kContentAspect* is wrong for a future build this is the line that
    // says so, instead of it surfacing as a pointer that drifts for one player
    // and not the other.
    if (!g.said_aspect && vp[2] > 0 && vp[3] > 0) {
        const int dw = vp[2] > w ? vp[2] - w : w - vp[2];
        const int dh = vp[3] > h ? vp[3] - h : h - vp[3];
        if (dw > 2 || dh > 2) {
            g.said_aspect = true;
            log_line("OVERLAY", "!! content rectangle derived as %dx%d at %d,%d "
                                "(%d:%d in a %dx%d drawable) but the game's own "
                                "viewport is %dx%d -- if the peer pointer drifts "
                                "between differently-shaped windows, this aspect "
                                "is the thing to re-derive",
                     w, h, cx, cy, tune::kContentAspectW, tune::kContentAspectH,
                     pw, ph, vp[2], vp[3]);
        }
    }

    // And because the origin we derived is not the one the game left bound, the
    // overlay has to set its own viewport after all -- drawing through the
    // game's would put our arrows in the bottom-left corner with the content in
    // the middle. GL measures from the bottom, and a centred rectangle has the
    // same gap above and below, so the bottom offset is also `cy`.
    const GLint restore_vp[4] = { vp[0], vp[1], vp[2], vp[3] };
    const bool  own_vp = (vp[0] != cx || vp[1] != cy || vp[2] != w || vp[3] != h);
    if (own_vp) gl.Viewport(cx, cy, w, h);

    // The mouse crosses three spaces to become a fraction of that rectangle:
    //
    //   mouse      LOGICAL window units, origin top-left of the WINDOW
    //   drawable   FRAMEBUFFER pixels -- differs from logical by OS scaling
    //   content    FRAMEBUFFER pixels, inset by the letterbox bars
    //
    // NOTE there is no max-coordinate safety net here any more. It was a
    // stopgap from when nothing knew the window size, and once SDL did it
    // became actively harmful: the largest mouse x seen in a 1280-wide window
    // survived the resize to a 958-wide one, so every x was scaled by 958/1240
    // and every pointer sat a fixed distance too far left. A stale measurement
    // is worse than no measurement.
    int lw = 0, lh = 0;
    mouse_space(window, pw, ph, lw, lh);
    g.mouse_w = lw; g.mouse_h = lh;
    if (lw <= 0) lw = pw;
    if (lh <= 0) lh = ph;

    // --- where our own mouse is, as a fraction of the content rectangle -----
    double mx = 0, my = 0;
    if (w > 0 && h > 0 && read_mouse(mx, my)) {
        const double px = mx * (double)pw / (double)lw;   // logical -> pixels
        const double py = my * (double)ph / (double)lh;
        const float nx = (float)((px - (double)cx) / (double)w);
        const float ny = (float)((py - (double)cy) / (double)h);
        // Outside [0,1] is ORDINARY here, not suspicious: the fraction is taken
        // against the content rectangle, so anywhere in a letterbox bar is
        // legitimately negative or greater than one. With 290-tall bars on a
        // 539-tall content rect the bottom of the window is 1.54, and the old
        // +-0.5 guard rejected it -- freezing the peer's pointer at wherever it
        // last was instead of pinning it to the edge of the content.
        //
        // The guard is only here to reject nonsense (the mouse cache before the
        // first frame, or a wild value from an alt-tab), so it is wide enough
        // now to admit any real point in any plausible window and no wider.
        if (nx > -2.0f && nx < 3.0f && ny > -2.0f && ny < 3.0f) {
            g.nx = nx < 0 ? 0 : (nx > 1 ? 1 : nx);
            g.ny = ny < 0 ? 0 : (ny > 1 ? 1 : ny);
            g.have_local = true;

            if (tune::kCursorTrace && g.traced_local < 60 &&
                (g.last_traced_y < -1 || fabsf(g.ny - g.last_traced_y) > 0.05f)) {
                trace("TRACE local mouse %.1f,%.1f logical %dx%d"
                      " -> px %.1f,%.1f in drawable %dx%d;"
                      " content %d,%d %dx%d (game said vp %d,%d %dx%d)"
                      " -> %.3f,%.3f",
                      mx, my, g.mouse_w, g.mouse_h, px, py, pw, ph,
                      cx, cy, w, h, vp[0], vp[1], vp[2], vp[3], g.nx, g.ny);
                g.last_traced_y = g.ny;
                ++g.traced_local;
            }
        }
    }

    // ...and which cursor we are showing, so the peer can draw the same one.
    char state[32];
    if (read_cursor_state(state)) {
        const uint8_t m = mode_for_state(state);
        // Once per distinct state: the interesting thing is the SET of states a
        // session actually reaches, and there is no way to know in advance
        // which of the sixteen those are. Saying it on every change instead
        // said the same two states thousands of times.
        if (m < 32 && !(g.seen_states & (1u << m))) {
            g.seen_states |= (1u << m);
            trace("local cursor '%s' -> %s (first time this session)",
                  state, kArt[m].state);
        }
        g.mode = m;
    }

    // Everything below scales by the drawable, so an unreadable size is a
    // reason to draw nothing rather than to divide by zero.
    if (w <= 0 || h <= 0) {
        if (!g.said_fail) {
            log_line("OVERLAY", "!! the GL viewport is %dx%d -- the peer pointer"
                                " cannot be placed", w, h);
            g.said_fail = true;
        }
        return;
    }

    const bool test = tune::kCursorGlTest;
    if (!test && !net_active()) return;

    // Is there anybody to draw? Checked before any GL work so a solo session
    // never builds a program it will not use.
    //
    // net_cursor_gl_test forces the draw with no peer at all, which is the one
    // experiment that separates "our GL does not work" from "no peer position
    // arrived". Those two look exactly the same on screen and have nothing in
    // common as bugs.
    // Frame delta for the smoother, measured across swaps.
    double dt = 0.0;
    if (!g.qpc_freq) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); g.qpc_freq = f.QuadPart;
    }
    {
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        if (g.qpc_last && g.qpc_freq)
            dt = (double)(t.QuadPart - g.qpc_last) / (double)g.qpc_freq;
        g.qpc_last = t.QuadPart;
    }
    const float k = smoothing_alpha(dt);

    const uint8_t self = net_self();
    const uint64_t now = GetTickCount64();
    bool any = false;
    for (uint8_t i = 0; i < kMaxPeers && !any; ++i)
        any = (i != self) && g.peers[i].have && (now - g.peers[i].at <= kStaleMs);
    if (!any && !test) {
        if (!g.said_quiet) {
            trace("no peer position has arrived yet -- nothing to draw."
                  " Set net_cursor_gl_test = 1 to draw a marker anyway");
            g.said_quiet = true;
        }
        return;
    }

    // A context recreated under us (a resolution change) invalidates every name
    // we hold. glIsProgram is the cheapest way to ask, and it is asked every
    // frame because there is no notification we could subscribe to.
    if (g.built && !gl.IsProgram(g.prog)) g.built = false;
    if (!g.built) {
        g.built = build();
        if (!g.built) {
            if (!g.said_fail) { log_line("OVERLAY", "!! peer pointer disabled after a build failure"); g.said_fail = true; }
            g.on = false;
            return;
        }
    }

    // --- save every piece of state we are about to touch --------------------
    GLint prev_prog = 0, prev_vao = 0, prev_vbo = 0, prev_fbo = 0;
    GLint prev_tex = 0, prev_unit = 0;
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &prev_unit);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    const bool had_blend   = gl.IsEnabled(GL_BLEND) != 0;
    const bool had_depth   = gl.IsEnabled(GL_DEPTH_TEST) != 0;
    const bool had_cull    = gl.IsEnabled(GL_CULL_FACE) != 0;
    const bool had_scissor = gl.IsEnabled(GL_SCISSOR_TEST) != 0;

    // Framebuffer 0 explicitly: at swap time the presented image is there, but
    // the game may have left an offscreen target bound from its last pass, and
    // drawing into that would be invisible for a reason nothing would report.
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_SCISSOR_TEST);
    gl.UseProgram(g.prog);
    gl.BindVertexArray(g.vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, g.vbo);     // draw_cursor rewrites the quad
    gl.ActiveTexture(GL_TEXTURE0);
    gl.Uniform1i(g.u_tex, 0);

    // Sizing is per-cursor now (each state's ink box differs), so it happens
    // inside draw_cursor rather than once here.

    if (test) {
        // Dead centre, fully opaque, in a colour no peer uses. If this is not
        // on screen then no peer arrow ever will be, and the fault is in the
        // GL path rather than in anything to do with the network.
        // TWO markers, because there are two independent things to prove and
        // they fail identically on screen.
        //
        // The BOX is raw NDC and untextured -- uOrigin 0, uScale 1 -- so it
        // needs no window size, no mouse, no texture and no correct geometry.
        // A missing box means the fault is GL state (program, blend, target)
        // and has nothing to do with cursors.
        //
        // The cyan ARROW beside it is the real path. Box but no arrow isolates
        // the texture or the placement; both missing means neither mattered.
        const float ndc_box[6][4] = {
            { -0.15f, -0.15f, 0.0f, 0.0f }, { 0.15f, -0.15f, 1.0f, 0.0f },
            {  0.15f,  0.15f, 1.0f, 1.0f }, { -0.15f, -0.15f, 0.0f, 0.0f },
            {  0.15f,  0.15f, 1.0f, 1.0f }, { -0.15f,  0.15f, 0.0f, 1.0f },
        };
        gl.BufferSubData(GL_ARRAY_BUFFER, 0, sizeof(ndc_box), ndc_box);
        gl.Uniform1f(g.u_usetex, 0.0f);
        gl.Uniform4f(g.u_colour, 1.0f, 0.0f, 1.0f, 0.6f);
        gl.DrawArrays(GL_TRIANGLES, 0, 6);

        // The same arrow quad twice: once flat, once textured. Flat-but-not-
        // textured says the geometry is right and the sampler is not, which is
        // a completely different fix from a quad in the wrong place.
        g.debug_flat = true;
        const float green[3] = { 0.2f, 1.0f, 0.2f };
        draw_cursor(0, 0.42f, 0.5f, green, 1.0f, w, h);
        g.debug_flat = false;

        const float cyan[3] = { 0.0f, 1.0f, 1.0f };
        draw_cursor(0, 0.58f, 0.5f, cyan, 1.0f, w, h);
    }

    for (uint8_t i = 0; i < kMaxPeers; ++i) {
        if (i == self) continue;
        Peer& p = g.peers[i];
        if (!p.have || now - p.at > kStaleMs) continue;

        // Chase the reported position. The first sample SNAPS: easing in from
        // (0,0) would send the arrow sailing across the screen the moment a
        // peer appears, and a cursor that arrives by flying in from the corner
        // looks like a bug even though it is only the smoother doing its job.
        if (!p.placed) { p.cx = p.nx; p.cy = p.ny; p.placed = true; }
        p.cx += (p.nx - p.cx) * k;
        p.cy += (p.ny - p.cy) * k;

        draw_cursor(p.mode, p.cx, p.cy, kPeerRGB[i % kMaxPeers],
                    (float)alpha_for(p.owns_turn != 0), w, h);
        ++g.drawn;
    }

    if (!g.said_draw) {
        trace("drew into the game's viewport %d,%d %dx%d, fbo was %d,"
              " local mouse %.0f,%.0f -> %.3f,%.3f%s",
              vp[0], vp[1], w, h, (int)prev_fbo, mx, my, g.nx, g.ny,
              test ? "  (TEST MARKER at screen centre)" : "");
        g.said_draw = true;
    }

    // --- and put it all back ------------------------------------------------
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    gl.ActiveTexture((GLenum)prev_unit);
    gl.BindVertexArray((GLuint)prev_vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_vbo);
    gl.UseProgram((GLuint)prev_prog);
    gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    if (own_vp)
        gl.Viewport(restore_vp[0], restore_vp[1], restore_vp[2], restore_vp[3]);
    if (!had_blend)  gl.Disable(GL_BLEND);
    if (had_depth)   gl.Enable(GL_DEPTH_TEST);
    if (had_cull)    gl.Enable(GL_CULL_FACE);
    if (had_scissor) gl.Enable(GL_SCISSOR_TEST);

    // Swallow anything we provoked, so a mistake of ours is never reported as
    // the game's on its next glGetError -- but say what it was the first time,
    // because a silently-dropped GL error is exactly how a draw goes missing.
    const GLenum err = gl.GetError();
    if (err && !g.said_fail) {
        log_line("OVERLAY", "!! GL error 0x%04X after the overlay draw", (unsigned)err);
        g.said_fail = true;
    }
}

void overlay_on_message(uint8_t from, const CursorMsg& c) {
    if (from >= kMaxPeers) return;
    Peer& p = g.peers[from];
    p.nx = c.nx;
    p.ny = c.ny;
    p.mode = c.mode;
    p.owns_turn = c.owns_turn;
    p.at = GetTickCount64();
    if (tune::kCursorTrace && g.traced_peer < 60) {
        trace("TRACE peer %u sent %.3f,%.3f", (unsigned)from, p.nx, p.ny);
        ++g.traced_peer;
    }
    if (!p.have && !g.said_msg) {
        trace("peer %u pointer at %.3f,%.3f -- positions are arriving",
              (unsigned)from, p.nx, p.ny);
        g.said_msg = true;
    }
    p.have = true;
}

} // namespace mgmp
