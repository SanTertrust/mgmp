#pragma once
// mgmp_bytestream -- glaiel::ByteStream's layout, in ONE place.
//
// Two modules hand the game a stream of their own now (mgmp_catsync for
// CatData, mgmp_runhist for the run history) and both drive a BIDIRECTIONAL
// serializer through it: the game's function reads or writes depending on
// ByteStream+0x00, so the host's push and the client's apply are the same call
// with a different mode. That is the whole reason neither format is
// reimplemented here.
//
// The offsets are read off ByteStream.cpp's own asserts -- ::read
// (sub_1409B3770), ::write (sub_1409B35E0), ::skip (sub_1409B31C0) and the
// destructor (sub_1409B3130).
//
// TWO OF THEM ARE LOAD-BEARING AND WORTH NAMING TWICE.
//
// kBS_ReadOwns is the destructor's first instruction, `cmp byte ptr [rcx+20h],
// 0`: the "I own the read buffer" flag. Lending the game a buffer from our /MT
// CRT with that byte left at 0 is what stops it calling ITS free() on OUR
// memory. Two heaps, one pointer, and the crash lands three screens later.
//
// kBS_Ofstream is an embedded std::ofstream, which is why a hand-zeroed stream
// cannot simply be destroyed -- the destructor dereferences a vtable that is
// null on a zeroed block. Construct that member first, always.

#include <cstdint>

namespace mgmp {

constexpr uintptr_t kBS_Mode      = 0x00;   // u32: 0 = read, 1 = write, 2 = file
constexpr uintptr_t kBS_WriteCap  = 0x08;   // u32
constexpr uintptr_t kBS_WriteLen  = 0x0C;   // u32
constexpr uintptr_t kBS_WriteBuf  = 0x10;   // void*  (game heap)
constexpr uintptr_t kBS_ReadBuf   = 0x18;   // void*
constexpr uintptr_t kBS_ReadOwns  = 0x20;   // u8   -- see above
constexpr uintptr_t kBS_ReadLen   = 0x24;   // u32
constexpr uintptr_t kBS_ReadPos   = 0x28;   // u32
constexpr uintptr_t kBS_WritePos  = 0x2C;   // u32
constexpr uintptr_t kBS_Ofstream  = 0x30;   // an embedded std::ofstream
constexpr uintptr_t kBS_SwapElem  = 0x140;  // u32, max byte-swap element size

// Over-allocated deliberately. The ofstream constructor writes past +0x100 from
// its own base, so the real struct is about 0x150; the slack costs nothing on a
// stack that only holds one of these at a time and removes a whole class of
// "the struct was bigger than we thought" corruption.
constexpr size_t kByteStreamSize = 0x200;

} // namespace mgmp
