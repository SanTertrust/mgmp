#pragma once
// mgmp_crash -- name the throw site instead of guessing at it.
//
// WHY THIS EXISTS. A crash in a hooked process arrives as a WER signature and
// nothing else: "Mewgenics.exe / KERNELBASE.dll / e06d7363". That names the
// module RaiseException lives in, which is the same answer for every C++ throw
// in the program, so it distinguishes nothing. The log simply stops, and the
// last line before it stops is where the guessing starts.
//
// The two facts that turn that into an answer are both available at the moment
// of the throw and both gone a millisecond later:
//
//   1. WHICH TYPE was thrown. For code 0xE06D7363 the exception record carries
//      a ThrowInfo, and walking it reaches the C++ TypeDescriptor -- so
//      "std::length_error" or "class glaiel::Assert" can be printed by name
//      rather than inferred from what the code was doing.
//
//   2. WHETHER WE ARE ON THE STACK. RtlCaptureStackBackTrace in a first-chance
//      handler runs on the throwing thread with the frames still live. Every
//      return address resolves to <module>+<rva>, so "mgmp.dll+0x1234" appearing
//      in the trace attributes the crash to the mod, and its absence exonerates
//      it. That is the single most expensive question to answer any other way.
//
// FIRST-CHANCE, DELIBERATELY. An unhandled-exception filter runs after the
// stack has been unwound, by which point the frames that matter are gone. A
// vectored handler at first chance sees the throw where it happened -- but it
// also sees throws the game catches and handles perfectly well, which are
// normal and must not be mistaken for the fault. So they are RECORDED, not
// reported: a fixed-size ring holds the recent ones with no allocation and no
// I/O, and only a fatal exception makes the ring reach the log.
//
// It follows that the ring's last entries are the interesting ones, and that a
// handled throw appearing there is not evidence of anything by itself. What is
// evidence is the fatal record at the end, and whose code is in its stack.
//
// SAFETY. The handler allocates nothing, takes no lock of its own, and returns
// EXCEPTION_CONTINUE_SEARCH always -- it observes and never changes what the
// process would have done. That is the same rule the phase 1 hooks follow, and
// for the same reason: a diagnostic that alters the failure is not a
// diagnostic.

#include <cstdint>

namespace mgmp {

// `base` is the game module's load address, so stack frames inside it can be
// printed as an RVA that matches the addresses in the IDB.
void crash_install(uintptr_t base);
void crash_shutdown();

} // namespace mgmp
