#pragma once
// mgmp_savefile -- the host's save file is pushed to the client, and the client
// never sees the save-selection screen.
//
// WHY THIS EXISTS AT ALL. Two peers must start a run from byte-identical state,
// and CLAUDE.md's own first-milestone advice was to arrange that by hand: "copy
// the host's save file, do not match checkpoints", because two players at the
// same tutorial checkpoint on separate save slots do NOT share state -- battle
// setup draws ~44 times into tls+0x178 from the cat generation cluster, and a
// separately-created save starts from a different seed, so the first RollChance
// diverges. This module is that manual copy, done by the mod.
//
// It is also the cheapest possible answer to a question RUNSTATE will have to
// answer properly. A save is one self-contained sqlite3 file (~45 KB; the
// shipped ones open with the literal "SQLite format 3\0"), so shipping it whole
// needs none of the ~1390-class serialization the battle layer deliberately
// avoids. It does NOT replace RUNSTATE: this transfers a checkpoint on disk,
// not the host's live in-memory run. See the caveat on savefile_pump.
//
// THE MECHANISM, one screen earlier than the map layer's:
//
//     ContinueSlot(SaveSelection* this, int slot, bool play_sound) @ 0x1401BCE90
//
// Every route from the save-selection screen into a run passes through it (two
// callers, both Button callbacks; its only data xref is .pdata, so it is not
// virtual). That is the same shape as MapScreen::EnterNode and
// TurnControl::ApplyTurnAction, and it does the same two jobs here:
//
//   - on the HOST it names the file. The slot indexes a std::vector<std::string>
//     at SaveSelection+0x38, proven by what the transition lambda does with it:
//         mov rdi,[rcx+8] / movsxd rbx,[rcx+10h] / shl rbx,5 / add rbx,[rdi+38h]
//     then CreateMewDirector(scene, parent, &name) -> MewDirector::init(name).
//     Publishing at the click is what makes the transfer exact: the bytes we
//     send are the bytes the host is itself about to load.
//
//   - on the CLIENT it is both the thing to suppress and the thing to call.
//     A local click is swallowed (the host owns the run), and the auto-select
//     goes through the MinHook trampoline, which bypasses our own detour -- the
//     same trick mgmp_follow uses for EnterNode, and the reason there is no
//     "am I injecting" flag anywhere in here.
//
// THE CLIENT'S OWN SAVES ARE NEVER TOUCHED, and getting there took one more
// hook. The obvious implementation writes the host's bytes over whichever
// steamcampaignNN.sav the host was playing, because that is the only name the
// client's save-selection screen can ask for -- the slot names are a fixed
// vector, and MewDirector::init opens saves/<name> and nothing else. Putting
// the file somewhere else instead is not an option on its own: a file the game
// never opens is not a save.
//
// But the NAME is not fixed, only the vector is. MewDirector::init takes its
// filename by value, so substituting a std::string of our own at the moment of
// the load points it at any file we like. The host's run therefore lands in
// mgmp_coop.sav -- a name the game otherwise never uses -- and the player's
// three real saves are left alone.
//
// It also makes the thing testable on one box, which it otherwise is not. Two
// local instances share one save directory (the game resolves it through
// SDL_GetPrefPath -> SHGetFolderPathW(CSIDL_APPDATA), which no environment
// variable can redirect), so an overwrite-in-place design has the client
// fighting the host for a single file. With the redirect they use two: the host
// plays its own save, the client plays mgmp_coop.sav, and the transfer is a
// real transfer between two real files.

#include <cstdint>

namespace mgmp {

struct SaveFileMsg;

void savefile_init();
void savefile_shutdown();

// Called every frame once the session is Ready. Drives the host's publish,
// which is deferred rather than immediate because the click can happen long
// before a peer connects -- or, on a brand-new game, before the file the click
// names even exists on disk.
void savefile_pump();

// Resolve sub_1403B9CE0 ("save the adventure"), prologue-checked, so the host
// can make the .sav on disk equal its LIVE run before publishing. Called from
// hooks_install alongside catsync_set_base. Failure is non-fatal: it degrades
// the publish to the last checkpoint and says so.
void savefile_set_base(uintptr_t base);

// Send the already-published save to one peer that joined afterwards. No-op if
// the host has not published yet, or on a client.
void savefile_catchup(uint8_t peer);

// From h_SaveSlotClick. Returns false if the caller must NOT run the original,
// which is how a client's local pick is swallowed.
bool savefile_on_slot_click(void* save_selection, int slot);

// From h_SaveSelUpdate, at the tail. Returns the slot the caller should pass to
// the original ContinueSlot, or -1. Returning the index rather than calling it
// keeps the trampoline pointer in mgmp_hooks.cpp, where every other original
// lives.
int  savefile_autoselect(void* save_selection);

// Takes a copy of m.data; the caller keeps ownership of the frame.
void savefile_on_message(const SaveFileMsg& m);

// From h_MewDirectorInit. Returns a std::string to load INSTEAD of the one the
// game chose, or nullptr to leave the call alone. Non-null exactly once per
// injected slot click, so a load the client starts for any other reason is
// never redirected.
const void* savefile_redirect_load();

// Is an adventure loaded on THIS peer? Read off the MewDirector's run cat-id
// list ({cap,count,data} at +1464/+1468/+1472), the same fields catsync uses,
// with the same implausibility bound -- so a drifted offset answers "no" rather
// than answering confidently.
//
// A NON-NULL MewDirector IS NOT A LOADED ADVENTURE: it is a singleton that
// exists from startup, so `director != nullptr` is true on the save-selection
// screen. Assuming otherwise once overwrote a player's save file. Exported for
// mgmp_leave, which wants an answer that does not depend on the scene walk.
bool savefile_adventure_is_loaded();

// From h_ButtonUpdate, at the tail: PRESS PLAY FOR A CLIENT THAT IS SITTING ON
// THE MAIN MENU with the host's save already in hand.
//
// The gap this closes was reported from the wild and is visible in the
// screenshot that came with it: the client connects, sees the host's cursor,
// and stays on Play / Settings / Meow / Quit forever. savefile_autoselect can
// only run from SaveSelection::update, so until a human presses Play there is
// no screen for it to act on -- and nothing in the log said so, because from
// the module's point of view nothing had gone wrong yet.
//
// It hangs off the BUTTON hook because that detour already fires for every
// button in the game with a guaranteed-live Button*, so no new hook, no
// scene-lifetime reasoning and no cached pointer are needed. MainMenu itself
// could not have provided one: its vftable slots 6..15 are all the ICF-folded
// empty virtual, so it has no update to hook.
//
// Cost outside the one moment it matters is a bool load. The name at Button+504
// is only read once a save is actually pending.
void savefile_on_button_update(void* button);

} // namespace mgmp
