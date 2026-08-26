// mgmp_turnaction.h -- glaiel::TurnAction, recovered from a phase-1 trace.
//
// The binary has no type information for this struct: it is passed by value and
// is only ever poked at by offset. The layout below was read off 24 real
// actions from a tutorial battle, cross-checked against pointers that appear
// independently in the same log lines.
//
//   +0x00 u32        type          see TurnActionType below
//   +0x04 --         PADDING -- see the note below, never read this
//   +0x08 Ability*   the ability being used; matches the `this` that
//                    Ability::trigger is subsequently called on
//   +0x10 iVec2D     target tile (x, y)
//   +0x18 iVec2D     direction (x, y); observed (0,0), (1,0), (0,1), (-1,0)
//   +0x20 Character* the acting character; matches Character::DoAction's `this`
//   +0x28 ...        zero in every sample so far
//   +0x30 u8         read by Ability::trigger and passed to Ability::Prime
//   +0x31 u8         read by Ability::trigger as a gate
//   +0x78 void*      std::function impl pointer (see the warning below)
//
// **sizeof(TurnAction) == 0x88 (136 bytes)**, from TurnControl::QueueDecision:
// it does `operator new(0x88)` per queued action. The 64 bytes the first trace
// dumped were only the front of it.
//
// **TurnAction carries a std::function.** TurnControl::ApplyTurnAction treats
// type 7 by calling through the pointer at +0x78, and Brain::UpdateDecision
// zero-initialises +0x78 and +0x80 alongside the scalar fields. A std::function
// cannot go on the wire. Any type-7 action must therefore be *reproduced* on
// both peers rather than transmitted -- worth knowing before the protocol is
// designed around "just serialize the TurnAction".
//
// The padding at +0x04 is how we know the struct is copied member-wise rather
// than memcpy'd: in Character::DoAction's copy those four bytes read 0x85, and
// in the copy that reaches Ability::trigger they read "pone" -- stale stack from
// some unrelated string. A memcpy would have carried the same value through.
// Anything the wire protocol serializes must therefore skip +0x04, and any
// state hash must skip it too or it will differ between peers for no reason.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

#pragma pack(push, 1)
struct TurnAction {
    uint32_t type;
    uint32_t pad_04;        // uninitialized; do not read, do not hash
    void*    ability;
    int32_t  target_x;
    int32_t  target_y;
    int32_t  dir_x;
    int32_t  dir_y;
    void*    actor;
    uint8_t  tail[0x60];    // 0x28..0x87; includes the std::function at +0x78
};
#pragma pack(pop)

static_assert(offsetof(TurnAction, ability)  == 0x08, "TurnAction layout");
static_assert(offsetof(TurnAction, target_x) == 0x10, "TurnAction layout");
static_assert(offsetof(TurnAction, dir_x)    == 0x18, "TurnAction layout");
static_assert(offsetof(TurnAction, actor)    == 0x20, "TurnAction layout");
static_assert(sizeof(TurnAction)             == 0x88, "TurnAction is 136 bytes");

enum TurnActionType : uint32_t {
    TA_None    = 1,   // brain has not decided yet -- PlayerBrain polls with this
    TA_Ability = 2,   // use `ability` at `target` facing `dir` -> Character::DoAction
    TA_EndTurn = 3,   // confirmed -> Character::EndTurn
    TA_Invoke  = 7,   // calls the std::function at +0x78; not serializable
};

// "type=2(ability) abil=glaiel::MoveAbility target=(5,5) dir=(1,0)
//  actor=glaiel::Character"  -- guarded, never faults on a bad pointer.
void format_turn_action(char* out, size_t out_size, const void* ta, bool raw_bytes);

// Cheap identity for spam suppression: the fields that make an action distinct.
uint64_t turn_action_digest(const void* ta);

} // namespace mgmp
