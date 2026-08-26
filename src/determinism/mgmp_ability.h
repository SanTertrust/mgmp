// mgmp_ability.h -- stable identity for a glaiel::Ability.
//
// The replayer cannot address an ability by pointer. Measured across runs C and
// D of the same battle: 0 of 21 ability pointers and 0 of 21 actor pointers
// matched. `EvAction::ability_ptr` exists precisely to have proved that, and it
// stays in the capture -- it is just not usable as identity.
//
// The game already ships the identity we need. glaiel::Character::FindAbility
// @ RVA 0x1183E0 resolves an ability from a *string*, and the names it accepts
// are authored data:
//
//     none | move | attack | bonus | weapon | trinket | spellN | <GON name>
//
// Character's ability storage, confirmed by three independent readings
// (FindAbility, Character::GetAllAbilities @ 0x118230, Character::init
// @ 0x0F6830):
//
//     +0xD0  Ability*   move
//     +0xD8  Ability*   attack
//     +0xE0  Ability*   bonus
//     +0xE8  u32        spell vector capacity   \
//     +0xEC  u32        spell vector count       > CustomVector<Ability*>
//     +0xF0  Ability**  spell vector data       /
//
// CustomVector<T*> is { u32 capacity; u32 count; T** data; } -- 16 bytes. Its
// push @ 0x047FC0 is generic and ICF-shared across 3401 call sites, so its
// xrefs identify no owner; sites have to be filtered by the offset lea'd into
// rcx instead.
//
// WHY THE SLOT INDEX IS STABLE ACROSS RUNS. Character::init @ 0x0FBEC0 walks
// the character's authored GON `spells` array in file order, calls
// SpawnDatabase::CreateAbility per entry, and appends. Append-only, never
// sorted, no RNG, no dependence on allocation order. The convention is the
// author's own: data/tutorial_cats.gon has `abilities { spell0 Spit }`, and
// data/characters/terminator.gon's AI reads
// `do_best_multiple [attack spell0 spell1 spell2 spell3 spell4 weapon trinket]`.
//
// WHY IT IS STABLE WITHIN A BATTLE. Of those 3401 push sites only 12 target
// offset +0xE8, and exactly one of the twelve is a Character (init); the rest
// are unrelated classes' own +0xE8 fields. BreakEquipment, TransformEquipment
// and OnCharacterTransforms never touch the vector, so equipment churn does not
// shift indices.
//
// Caveat, and the reason everything here is *validated* rather than trusted:
// that filter only sees a `lea +0E8h` within 8 instructions of the call, so a
// pointer staged through another register would evade it -- the same trap that
// made a naive scan report the RNG fence list as already clean. Treat "nothing
// else appends" as strong evidence, not proof, and let the mismatch counter
// below be the thing that finds out.
//
// weapon/trinket are NOT separate slots: they are flagged abilities living in
// the same spell vector (Ability+0x8A1 / +0x8A2), so slot indexing already
// covers them and this file does not model them separately.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

// Character field offsets. Named rather than inlined because they appear in
// both directions of the lookup and in the replayer's validation.
static const size_t kCharAbilMove   = 0xD0;
static const size_t kCharAbilAttack = 0xD8;
static const size_t kCharAbilBonus  = 0xE0;
static const size_t kCharSpellCount = 0xEC;   // u32
static const size_t kCharSpellData  = 0xF0;   // Ability**

// TurnControl+0x68 is the current actor; Character+0x478/+0x480 are the
// handle-redirect GetCurrentActor applies (a generation check against the word
// eight bytes before the target). Replicated rather than called so a bad
// pointer produces a null, never a fault.
static const size_t kTcCurrentActor  = 0x68;
static const size_t kCharHandlePtr   = 0x478;
static const size_t kCharHandleGen   = 0x480;

// Ability+0x28 is the shared definition; its GON name is a std::string at +0x88
// (MSVC layout: 16-byte SSO union, size at +0x98, capacity at +0xA0).
static const size_t kAbilData     = 0x28;
static const size_t kAbilDataName = 0x88;

enum AbilitySlotKind : uint8_t {
    SLOT_NONE    = 0,   // no ability (an end-turn carries none)
    SLOT_MOVE    = 1,   // Character+0xD0
    SLOT_ATTACK  = 2,   // Character+0xD8
    SLOT_BONUS   = 3,   // Character+0xE0
    SLOT_SPELL   = 4,   // Character+0xF0, index = spell slot N
    SLOT_UNKNOWN = 255, // an ability the actor does not own -- see below
};

struct AbilitySlot {
    uint8_t kind;    // AbilitySlotKind
    uint8_t index;   // spell slot N when kind == SLOT_SPELL, else 0
};

// Reverse lookup: which slot of `character` holds `ability`?
//
// Returns SLOT_UNKNOWN when the ability is not in any of that character's
// slots. That is not necessarily a bug -- an action can in principle name an
// ability granted transiently -- but it IS the case the replayer cannot
// reproduce, so the recorder counts them and the decoder reports the count.
// A capture with a non-zero unknown count means the slot scheme is incomplete
// for that battle and needs looking at before run B is believed.
AbilitySlot ability_slot_of(const void* character, const void* ability);

// Forward resolve, the replayer's direction. Returns nullptr if the slot is
// empty or out of range -- never faults, never guesses.
void* ability_from_slot(const void* character, AbilitySlot slot);

// The ability's authored GON name ("Spit", "Roll"), read through
// Ability+0x28 -> +0x88. Writes "" and returns false if anything is unreadable.
// This is the second, independent identity: the replayer resolves by slot and
// cross-checks the name, and a disagreement between the two is itself a desync
// signal in the same spirit as hashing the pending-queue population.
bool ability_gon_name(const void* ability, char* out, size_t out_size);

// TurnControl -> current actor, replicating TurnControl::GetCurrentActor
// @ 0x8DEDA0. The recorder needs it because TurnAction+0x20 arrives null from
// the brain and is only filled in later by Character::DoAction, which runs
// after ApplyTurnAction.
void* current_actor(const void* turn_control);

} // namespace mgmp
