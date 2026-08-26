// test_resolve_live.cpp -- run the REAL resolver over the REAL Mewgenics.exe.
//
// test_sigscan.cpp pins what the functions return on synthetic buffers.
// tools/verify_sigs.py checks the patterns against the shipped PE with an
// independent Python implementation. Neither one runs the shipping C++ against
// the shipping bytes, and that gap is where this project's bugs like to live --
// "logic that is correct in isolation, called at a moment nobody checked".
//
// So: map Mewgenics.exe as an image (LOAD_LIBRARY_AS_IMAGE_RESOURCE maps the
// sections at their virtual addresses without running a single instruction of
// it, and without resolving imports), then point the actual sigscan code at it.
//
// This exercises three things the synthetic tests cannot:
//   - PE section discovery on the real file, including picking .text
//   - REBASING. The mapped base is not 0x140000000, so every `base + rva` in
//     the resolver is genuinely relocated rather than accidentally right.
//   - the hint path on all 49 real patterns at once -- the assertion being that
//     resolution is FREE on the pinned build, which is the design's main claim.
//
// It skips, loudly and successfully, when the game is not on this machine.
#include "mgmp_sigscan.h"
#include "mgmp_sigs.generated.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace mgmp;

// The game is not a build dependency and is not in this repo, so there is no
// path worth hardcoding: argv[1] first, then MEWGENICS_EXE, then the usual
// Steam install. Anything not found makes this a SKIP, not a failure.
static const char* kExeDefault =
    "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Mewgenics\\Mewgenics.exe";

int main(int argc, char** argv) {
    const char* env  = std::getenv("MEWGENICS_EXE");
    const char* path = (argc > 1) ? argv[1] : (env ? env : kExeDefault);

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        std::printf("SKIP: %s not present on this machine\n", path);
        return 0;                      // not a failure; the game is not a build dep
    }

    // AS_IMAGE_RESOURCE maps with section alignment and never executes anything.
    // The returned handle is tagged in its low bits, so it must be masked before
    // being used as a base address.
    HMODULE raw = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!raw) {
        std::printf("FAIL: LoadLibraryEx failed (%lu)\n", GetLastError());
        return 1;
    }
    const uintptr_t base = (uintptr_t)raw & ~(uintptr_t)3;
    std::printf("mapped at %p (pinned imagebase 0x140000000 -- rebased by %+lld)\n",
                (void*)base, (long long)(base - 0x140000000ull));

    SigRegion text;
    if (!sigscan_text_section(base, &text)) {
        std::printf("FAIL: could not find the executable section\n");
        return 1;
    }
    std::printf("text: rva %08X, %.1f MB, scanner = %s\n\n",
                text.rva, text.size / (1024.0 * 1024.0), sigscan_cpu_name());
    sigscan_profile(text);

    const LARGE_INTEGER zero = {};
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    int ok = 0, moved = 0, failed = 0, scanned = 0;

    // `--force-scan` poisons every hint so that every target takes the full
    // .text sweep. That is the cost of the day the game updates, and it is the
    // only number that says whether the fallback is viable at startup or needs
    // to be made cleverer. Measuring it here means never having to guess.
    bool force = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--force-scan") == 0) force = true;

    auto run = [&](const SigTargetDesc* tab, int n, const char* kind) {
        for (int i = 0; i < n; ++i) {
            SigTargetDesc t = tab[i];
            if (force) t.rva_hint = 0x7FFFFFFF;     // guaranteed miss
            if (!tab[i].pattern) {
                std::printf("  [!] %s %-34s has no pattern\n", kind, tab[i].name);
                ++failed;
                continue;
            }
            SigResolution r = sig_resolve(base, text, t);
            if (!r.ok) {
                std::printf("  [!] %s %-34s FAILED: %s\n", kind, tab[i].name, r.why);
                ++failed;
                continue;
            }
            ++ok;
            if (!r.from_hint) ++scanned;
            // Under --force-scan the hint is a lie we told, so its "drift" is
            // ours and means nothing.
            if (r.drift && !force) { ++moved;
                std::printf("  [~] %s %-34s %s\n", kind, tab[i].name, r.why); }
        }
    };

    run(kTargetSigs, (int)(sizeof(kTargetSigs)/sizeof(kTargetSigs[0])), "target");
    run(kCallSigs,   (int)(sizeof(kCallSigs)  /sizeof(kCallSigs[0])),   "call  ");

    int dok = 0;
    for (int i = 0; i < (int)(sizeof(kSigData)/sizeof(kSigData[0])); ++i) {
        SigResolution r = sig_resolve_data(base, text, kSigData[i]);
        if (!r.ok || r.drift != 0) {
            std::printf("  [!] data %-34s %s\n", kSigData[i].name,
                        r.ok ? r.why : "FAILED");
            ++failed;
        } else {
            ++dok;
        }
    }

    QueryPerformanceCounter(&t1);
    const double ms = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    (void)zero;

    std::printf("\n%d code targets resolved, %d data targets resolved, "
                "%d failed, %d needed a scan, in %.2f ms\n",
                ok, dok, failed, scanned, ms);

    FreeLibrary(raw);

    if (failed) { std::printf("\n%d FAILURE(S)\n", failed); return 1; }

    // The claim the whole design rests on: on the build the signatures were
    // generated from, nothing is scanned. If this ever trips, either the
    // generated table is stale or a hint is wrong -- both worth failing over,
    // because the cost is paid silently at every launch otherwise.
    if (force) {
        std::printf("\n(--force-scan: every hint was poisoned, so the %d scans "
                    "above are the whole-.text fallback path)\n", scanned);
        return 0;
    }
    if (scanned) {
        std::printf("\nFAIL: %d target(s) fell through to a scan on the PINNED "
                    "build -- the hints should all have held\n", scanned);
        return 1;
    }
    if (moved) {
        std::printf("\nFAIL: %d target(s) reported drift against the build they "
                    "were generated from\n", moved);
        return 1;
    }

    std::printf("\nall live resolutions took the hint path -- zero scanning\n");
    return 0;
}
