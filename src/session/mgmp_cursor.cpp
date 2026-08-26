// mgmp_cursor.cpp -- see mgmp_cursor.h for why this is immediate-mode and why
// the unit on the wire is a tile.
#include "mgmp_cursor.h"

#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_net.h"
#include "mgmp_overlay.h"
#include "mgmp_proto.h"

#include <windows.h>

#include <cstring>

namespace mgmp {
namespace {

// --- the two game functions we call -----------------------------------------

// Component* -> ImmediateModeGameUI*. Null off the battle screen.
typedef void* (__fastcall* fn_im_game_ui)(void* component);

// The immediate-mode submit. Four register arguments, four on the stack; see
// the annotated entry in kCalls for where each one came from.
//
// `tile` is BY VALUE and eight bytes, so it goes in a stack slot whole -- which
// is why it is a uint64_t here rather than a struct: an aggregate of exactly 8
// bytes is passed in a register-sized slot by the MSVC x64 ABI either way, and
// spelling it as an integer removes any question about whether the compiler
// decided to pass a pointer instead.
typedef void* (__fastcall* fn_tile_piece)(void* ui, void* id, int layer, void* anim,
                                          uint64_t tile, const float* rgba,
                                          int frame, const double* scale);

// BOTH pointer arguments are dereferenced with MOVAPS, which faults on anything
// that is not 16-byte aligned:
//
//   0x14034007F  mov    rcx, [rsp+arg_28]      ; the rgba float[4]
//   0x140340087  movaps xmm0, xmmword ptr [rcx]
//   0x14034005E  mov    rcx, [rsp+arg_38]      ; the scale double[3]
//   0x140340066  movaps xmm0, xmmword ptr [rcx]
//
// A bare `float[4]` gets 4-byte alignment and a `double[3]` 8-byte, so passing
// plain locals is a coin flip decided by whatever else is in the stack frame.
// It came up heads for a while: the overlay shipped with unaligned locals and
// drew fine, then adding a second call site reshuffled the frame and both peers
// took an access violation at 0x140340066 on the first battle frame.
//
// The game's own callers get this for free -- theirs are Vec3D/colour members
// of aligned objects -- so there is nothing in the call sites to warn you.
struct alignas(16) PieceColour { float  v[4]; };
struct alignas(16) PieceScale  { double v[3]; };

// --- a std::string the game can consume -------------------------------------
//
// Both string arguments are DESTROYED by the callee (_Tidy_deallocate before it
// returns), which frees only when capacity exceeds 15. Every name this module
// passes is therefore 15 characters or fewer, so the destructor does nothing at
// all and no memory crosses between our /MT CRT heap and the game's.
//
// That is a constraint on the names, not a coincidence: an animation whose name
// does not fit would have to be allocated by the GAME's allocator for the
// game's free to be the one that runs on it, which is the two-heap hazard
// mgmp_invsync answers with C_GameStrAlloc / C_GameStrDtor. Dodging it is
// strictly cheaper than handling it, so a name that does not fit is refused and
// logged instead.
struct GameStr {
    char     buf[16];
    uint64_t size;
    uint64_t cap;
};

bool make_str(GameStr& s, const char* text) {
    size_t n = strlen(text);
    if (n > 15) return false;          // would become a heap string; see above
    memset(&s, 0, sizeof(s));
    memcpy(s.buf, text, n);
    s.size = n;
    s.cap  = 15;
    return true;
}

// --- who gets which colour --------------------------------------------------
//
// Fixed per peer id rather than negotiated, because both sides already agree on
// ids (PEERS is host-authored and sorted) and a colour nobody has to be told is
// a colour that cannot be told wrong. Chosen to stay apart on the game's
// palette, which is mostly browns and greys.
const float kPeerRGB[kMaxPeers][3] = {
    { 1.00f, 0.80f, 0.20f },   // 0, the host  -- gold
    { 0.30f, 0.80f, 1.00f },   // 1            -- cyan
    { 0.45f, 1.00f, 0.45f },   // 2            -- green
    { 1.00f, 0.40f, 0.85f },   // 3            -- magenta
};

// Matching Brain::UpdateDecision's own target cursor, so a peer's mark reads as
// the same kind of object the local one is rather than as a foreign overlay.
//
// This is the MOVIECLIP name, and it is NOT the string Brain::UpdateDecision
// passes in rdx. The two arguments read backwards at a glance because the
// game's own names invert the convention you would expect: rdx carries the
// immediate-mode identity, which Tyler spells lowercase ("target", "aoe",
// "path"), and r9 carries the animation, which is the CamelCase asset name
// exported from swfs/ui.swf ("TargetCursor", "AreaIndicator", "PathIndicator").
//
// Getting this backwards is not a silent cosmetic failure. Renderer::init
// fatal-errors on an unknown animation -- "Could Not Find MovieClip named: " --
// by THROWING a std::string, which unwinds to std::terminate and kills the
// process. It surfaces with no mgmp.dll frame on the stack, because the lookup
// happens in the UI consumer pass long after our submit has returned. That cost
// a session; see the crash write-up in CLAUDE.md.
const char* kAnimName = "TargetCursor";   // 12 chars -- still inside the SSO budget
constexpr int kLayer  = 6;
constexpr int kFrame  = -1;

// The pointer itself is NOT drawn here, and that is the whole lesson of this
// file. It lives in mgmp_overlay.cpp, in screen space, drawn with GL.
//
// Three attempts went through this path first -- the game's own MewCursor clip
// submitted onto a tile -- and each failed differently for one underlying
// reason: a board piece is anchored to a square, depth-sorted against the
// scenery, and authored at board scale. A pointer is none of those things. It
// snapped to tiles, hid behind the front rows, and drew at 12x22 pixels against
// a 140-pixel tile; scaling it up only made it a tile-sized smear.
//
// What stays here is the RETICLE, which IS a board fact and belongs on the
// board: it says which SQUARE the peer is over, and it stays correct however
// either player has panned their camera.

// A peer heard from longer ago than this is not drawn. It is generous on
// purpose: the send side is throttled and a peer sitting perfectly still still
// heartbeats, so anything approaching this means the peer is genuinely gone.
constexpr uint64_t kStaleMs = 3000;

// Send on movement, but never faster than this, and at least this often even
// when nothing moved so a peer that joined mid-battle sees a cursor promptly.
constexpr uint64_t kMinSendGapMs   = 50;
constexpr uint64_t kHeartbeatMs    = 500;

struct Peer {
    CursorMsg msg;
    uint64_t  at   = 0;       // GetTickCount64 when it arrived
    bool      have = false;
};

struct State {
    bool on = false;

