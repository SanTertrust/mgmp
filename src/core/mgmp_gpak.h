// mgmp_gpak.h -- read one named entry out of the game's resources.gpak.
//
// The overlay draws the peer pointer with the game's OWN cursor art, which
// lives in textures/cursor/*.png inside the 5 GB archive. Reading it at runtime
// rather than embedding a copy means the pointer is whatever this install
// actually ships -- a game update that redraws the cursor updates ours too, and
// there is no second copy of someone else's art in our binary.
//
// The format is GPak::LoadIndex @ 0x140A434D0, which is three lines:
//
//   u32 count
//   count x { u16 namelen, char name[namelen], u32 size }   // offsets accumulate
//   ...data, starting at the stream position after the index
//
// Note what LoadIndex does NOT do: it does not read the payloads. The archive is
// opened as a plain std::ifstream and entries stream on demand, which is why a
// 5 GB file is not 5 GB of RAM -- and why doing the same here costs one open,
// one index walk and one seek.
#pragma once

#include <cstdint>
#include <cstddef>

namespace mgmp {

// Reads `name` (e.g. "textures/cursor/default.png") into a freshly malloc'd
// buffer the caller owns and must free().
//
// Returns false and leaves *out null if the archive cannot be found or opened,
// the index does not parse, or the name is not in it. Every one of those is a
// reason to draw a fallback rather than to fail loudly: the pointer is cosmetic.
//
// The archive is located beside the game executable, which is where GPak itself
// looks. Nothing is cached between calls -- this is used a handful of times at
// most, once per cursor state that actually gets displayed.
bool gpak_read(const char* name, uint8_t** out, uint32_t* out_size);

} // namespace mgmp
