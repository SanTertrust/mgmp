// mgmp_rng.h -- access to the game's TLS simulation RNG stream.
//
// The shared "global" simulation stream lives in thread-local storage at
// [gs:0x58][0] + 0x178 (a 32-byte xoshiro256 state).
#pragma once

#include <cstdint>

namespace mgmp {

// The TLS global stream, or nullptr if TLS is not set up on this thread yet.
uint64_t* rng_global_stream();

} // namespace mgmp
