// mgmp_record.h -- phase 2 binary event stream (the recorder).
//
// Phase 1's text trace is the wrong tool for phase 2. Three reasons:
//
//  1. Volume. A global-stream RNG draw is not a rare event; formatting and
//     flushing a line per draw is orders of magnitude more work than the draw.
//  2. Timing. `printf` + flush per draw would itself change frame pacing --
//     and frame pacing is precisely the variable Run D exists to test. The
//     instrument must not move the thing it measures.
//  3. Diffing. Text needs parsing before it can be compared; fixed-size binary
//     records can be memcmp'd stride by stride.
//
// So: one append-only binary stream, fixed-size records, formatted offline by
// tools/decode_record.py.
//
// ---------------------------------------------------------------------------
// One stream, not several
//
// RNG draws, actions, queue pushes, turns and frames all go into the *same*
// stream in one `seq` order. That is deliberate. The question phase 2 asks is
// not "what draws happened" but "which draws happened between which actions" --
// two separate files would throw away exactly the ordering that answers it.
//
// ---------------------------------------------------------------------------
// Everything is imagebase-relative
//
// Call sites are stored as RVAs, never as absolute addresses, so two runs with
// different ASLR bases produce byte-identical records for the same behaviour.
// Raw heap pointers appear in exactly one place (EV_ACTION) and are there to
// answer an open question, not to be diffed -- see the note on EvAction.
//
// ---------------------------------------------------------------------------
// The clock is QueryPerformanceCounter, deliberately
//
// The game derives its own frame dt from SDL_GetPerformanceCounter, which on
// Windows wraps QPC. Using the same counter means a timestamp in this stream is
// directly comparable to the dt the game itself saw -- a different clock would
// need a correlation step that could only add error. QPC is also cheap enough
// to sit in the draw path: with an invariant TSC it resolves through
// KUSER_SHARED_DATA without a syscall, so the ~76k records of a long battle
// cost single-digit milliseconds in total. That matters here more than usual,
// because reason 2 above is that the instrument must not move frame pacing --
// and frame pacing is the exact variable run D exists to measure.
//
// Timestamps are ticks since record_init, not raw counter values, so two
// captures are directly comparable without subtracting a per-run origin.
// EV_META carries the tick frequency; nothing else in the format can convert
// ticks to seconds.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

// Bump when any record layout below changes. The decoder refuses mismatches
// rather than silently misparsing an old capture.
// v2: EvTurn carries the cumulative draw counters, so a capture no longer
//     depends on the text trace to say how many draws were filtered out.
// v3: EvRng identifies WHICH stream a draw came from, not just whether it was
//     the TLS global one.
// v4: streams in the TLS block are reported BY OFFSET. v3 called them "HEAP"
//     because a dynamic TLS block is heap-allocated, and that mislabel hid the
//     most important fact in run C: the 11739 draws it attributed to "one
//     persistent heap stream" were TLS+0x198 -- a SECOND TLS stream, 32 bytes
//     (one xoshiro256 state) past the TLS+0x178 that was being watched. A
//     static scan then found a third at TLS+0x20 and 55 more sites that compute
//     the offset in a register. "The global stream" was never one stream.
// v5: EvAction carries the ABILITY SLOT identity (slot_kind/slot_index) and an
//     interned GON name, because the pointers it already carried turned out to
//     be unusable: 0 of 21 ability pointers and 0 of 21 actor pointers matched
//     between runs C and D. The pointers stay -- they are what proved it -- but
//     the replayer addresses abilities by slot. See mgmp_ability.h.
// v6: EvHead carries a TIMESTAMP, and EV_META carries the counter frequency
//     needed to read it. Run D asks whether queued effects advance on
//     wall-clock dt or on turn counts, and no capture before v6 could answer
//     it: EvFrame was {frame, turn} with no clock at all, so a capture could
//     not show that frames had been starved, let alone by how much. The
//     timestamp lives in the HEADER rather than in each payload for two
//     reasons -- emit_locked is the single choke point every record already
//     passes through, so one clock read covers every kind; and diff keys are
//     built from payloads, so a header field is excluded from comparison by
//     construction instead of needing a special case in each key.
static const uint32_t kRecordVersion = 6;
static const uint32_t kRecordMagic   = 0x50524D47;   // 'GMRP'