    uintptr_t base = 0;       // the game module, for the ApplicationBase global

    fn_im_game_ui im_game_ui = nullptr;
    fn_tile_piece tile_piece = nullptr;

    Peer peers[kMaxPeers];

    // Outbound throttle.
    CursorMsg last_sent;
    uint64_t  last_send_at = 0;
    bool      have_sent    = false;

    // Whether we have written ANY alpha onto the game's own pips, so they can
    // be put back exactly once when the overlay stops. Without this a session
    // that ends mid-battle leaves the local player's cursor at whatever we last
    // wrote, with nothing left running to explain it.
    //
    // It tracks "we wrote", not "we dimmed", and the difference is not
    // cosmetic: net_cursor_alpha is a knob, so the value we write on our own
    // turn is only 1.0 at the default setting. Keying the restore on dimming
    // alone would leave a partly faded cursor behind forever on any other one.
    bool touched_pips = false;

    // Said-once diagnostics. This runs every frame of every battle; anything
    // that can be wrong here can be wrong sixty times a second.
    bool said_no_ui       = false;
    bool said_epoch_skew  = false;
    uint32_t drawn_frames = 0;
} g;

const Config& cfg() { return config(); }

double alpha_for(bool owns_turn) {
    uint32_t pct = owns_turn ? tune::kCursorAlpha : tune::kCursorAlphaDim;
    if (pct > 100) pct = 100;
    return (double)pct / 100.0;
}

// --- reading the local hover ------------------------------------------------

// StatusMenu+124 is not 8-byte aligned, so it is read as bytes rather than as a
// qword. The game itself writes it with an unaligned store, which x86 does not
// care about, but a strict-aliasing-friendly read here costs nothing.
bool read_hover_tile(const void* sm, int32_t& x, int32_t& y) {
    uint8_t raw[8];
    if (!mem_read((const uint8_t*)sm + kSM_HoverTile, raw, sizeof(raw))) return false;
    memcpy(&x, raw + 0, 4);
    memcpy(&y, raw + 4, 4);
    return true;
}

// The board size, straight from the object StatusMenu::update bounds-checks
// against before it will show a pip of its own. Returns false when there is no
// grid, which is the normal state outside a battle.
bool read_grid_bounds(const void* sm, uint32_t& w, uint32_t& h) {
    void* grid = nullptr;
    if (!mem_read((const uint8_t*)sm + kSM_Grid, &grid, sizeof(grid)) || !grid) return false;
    if (!mem_read((const uint8_t*)grid + kGrid_Width,  &w, sizeof(w))) return false;
    if (!mem_read((const uint8_t*)grid + kGrid_Height, &h, sizeof(h))) return false;
    // A plausibility gate, not a correctness one: if these are wild the pointer
    // is not a grid and nothing below should run.
    return w > 0 && h > 0 && w <= 4096 && h <= 4096;
}

// --- the game's own two pips ------------------------------------------------

// Writes `a` to Renderer+0x60 on both of StatusMenu's cursor pips.
//
// It does NOT touch their visible byte at +81: StatusMenu::update owns that and
// sets it every frame from whether the mouse is over the board. Fighting it
// would make the local cursor flicker; alpha is a field nothing else writes
// after construction, which is exactly why it is the one to use.
void set_local_pip_alpha(const void* sm, double a) {
    const uintptr_t slots[2] = { kSM_PipGround, kSM_Pip3D };
    for (int i = 0; i < 2; ++i) {
        const uintptr_t off = slots[i];
        void* pip = nullptr;
        if (!mem_read((const uint8_t*)sm + off, &pip, sizeof(pip)) || !pip) continue;
        mem_write((uint8_t*)pip + kRenderer_Alpha, &a, sizeof(a));
    }
}

// --- drawing ----------------------------------------------------------------

// One immediate-mode submission.
//
// `id_prefix` must be at most 14 characters: a peer digit is appended, and the
// result is the piece's IDENTITY. Sharing one id between two peers would make
// them the same piece, so the second submission of a frame would simply move
// the first rather than draw a second cursor.
//
// Both strings are consumed by the callee, which is why they are rebuilt per
// submission rather than cached: handing the same image over twice would be a
// double free the moment one of them is a heap string.
void submit(void* ui, uint8_t peer, const char* id_prefix, const char* anim_name,
            int layer, uint64_t tile, const PieceColour& rgba) {
    char id_text[16] = {};
    size_t n = strlen(id_prefix);
    if (n > 14) return;
    memcpy(id_text, id_prefix, n);
    id_text[n]     = (char)('0' + (peer % 10));
    id_text[n + 1] = 0;

    GameStr id{}, anim{};
    if (!make_str(id, id_text)) return;
    // Only fails on a name longer than the small-string buffer, which is a
    // constant in this file and therefore a build-time mistake, not a runtime
    // condition. The id above was built but never handed over; it is a small
    // string by construction, so there is nothing to release.
    if (!make_str(anim, anim_name)) return;

    const PieceScale scale = { { 1.0, 1.0, 1.0 } };   // native size
    g.tile_piece(ui, &id, layer, &anim, tile, rgba.v, kFrame, scale.v);

}

void draw_peer(void* ui, uint8_t peer, const CursorMsg& c) {
    const float* rgb = kPeerRGB[peer % kMaxPeers];

    // Alpha IS the turn indicator: the peer whose cat is deciding is drawn
    // solid and everybody else is faded, so you can tell whose move it is
    // without reading a name. Element 3 of the colour is the alpha outright --
    // the consumer premultiplies rgb by it and writes it to Renderer+0x60 --
    // so there is no blend state to set and no second draw.
    const float a = (float)alpha_for(c.owns_turn != 0);
    const PieceColour rgba = { { rgb[0], rgb[1], rgb[2], a } };

    // Packed exactly as the game stores an iVec2D: x in the low dword.
    const uint64_t tile = ((uint64_t)(uint32_t)c.y << 32) | (uint32_t)c.x;

    // "MGMPCur0" -- the square the peer is over, and the only thing this file
    // draws. The pointer is mgmp_overlay's.
    submit(ui, peer, "MGMPCur", kAnimName, kLayer, tile, rgba);
}

} // namespace

// ---------------------------------------------------------------------------

void cursor_set_base(uintptr_t base) {
    g.base       = base;
    g.im_game_ui = nullptr;
    g.tile_piece = nullptr;

    struct { int which; void** slot; } want[] = {
        { C_ImGameUI,    (void**)&g.im_game_ui },
        { C_ImTilePiece, (void**)&g.tile_piece },
    };

    for (auto& w : want) {
        const uintptr_t addr = addr_of_call((Call)w.which);
        if (!addr) {
            // Non-fatal by design. A cursor is a nicety; a call to the wrong
            // address would run an unrelated function with our arguments on the
            // game's stack, which is not.
            log_line("CURSOR", "!! %s did not resolve by signature -- "
                               "the peer cursor overlay is OFF", kCalls[w.which].name);
            g.im_game_ui = nullptr;
            g.tile_piece = nullptr;
            return;
        }
        *w.slot = (void*)addr;
    }
}

void cursor_init() {
    for (auto& p : g.peers) p = Peer{};
    g.have_sent    = false;
    g.last_send_at = 0;
    g.touched_pips = false;
    g.said_no_ui   = false;
    g.said_epoch_skew = false;
    g.drawn_frames = 0;

    g.on = tune::kCursors && g.im_game_ui != nullptr && g.tile_piece != nullptr;
    if (!tune::kCursors)
        log_line("CURSOR", "peer cursors disabled (tune::kCursors)");
    else if (!g.on)
        log_line("CURSOR", "peer cursors unavailable -- the draw calls did not resolve");
    else
        log_line("CURSOR", "peer reticles on -- %u%% alpha on the deciding peer,"
                           " %u%% on the others (the pointer itself is the overlay's)",
                 tune::kCursorAlpha, tune::kCursorAlphaDim);
}

void cursor_shutdown() {
    g.on = false;
    for (auto& p : g.peers) p = Peer{};
}

void cursor_on_message(uint8_t from, const CursorMsg& c) {
    if (from >= kMaxPeers) return;
    g.peers[from].msg  = c;
    g.peers[from].at   = GetTickCount64();
    g.peers[from].have = true;
}

void cursor_on_status_menu(void* sm) {
    if (!sm) return;

    // The pips must be put back before anything else bails out, or a session
    // that ends mid-battle leaves the local cursor faded forever.
    if (!g.on || !net_active()) {
        if (g.touched_pips) { set_local_pip_alpha(sm, 1.0); g.touched_pips = false; }
        return;
    }

    uint32_t w = 0, h = 0;
    if (!read_grid_bounds(sm, w, h)) {
        if (g.touched_pips) { set_local_pip_alpha(sm, 1.0); g.touched_pips = false; }
        return;   // not on a battle board; nothing to point at
    }

    const uint64_t battle = lockstep_battle_id();
    const bool     mine   = lockstep_local_actor();
    const uint64_t now    = GetTickCount64();

    // --- our own cursor: fade it when the turn is not ours ------------------
    set_local_pip_alpha(sm, alpha_for(mine));
    g.touched_pips = true;

    // --- publish where we are pointing --------------------------------------
    CursorMsg out;
    out.battle_id = battle;
    out.owns_turn = mine ? 1 : 0;

    // Where our mouse is on our own window. Published by the swap hook, which
    // is the only place that has a window to divide by; absent before the first
    // frame, in which case the peer simply keeps the last position it had.
    overlay_local_pointer(out.nx, out.ny, out.mode);

    int32_t hx = 0, hy = 0;
    if (read_hover_tile(sm, hx, hy) &&
        hx >= 0 && hy >= 0 && (uint32_t)hx < w && (uint32_t)hy < h) {
        out.x = hx; out.y = hy; out.on_board = 1;
    } else {
        out.on_board = 0;
    }

    const bool moved = !g.have_sent ||
                       out.x != g.last_sent.x || out.y != g.last_sent.y ||
                       out.on_board  != g.last_sent.on_board ||
                       out.owns_turn != g.last_sent.owns_turn ||
                       out.nx != g.last_sent.nx || out.ny != g.last_sent.ny ||
                       out.battle_id != g.last_sent.battle_id;
    const uint64_t since = now - g.last_send_at;
    if ((moved && since >= kMinSendGapMs) || since >= kHeartbeatMs) {
        if (net_send_cursor(out)) {
            g.last_sent    = out;
            g.have_sent    = true;
            g.last_send_at = now;
        }
    }

    // --- draw everybody else ------------------------------------------------
    void* ui = g.im_game_ui(sm);
    if (!ui) {
        // Expected on any screen that is not a battle, so it is said once and
        // only matters if it is said while a battle is clearly running.
        if (!g.said_no_ui) {
            log_line("CURSOR", "no ImmediateModeGameUI on this screen -- peer cursors"
                               " will appear once a battle is up");
            g.said_no_ui = true;
        }
        return;
    }

    const uint8_t self = net_self();
    for (uint8_t i = 0; i < kMaxPeers; ++i) {
        if (i == self) continue;                 // ours is the game's own pip
        const Peer& p = g.peers[i];
        if (!p.have || !p.msg.on_board) continue;
        if (now - p.at > kStaleMs) continue;     // gone quiet; stop drawing it

        // A peer legitimately runs a battle ahead or behind -- that is why a
        // battle mismatch is a drop everywhere else in this protocol and never
        // a halt. Here it means their tile refers to a board that is not the
        // one on screen, so drawing it would be confidently wrong. Hide instead.
        if (p.msg.battle_id != battle) {
            if (!g.said_epoch_skew) {
                log_line("CURSOR", "peer %u is in battle %016llx and we are in %016llx"
                                   " -- its cursor is hidden until the two agree",
                         (unsigned)i, (unsigned long long)p.msg.battle_id,
                         (unsigned long long)battle);
                g.said_epoch_skew = true;
            }
            continue;
        }

        // The wire already range-checked this loosely; the live board is the
        // only place the real bounds exist, and the tile reaches an array
        // subscript inside the game.
        if (p.msg.x < 0 || p.msg.y < 0 ||
            (uint32_t)p.msg.x >= w || (uint32_t)p.msg.y >= h) continue;

        draw_peer(ui, i, p.msg);
        ++g.drawn_frames;
    }
}

} // namespace mgmp
