#include "mgmp_ability.h"

#include "mgmp_mem.h"

#include <cstring>

namespace mgmp {
namespace {

// Every read below goes through mem_read: these offsets are recovered, not
// declared, and a wrong one must degrade to "unknown" rather than fault.
const void* read_ptr(const void* base, size_t off) {
    if (!base) return nullptr;
    const void* p = nullptr;
    if (!mem_read((const uint8_t*)base + off, &p, sizeof(p))) return nullptr;
    return p;
}

uint32_t read_u32(const void* base, size_t off) {
    if (!base) return 0;
    uint32_t v = 0;
    if (!mem_read((const uint8_t*)base + off, &v, sizeof(v))) return 0;
    return v;
}

// A count read out of a live object is only as trustworthy as the offset it
// came from. Cap the walk so a garbage count cannot turn into a long scan on
// the hot path; a real character has a handful of spells.
const uint32_t kMaxSpellSlots = 64;

} // namespace

AbilitySlot ability_slot_of(const void* character, const void* ability) {
    AbilitySlot s{ SLOT_NONE, 0 };
    if (!ability) return s;                    // an end-turn: genuinely no ability
    s.kind = SLOT_UNKNOWN;
    if (!character) return s;

    if (read_ptr(character, kCharAbilMove)   == ability) { s.kind = SLOT_MOVE;   return s; }
    if (read_ptr(character, kCharAbilAttack) == ability) { s.kind = SLOT_ATTACK; return s; }
    if (read_ptr(character, kCharAbilBonus)  == ability) { s.kind = SLOT_BONUS;  return s; }

    uint32_t    count = read_u32(character, kCharSpellCount);
    const void* data  = read_ptr(character, kCharSpellData);
    if (!data) return s;
    if (count > kMaxSpellSlots) count = kMaxSpellSlots;

    for (uint32_t i = 0; i < count; ++i) {
        const void* slot = nullptr;
        if (!mem_read((const uint8_t*)data + i * sizeof(void*), &slot, sizeof(slot)))
            break;
        if (slot == ability) {
            s.kind  = SLOT_SPELL;
            s.index = (uint8_t)i;
            return s;
        }
    }
    return s;
}

void* ability_from_slot(const void* character, AbilitySlot slot) {
    if (!character) return nullptr;
    switch (slot.kind) {
    case SLOT_NONE:   return nullptr;
    case SLOT_MOVE:   return (void*)read_ptr(character, kCharAbilMove);
    case SLOT_ATTACK: return (void*)read_ptr(character, kCharAbilAttack);
    case SLOT_BONUS:  return (void*)read_ptr(character, kCharAbilBonus);
    case SLOT_SPELL:  break;
    default:          return nullptr;   // SLOT_UNKNOWN is not reproducible
    }

    uint32_t    count = read_u32(character, kCharSpellCount);
    const void* data  = read_ptr(character, kCharSpellData);
    if (!data || slot.index >= count || count > kMaxSpellSlots) return nullptr;

    void* p = nullptr;
    if (!mem_read((const uint8_t*)data + slot.index * sizeof(void*), &p, sizeof(p)))
        return nullptr;
    return p;
}

bool ability_gon_name(const void* ability, char* out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = 0;
    if (!ability) return false;

    const void* data = read_ptr(ability, kAbilData);
    if (!data) return false;

    // MSVC std::string: union { char buf[16]; char* ptr; } at +0, size at +16,
    // capacity at +24. Capacity > 15 means the heap form. FindAbility performs
    // exactly this test at (Ability+0x28)+0xA0.
    const uint8_t* str = (const uint8_t*)data + kAbilDataName;
    uint64_t size = 0, cap = 0;
    if (!mem_read(str + 16, &size, sizeof(size))) return false;
    if (!mem_read(str + 24, &cap,  sizeof(cap)))  return false;
    if (size >= out_size || size > 4096) return false;   // not a plausible name

    const void* chars = str;
    if (cap > 15) {
        if (!mem_read(str, &chars, sizeof(chars)) || !chars) return false;
    }
    if (!mem_read(chars, out, (size_t)size)) { out[0] = 0; return false; }
    out[size] = 0;
    return true;
}

void* current_actor(const void* turn_control) {
    if (!turn_control) return nullptr;

    void* actor = (void*)read_ptr(turn_control, kTcCurrentActor);
    if (!actor) return nullptr;

    // The handle redirect GetCurrentActor applies: if Character+0x478 points at
    // something whose preceding qword still matches the generation stored at
    // Character+0x480, that target is the real actor (possession / mount).
    const void* h = read_ptr(actor, kCharHandlePtr);
    if (h) {
        uint64_t stamp = 0, gen = 0;
        if (mem_read((const uint8_t*)h - 8, &stamp, sizeof(stamp)) &&
            mem_read((const uint8_t*)actor + kCharHandleGen, &gen, sizeof(gen)) &&
            stamp == gen)
            return (void*)h;
    }
    return actor;
}

} // namespace mgmp