enum EvKind : uint8_t {
    EV_META   = 0,   // run header, once, first record
    EV_CLASS  = 1,   // vptr-interned RTTI class name: id -> string
    EV_THREAD = 2,   // tid -> small index, on first sight of a thread
    EV_FRAME  = 3,   // ApplicationBase::FrameBegin
    EV_TURN   = 4,   // TurnControl::NextTurn
    EV_ACTION = 5,   // TurnControl::ApplyTurnAction  -- the command boundary
    EV_QUEUE  = 6,   // TurnControl::QueueDecision    -- a push into the ring
    EV_RNG    = 7,   // one draw off a xoshiro256 stream
    EV_STREAM = 8,   // interned RNG state pointer: id -> address + class
    EV_NAME   = 9,   // interned authored string (an ability's GON name)
};

// Where a draw's state pointer lives. This is the classification that decides
// whether a non-global draw matters for lockstep:
//
//   STREAM_GLOBAL  the TLS shared stream. Both peers must agree on it.
//   STREAM_STACK   a per-call temporary in the caller's frame. Cannot carry
//                  state between calls, so it cannot desync anyone -- as long
//                  as whatever SEEDS it is deterministic.
//   STREAM_HEAP    lives in a heap allocation, i.e. inside a game object, and
//                  therefore persists across calls. THIS is the dangerous one:
//                  a per-Level or per-Character stream is state that lockstep
//                  has to keep in step, and it is invisible to a filter that
//                  only checks for the TLS address.
//   STREAM_IMAGE   a global in the module. Also persistent, also matters.
//   STREAM_TLS     another stream in the same TLS block. Per-thread and
//                  persistent, so it matters exactly as much as the one at
//                  +0x178 does -- which is the whole reason this class exists
//                  separately from HEAP. EvStream::tls_offset says which.
enum StreamClass : uint8_t {
    STREAM_GLOBAL = 0,   // the TLS+0x178 stream specifically
    STREAM_STACK  = 1,
    STREAM_HEAP   = 2,
    STREAM_IMAGE  = 3,
    STREAM_OTHER  = 4,
    STREAM_TLS    = 5,   // in the TLS block, but not +0x178
};

// How far past the TLS block base we still consider a pointer to be "in TLS".
// The block's real size is not exposed; 0x2000 comfortably covers every offset
// the static scan found (max 0x198) without swallowing unrelated heap.
static const uint32_t kTlsBlockSpan = 0x2000;

enum RngFn : uint8_t {
    RNG_INT   = 0,   // randint(int, u64*)      @ 0x14094B0B0
    RNG_FLOAT = 1,   // randfloat(double, u64*) @ 0x140158B80
    RNG_TWO   = 2,   // rand2(double, u64*)     @ 0x14094B230 -- draws TWICE
    RNG_ROLL  = 3,   // RollChance(double,double,u64*) @ 0x14094B550 -- the
                     // proc/crit gate. Hooked because it CALLS rand2, so every
                     // proc roll was previously attributed to a return address
                     // inside the RNG module and the passive that rolled was
                     // invisible. Takes NO draw when p >= 1.0.
};

// EvHead::flags for EV_RNG.
static const uint8_t kRngFnMask     = 0x03;   // RngFn
static const uint8_t kRngGlobal     = 0x04;   // state == the TLS global stream
static const uint8_t kRngThreadShift = 4;     // thread index in the high nibble

#pragma pack(push, 1)

struct EvHead {
    uint32_t seq;      // monotonic across every record in the stream
    uint8_t  kind;     // EvKind
    uint8_t  flags;    // kind-specific
    uint16_t len;      // payload bytes following this header
    uint64_t qpc;      // QueryPerformanceCounter ticks since record_init
};
static_assert(sizeof(EvHead) == 16, "EvHead is 16 bytes");

