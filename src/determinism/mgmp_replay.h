// mgmp_replay.h -- phase 2 run B: inject recorded decisions at Brain::GetChoice.
//
// The point of run B is to remove the human from the loop. Runs A, C and D were
// all played by hand, so any difference between them mixes "the game is
// nondeterministic" with "the hands were not identical". Replay pins the input
// and moves exactly one variable at a time.
//
// ---------------------------------------------------------------------------
// What gets injected, and what deliberately does not
//
// Only the HUMAN's decisions. Every AI brain is left alone to re-derive its own
// choice from game state, and that is the actual experiment: if a PatternBrain
// re-runs the same battle and picks something different, we have found real
// nondeterminism. Injecting over the AI too would hide precisely the bug we are
// looking for.
//
// `EvAction::brain_cls` is what makes that separation possible -- it records the
// class of the actor's Brain (Character+0x68) at the moment the action was
// applied. An action is replayable when that class is one of `replay_brains`
// from the ini (default: any class whose name contains "PlayerBrain").
//
// Types 6 and 7 are never injected either. Neither ever originates in a brain:
// type 6 is a reaction broadcast that NextTurn fires directly, bypassing the
// queue entirely, and type 7 invokes a std::function queued by a passive. Both
// are produced locally on every peer. The FIFO holds types 2 and 3 only -- the
// same restriction the wire protocol has.
//
// ---------------------------------------------------------------------------
// Identity: by slot, not by pointer
//
// A recorded action names its ability by (slot_kind, slot_index) on its actor,
// plus the ability's authored GON name. Pointers do not reproduce across runs
// (0 of 21 matched between runs C and D), slots do -- see mgmp_ability.h for
// why. The replayer resolves the slot against the *live* actor and then checks
// the GON name of what came back. A disagreement means the two identities point
// at different abilities, which is a divergence in its own right and is
// reported rather than papered over.
//
// ---------------------------------------------------------------------------
// The release protocol, and why it is idempotent
//
// Injection does NOT pop the queue. Brain::GetChoice is a poll -- it is called
// every frame while the game waits, and Brain::UpdateDecision only calls it when
// its cache at Brain+0x220 is empty. A pop-on-read design would therefore lose
// an action any time the cache was invalidated, and could queue one twice.
//
// So instead:
//
//   GetChoice   fills in the head action and latches `outstanding`. While
//               outstanding, further polls return type=1 (no decision) so the
//               same choice cannot be queued twice -- which is exactly how a
//               human behaves: one decision at a time.
//   ApplyAction the head is popped only when TurnControl::ApplyTurnAction is
//               seen applying an action that MATCHES it. That is the proof the
//               injected decision actually landed.
//
// The match check is the run's real result. An applied action that does not
// match the head means the replay diverged, and it says so at the exact action
// where it happened -- which is the whole point of the exercise.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

struct TurnAction;

// Loads the EV_ACTION stream out of a .mgr capture. Returns false (and logs)
// if the file is missing, malformed, or was recorded before v5 -- a pre-v5
// capture has no slot identity and therefore nothing that can be replayed.
bool replay_init(const wchar_t* path, const char* brain_filter);

void replay_shutdown();
bool replay_active();

// Called from the Brain::GetChoice hook, with the brain and the hidden return
// buffer. Returns true if it wrote a decision into `out` -- the hook should
// then return `out` and not consult the original's result.
//
// Returns false when there is nothing to inject (queue drained, head belongs to
// an AI brain, actor unresolvable), in which case the game's own choice stands.
bool replay_fill_choice(void* brain, void* out);

// True while an injected decision has been handed out but not yet applied. The
// GetChoice hook forces type=1 in that window so the decision cannot be queued
// twice.
bool replay_outstanding();

// Called from the ApplyTurnAction hook. Pops the head when `applied` matches it
// and clears the latch; logs a divergence when it does not. `actor` is the
// character the action was applied to, used to re-derive the applied ability's
// slot for comparison.
void replay_on_applied(const void* applied, const void* actor);

// injected / matched / diverged / remaining, for the shutdown banner.
void replay_stats(uint32_t* injected, uint32_t* matched,
                  uint32_t* diverged, uint32_t* remaining);

} // namespace mgmp
