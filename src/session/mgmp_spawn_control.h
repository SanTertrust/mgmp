#pragma once

#include <cstdint>
#include <unordered_map>

namespace mgmp {

constexpr uint32_t kFirstSpawnActor = 0x100;
constexpr uint32_t kNoActor = UINT32_MAX;
constexpr uint8_t kNoControlSlot = UINT8_MAX;

// A native Character allocation can reuse an address. Its generation is local
// to this process. Only the ID assigned when it first asks for input goes on
// the wire, so animation timing and pointer values cannot choose that ID.
class SpawnControl {
public:
    struct Entry {
        uint64_t generation;
        uint8_t owner_slot;
        uint32_t id = kNoActor;
    };

    Entry* find(const void* character, uint64_t generation) {
        const auto it = entries.find(character);
        return it != entries.end() && it->second.generation == generation ? &it->second : nullptr;
    }

    uint8_t owner_slot(const void* character, uint64_t generation) {
        const auto* entry = find(character, generation);
        return entry ? entry->owner_slot : kNoControlSlot;
    }

    Entry& record(const void* character, uint64_t generation, uint8_t owner_slot) {
        if (auto* entry = find(character, generation)) return *entry;
        return entries.insert_or_assign(character, Entry{generation, owner_slot}).first->second;
    }

    uint32_t assign_id(Entry& entry) {
        if (entry.id == kNoActor) entry.id = next_id++;
        return entry.id;
    }

    void clear() {
        entries.clear();
        next_id = kFirstSpawnActor;
    }

private:
    std::unordered_map<const void*, Entry> entries;
    uint32_t next_id = kFirstSpawnActor;
};

} // namespace mgmp
