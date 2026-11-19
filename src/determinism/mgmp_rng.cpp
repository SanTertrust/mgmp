#include "mgmp_rng.h"

#include <intrin.h>

namespace mgmp {

uint64_t* rng_global_stream() {
    // gs:[0x58] is the TEB's ThreadLocalStoragePointer; slot 0 is the main
    // module's static TLS block, and the shared stream sits at +0x178 in it.
    char** tls = (char**)__readgsqword(0x58);
    if (!tls) return nullptr;
    char* block = tls[0];
    if (!block) return nullptr;
    return (uint64_t*)(block + 0x178);
}

} // namespace mgmp