// The draw record. 24 bytes of payload, 32 with the header.
//
// `s0` is the pre-draw state word 0. It is not needed to reproduce anything --
// it is a checksum. Two runs that agree on (site, seq-order, s0, result) at
// every draw have identical RNG histories; the first record where s0 diverges
// is the exact draw at which the two runs parted company, and `site` names the
// function that did it.
struct EvRng {
    uint32_t site;         // call-site RVA (return address - imagebase)
    uint32_t turn;
    uint64_t s0;           // state[0] BEFORE the draw
    uint64_t result;       // raw bits of the return value
    uint32_t stream_id;    // 0 = TLS global; otherwise an interned EV_STREAM id
    uint8_t  stream_class; // StreamClass
    uint8_t  _pad[3];
};
static_assert(sizeof(EvRng) == 32, "EvRng layout");

// Interned RNG state pointer. `addr` is instance-specific and not diffable;
// `id` is assigned in first-appearance order and therefore IS comparable
// between runs, which is what makes stream identity usable in a diff.
struct EvStream {
    uint32_t id;
    uint8_t  cls;          // StreamClass
    uint8_t  _pad[3];
    uint64_t addr;
    uint32_t tls_offset;   // offset in the TLS block when cls is GLOBAL or TLS
    uint32_t _pad2;
};
static_assert(sizeof(EvStream) == 24, "EvStream layout");

// The command boundary.
//
// `ability_ptr` / `actor_ptr` are raw heap pointers and are the one thing in
// this format that is *not* comparable between runs. They were recorded to
// answer whether these objects land at reproducible addresses across runs of
// the same battle. **They do not** -- 0 of 21 ability pointers and 0 of 21
// actor pointers matched between runs C and D. They stay in the format because
// they are the evidence for that, and because they cost nothing; nothing may
// diff them.
//
// `slot_kind` / `slot_index` are the identity that replaced them: the ability's
// slot on its actor, which is authored GON data and reproducible by
// construction. `ability_name` is the same ability's GON name, interned -- a
// second, independent identity. The replayer resolves by slot and cross-checks
// the name; a disagreement between the two is itself a desync signal, in the
// same spirit as hashing the pending-queue population. See mgmp_ability.h for
// the layout and for why the slot order is stable.
struct EvAction {
    uint32_t turn;
    uint32_t type;         // TurnActionType: 2 ability, 3 end turn, 6 reaction, 7 invoke
    int32_t  target_x, target_y;
    int32_t  dir_x, dir_y;
    uint32_t ability_cls;  // interned EV_CLASS id, 0 if null/unresolved
    uint32_t actor_cls;
    uint64_t ability_ptr;  // NOT diffable, see above
    uint64_t actor_ptr;    // NOT diffable, see above
    uint32_t queue_depth;  // TurnControl+0x60 as this action was pulled
    uint8_t  b30, b31;     // TurnAction+0x30/+0x31, read by Ability::trigger
    uint8_t  slot_kind;    // AbilitySlotKind; 255 = not owned by the actor
    uint8_t  slot_index;   // spell slot N when slot_kind == SLOT_SPELL
    uint32_t ability_name; // interned EV_NAME id, 0 if unresolved
    uint32_t brain_cls;    // interned EV_CLASS id of the actor's Brain
                           // (Character+0x68). This is what tells the replayer
                           // which decisions were the HUMAN's: only those get
                           // injected, and every AI brain is left to re-derive
                           // its own -- which is the actual experiment.
};
static_assert(sizeof(EvAction) == 64, "EvAction layout");

// A push into the ring at TurnControl+0x48.
//
// `site` is the payload here: it names *which* of the ~65 passive/status
// callbacks queued this action. A type-6 or type-7 that appears in one run and
// not another is the earliest possible signal that a proc roll diverged, and
// `site` turns that signal straight into a class name.
struct EvQueue {
    uint32_t turn;
    uint32_t type;
    uint32_t depth_after;
    uint32_t site;         // call-site RVA
};
static_assert(sizeof(EvQueue) == 16, "EvQueue layout");

