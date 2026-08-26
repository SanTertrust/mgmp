// mgmp_rtti.h -- runtime class name of a polymorphic game object.
//
// The battle code is ~1390 Component subclasses in a 170-slot vtable hierarchy,
// and we know almost none of their field layouts. But the build ships full MSVC
// RTTI (6767 type descriptors), so `vptr -> COL -> TypeDescriptor -> name` gives
// us the concrete class of any object we are handed -- MoveAbility vs
// SpawnAbility vs TeleportAbility -- without knowing a single field offset.
// That is the cheapest identity we have, so the trace uses it everywhere.
#pragma once

#include <cstddef>

namespace mgmp {

// Writes e.g. "glaiel::MoveAbility" into buf. Never faults; on any failure
// writes "?" and returns buf.
const char* rtti_class_name(const void* obj, char* buf, size_t buf_size);

} // namespace mgmp
