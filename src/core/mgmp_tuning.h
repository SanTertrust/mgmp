// mgmp_tuning.h -- the settings that are NOT in mgmp.json.
//
// Every constant here used to be a config key. They were removed from the file
// because a knob nobody turns is not a feature: it costs a parser branch, a
// line of documentation, a field in Config, and -- worst of the four -- it
// makes the config file long enough that the keys that DO matter stop being
// visible in it.
//
// The test for staying in mgmp.json was "has this ever actually been changed",
// and the evidence accepted was a flag in tools/net_test.ps1: role, address,
// port, cat split, the four diagnostic switches that script can flip, the
// recorder, and the panel. Nothing else passed, so nothing else is in the file.
//
// Changing one of these means editing this header and rebuilding, which for a
// mod that is rebuilt every session is the same gesture as editing a config
// file -- and it puts the switch next to the code it governs instead of in a
// file the code has to be trusted to read correctly.
#pragma once

#include <cstdint>

namespace mgmp {
namespace tune {

// --- the phase-1 text trace -------------------------------------------------

// Open a console window alongside the log file. OFF: everything it ever showed
// is now readable somewhere better -- the stamped file log keeps the whole run,
// the ImGui panel shows it live and filtered, and the failures that happen
// before the logger exists go to mgmp_boot.log, which the console never saw
// either. All it added was a second window to alt-tab past.
constexpr bool     kConsole      = false;
// Print raw pointers in trace lines. They differ between processes, so two
// peers' logs only diff cleanly with this off -- which is why it existed. In
// practice the diffing is done on the tagged lines, not the whole log.
constexpr bool     kPointers     = true;
// Bytes of TurnAction to hexdump when kTaRaw asks for one.
constexpr uint32_t kTaDump       = 64;
constexpr bool     kTaRaw        = false;
// Log every Brain::GetChoice poll, not just the ones that decided something.
// 1695 of 1711 calls in one battle return "nothing decided".
constexpr bool     kChoiceAll    = false;
// Frames between FRAME lines; 0 silences them. Effectively always 0 already:
// the frame hook is forced on by net_role and by the panel, and both silenced
// the logging when they did it, because they wanted the socket pumped and not
// a line every 60 frames.
constexpr uint32_t kFrameLogEvery = 0;

// --- phase 2: the recorder --------------------------------------------------

// Record draws from every RNG stream, not just the simulation one. On is the
// wrong setting and always was: the non-global draws turned out to be the
// interesting ones (they are how TLS+0x198 was found at all), and the volume
// that justified filtering never materialised.
constexpr bool     kRngGlobalOnly = false;
// Put EV_FRAME records in the stream. Only run D needed them -- a starved run
// has fewer frames by construction, so frames are excluded from the diff.
constexpr bool     kRecordFrames  = false;

// Which brains a decision may be injected into, and -- the load-bearing use --
// which cats count as HUMAN when the control split is derived. Substring match
// against the live RTTI class name.
//
// This is the one culled setting with a real consequence: it decides which cats
// the two peers divide between them. It is a constant because there has never
// been a second answer. If a summonable cat ever turns up with a PlayerBrain,
// this is where the exception goes -- see the summon note in CLAUDE.md.
constexpr const char* kReplayBrains = "PlayerBrain";

// --- the two hooks that CHANGE the game rather than watch it ----------------
//
// Both still announce themselves loudly in the startup banner while on, which
// is the property that mattered -- not that they were reachable from a file.

// No-op MewDirector::ApplySaveScumPenalty, so that reloading the same save to
// repeat a battle does not mutate cat state as a function of the reload count.
// This is a capture-methodology switch, not a cheat: with it live, run N and
// run N+1 start from different states by construction.
// ON: a multiplayer test session relaunches the game constantly for reasons
// that have nothing to do with the run -- a rebuilt DLL, a peer that dropped,
// a deliberate reconnect -- and Steven counts every one of them. The penalty
// was measuring our tooling, not the player.
constexpr bool     kHookSaveScum  = true;
// Convert TimeDelayStatusApplication's wall-clock countdown to a turn count.
// Never yet run against real content -- the one reachable user is AZ_LoseHead.
constexpr bool     kHookTimeDelay = false;
// Turn boundaries such a status waits before firing. 1 = the next boundary.
constexpr uint32_t kTimeDelayTurns = 1;

// --- the meta layer's sync modules ------------------------------------------
//
// Each of these was a switch whose documented purpose was "turn it off to tell
// a broken push apart from a broken assumption". That is a real debugging move
// and it is still available -- by editing the line, which is a rebuild rather
// than a relaunch. What it is NOT any more is something a stale config file in
// one peer's directory can silently disagree with the other peer about, and
// every one of them is a setting where the two peers disagreeing is worse than
// either value.

constexpr bool     kCatSync   = true;   // push the host's cats per map node
constexpr bool     kInvSync   = true;   // push the run inventory per map node
constexpr bool     kChoice    = true;   // replicate event / level-up decisions
constexpr bool     kRunHist   = true;   // push *(MewDirector+1424), the used-event list
constexpr bool     kNodeHash  = true;   // the meta layer's per-node hash

// Whether a NODEHASH mismatch is fatal. Off, unlike the battle layer's halt:
// a battle desync makes every later turn fiction, a meta divergence is usually
// one number and a run that still plays.
constexpr bool     kNodeHashHalt = false;

// --- the battle layer's detection -------------------------------------------

// Include character HP/shield/tile in the per-turn hash. Off leaves the RNG
// stream and the queue. The hash gates itself on evidence anyway.
constexpr bool     kStateHash  = true;
// One line per cat whose state changed, at every turn boundary. This is the
// only thing that answers "when did the field last move, and by how much",
// which is the question a two-HP difference actually turns on.
constexpr bool     kStateTrace = true;

// --- peer cursors -----------------------------------------------------------
//
// Presentation. Nothing here is hashed, sent at a command boundary, or able to
// reach a decision, so these are the safest constants in the file.

constexpr bool     kCursors        = true;  // the board reticle
constexpr bool     kCursorGl       = true;  // the screen-space pointer
// One magenta arrow at screen centre with no peer and no session, to tell "our
// GL is broken" apart from "no peer position arrived". Both look like no arrow.
constexpr bool     kCursorGlTest   = false;
// Bounded trace of the pointer fraction at both ends, re-armed on every resize.
constexpr bool     kCursorTrace    = false;
constexpr uint32_t kCursorAlpha    = 100;   // the peer whose cat is deciding
constexpr uint32_t kCursorAlphaDim = 30;    // everybody else
// Ink height in pixels at kCursorRefH, scaled by the content rectangle so a
// bigger window draws a bigger cursor the way the game's own does.
constexpr uint32_t kCursorPx       = 34;
constexpr uint32_t kCursorRefH     = 720;

// DRAW THE PEER'S POINTER AS THE CURSOR THEIR GAME IS SHOWING, rather than
// always as the plain arrow. OFF while the reported drift is being isolated.
//
// The report (2026-08-28) was that the peer pointer is exact until the player
// starts AIMING, at which point it moves. `mode` is the only thing in
// draw_cursor that changes at that moment, so it is the only candidate, and the
// shipped art says why it could move anything at all:
//
//   state       ink box              ink h    vs default
//   default     ( 29, 2)-(108,108)   106      --
//   move        ( 29, 2)-(107,120)   118      x1.113
//   spell       ( 29, 2)-(111,123)   121      x1.142
//   attack      ( 29, 2)-(106,128)   126      x1.189
//
// Every aiming state shares default's ink ORIGIN and is 11-19% TALLER, because
// the badge hangs below the arrow. The sizing divided by that height, so the
// glyph changed size the moment the state changed -- measured from the shipped
// PNGs, not inferred.
//
// That defect is fixed independently (kCursorInkRefH below), so turning this
// back on no longer changes the geometry. It is off because the report has not
// been re-measured since, and one word restores it.
constexpr bool     kPeerCursorArt  = false;

// Sizing reference: `default`'s ink height, measured above. EVERY state is now
// scaled by this rather than by its own ink box, so which cursor a peer is
// showing can never change how big their pointer is or where it sits. The badge
// on an aiming cursor simply hangs below the arrow, which is what it does in
// the game.
constexpr float    kCursorInkRefH  = 106.0f;
// THE GAME'S FIXED CONTENT ASPECT, and the letterbox rectangle is derived from
// it rather than from the GL viewport.
//
// The viewport was the original source and it is not trustworthy: whether the
// value observed at swap time belongs to the game's fixed-aspect offscreen pass
// or to its full-window composite depends on which pass happened to be bound
// last. When it is the composite, the "content rectangle" collapses to the whole
// window and the letterbox correction silently disappears -- which is invisible
// while both peers run the SAME window size, because both then measure the
// pointer against the same wrong rectangle and the error cancels exactly. It
// only shows when the two aspects differ, and it shows as the peer pointer
// gaining speed on the short axis and straying into the black bars. Reported
// from the wild 2026-08-28, with "same resolution everything is good" as the
// tell.
//
// 16:9, from TWO independent readings. The shipped `swfs/ui.swf` declares a
// stage of 25600x14400 twips = 1280x720 exactly, which is what the whole UI is
// authored against; and the one recorded runtime measurement agrees -- a
// 958x1120 window rendered content 958x539 (958 * 9 / 16 = 538.9) with
// 290-pixel bars top and bottom ((1120 - 539) / 2 = 290.5).
//
// Derived arithmetic is identical on both peers and does not depend on GL state,
// which is exactly the property the viewport lacked. The binary confirms why it
// lacked it: the game's own cursor pass (sub_140A16B00 @ 0x140A16E05 and
// 0x140A17069) calls SDL_GetWindowSizeInPixels and then
// glViewport(0, 0, whole drawable) before drawing, so on any frame that pass
// runs last the viewport we observe at swap time is the full window.
//
// It is cross-checked rather than trusted: the overlay logs the game's own
// viewport beside the derived rectangle, so a wrong aspect is one line away
// from being visible instead of being a slow drift nobody can name.
constexpr int      kContentAspectW = 16;
constexpr int      kContentAspectH = 9;

// Exponential smoothing time constant. CURSOR is throttled to ~20 messages a
// second, so drawn raw the arrow reads as a peer with an unsteady hand. 60 ms
// is roughly one throttle interval.
constexpr uint32_t kCursorSmoothMs = 60;

// --- the combat menu lock ---------------------------------------------------

// Grey the ability bar out while it belongs to a cat a PEER controls, using the
// game's own disabled button state. Presentation only: it writes one int on a
// UI object after the game has computed it, and the clicks it stops were already
// being discarded at Brain::GetChoice. See mgmp_combatlock.h.
//
// Here rather than in mgmp.json by the same test as everything else in this
// file: there is no experiment that wants it off. The reason it is a constant at
// all is that it costs two hooks, one of them on Button::update -- so turning it
// off has to remove the hooks, not just the effect, which is why the config
// layer reads this rather than the module doing it.
constexpr bool     kCombatLock = true;

// --- the peer's aim preview -------------------------------------------------

// Draw the range / AOE tiles the OTHER player is currently aiming at, using the
// game's own Brain::DrawAbilityAOE. Presentation, and in the strictest sense:
// the call is made with the simulation stream saved and restored around it, so
// it cannot move the sim even if the fence pass missed a site. See mgmp_aim.h.
//
// A constant rather than a config key by the usual test: there is no experiment
// that wants it off. It is read once at init, so turning it off removes the
// publish and the draw, not just the effect.
constexpr bool     kAimPreview = true;

// --- the debug panel --------------------------------------------------------

// Lines the log pane keeps for scrollback. The ring behind it holds 4096.
constexpr uint32_t kUiLogLines = 2000;

} // namespace tune
} // namespace mgmp