// A frame boundary (ApplicationBase::FrameBegin).
//
// There is no dt field: frame dt is EvHead::qpc minus the previous EV_FRAME's,
// and storing a derived value would only create something that can disagree
// with the timestamps it came from. The hook runs before the original
// FrameBegin, so consecutive timestamps bracket whole frames.
//
// Needs record_frames=1 in the ini -- and run D is meaningless without it. The
// captures taken before v6 all have zero EV_FRAME records.
struct EvFrame {
    uint32_t frame;
    uint32_t turn;
};

// Turn boundary, and the stream's own health check.
//
// The three counters are cumulative and exist because run A could not answer a
// basic question afterwards: it recorded 46 global-stream draws across a whole
// battle, and there was no way to tell from the capture whether that was the
// true total or whether thousands of scratch-stream draws had been filtered
// out. That number lived only in the text trace, which the next run overwrote.
// A capture has to be self-describing.
struct EvTurn {
    uint32_t turn;
    uint32_t _pad;
    uint64_t tc_ptr;
    uint64_t draws_total;     // every call through the three hooked entry points
    uint64_t draws_global;    // of those, on the TLS shared stream
    uint64_t draws_skipped;   // scratch-stream draws dropped by rng_global_only
};
static_assert(sizeof(EvTurn) == 40, "EvTurn layout");

struct EvThread {
    uint32_t tid;
    uint32_t index;
};

// EV_CLASS payload is { uint32_t id; } followed by a NUL-terminated name.
struct EvClass {
    uint32_t id;
};

#pragma pack(pop)

// ---------------------------------------------------------------------------

// Opens <path> and writes the EV_META record. Safe to call when recording is
// disabled -- every emit below then costs one predictable branch.
// Module bounds, needed to tell an in-image global apart from a heap object.
void record_set_image(uintptr_t base, uint32_t size);

void record_init(const wchar_t* path, uint32_t image_size, const char* note);
void record_shutdown();

bool record_active();

// Interns a class name by vptr. Returns the id used in EvAction, emitting an
// EV_CLASS record the first time a given vptr is seen. Returns 0 for null or
// unreadable objects.
uint32_t record_intern_class(const void* obj);

// Interns an authored string (an ability's GON name), emitting an EV_NAME
// record on first sight. Keyed by the string's own bytes rather than by an
// address, so two runs that see the same name mint the same id. Returns 0 for
// null/empty or once the table is full.
uint32_t record_intern_name(const char* s);

// Count of actions whose ability was not in any of the actor's slots
// (SLOT_UNKNOWN). Should be zero. A non-zero count means the slot scheme does
// not cover that battle and run B's result cannot be believed until it does --
// so it is carried out of the capture rather than left in a log line.
uint32_t record_unknown_slots();
void     record_note_unknown_slot();

void record_rng(uint8_t fn, uint32_t site, uint64_t s0, uint64_t result,
                bool global, const void* state);

// Interns an RNG state pointer, emitting an EV_STREAM record on first sight.
// Returns 0 for the TLS global stream. Bounded: past kStreamSlots distinct
// pointers it stops minting ids and returns kStreamOverflowId, which is itself
// the answer to "few persistent streams or many per-call temporaries?".
static const uint32_t kStreamOverflowId = 0xFFFFFFFFu;
uint32_t record_intern_stream(const void* state, uint8_t cls, uint32_t tls_offset);

// Distinct stream pointers seen, and whether the table overflowed.
void record_stream_stats(uint32_t* distinct, bool* overflowed,
                         uint64_t* stack, uint64_t* heap, uint64_t* image,
                         uint64_t* tls);
void record_action(const EvAction& a);
void record_queue(const EvQueue& q);
void record_frame(uint32_t frame);
void record_turn(uint32_t turn, const void* tc,
                 uint64_t draws_total, uint64_t draws_global);

// Bulk-writes whatever is buffered. Called on a timer-free schedule (every
// EV_TURN) and at shutdown; there is no periodic flush thread by design, since
// a background thread waking up would perturb the pacing Run D measures.
void record_flush();

// Cumulative count of draws dropped because they used a scratch state rather
// than the global stream. Carried in every EvTurn so a capture can report its
// own filtering without the text trace.
uint64_t record_skipped_draws();
void     record_note_skipped();

} // namespace mgmp
