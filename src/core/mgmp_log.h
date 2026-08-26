// mgmp_log.h -- line-atomic trace log.
//
// Format is chosen for diffing two instances against each other (phase 2), so
// every line starts with fixed-width, deterministic counters and nothing that
// varies with wall clock:
//
//   <seq:06> <turn:04> <TAG> field=value ...
//
// Raw pointers are inherently instance-specific; they are emitted only when
// tune::kPointers is on, and always at the end of the line so a diff can be
// taken on a prefix.
#pragma once

#include <cstdarg>
#include <cstdint>

namespace mgmp {

void log_init(const wchar_t* path, bool console);
void log_shutdown();

// Emits one complete line (newline appended, flushed).
void log_line(const char* tag, const char* fmt, ...);

// No seq/turn prefix -- used for the startup banner.
void log_raw(const char* fmt, ...);

void     log_set_turn(uint32_t turn);
uint32_t log_turn();
uint32_t log_bump_turn();

// --- the in-memory ring, for the ImGui log pane ------------------------------
//
// The file stays the record of a run; this is the live view of it. Every line
// that reaches the file is also copied here with a severity, so the pane never
// has to re-parse the text and the two can never disagree about what was said.
//
// Severity is classified ONCE, at emit time, in classify() -- not per frame in
// the UI. The classifier is a heuristic because the codebase has exactly one
// severity marker: `!!`, at 110 call sites, and it deliberately covers both
// "this run is over" (!! HALT) and "this is odd, proceeding" (!! host offered
// 3 options, this peer built 4). Splitting those two apart is the whole reason
// the classifier looks at the body and not just the prefix.
//
// log_line_lvl() is the escape hatch: state the level and the heuristic is
// skipped. Use it where being wrong would be misleading, not everywhere.

enum class LogLevel : uint8_t {
    Trace = 0,   // high-frequency chatter -- wire traffic, frame ticks
    Info  = 1,   // the default
    Good  = 2,   // something agreed, connected or completed
    Warn  = 3,   // odd, but the run continues
    Error = 4,   // a halt, a mismatch, or a feature turning itself off
};

struct LogEntry {
    uint32_t seq   = 0;
    uint32_t turn  = 0;
    LogLevel level = LogLevel::Info;
    char     tag[12]  = {};
    char     text[236] = {};
};

// As log_line, but states the severity instead of having it inferred.
void log_line_lvl(LogLevel level, const char* tag, const char* fmt, ...);

// Copies out every entry newer than `cursor`, oldest first, up to `cap`, and
// advances `cursor` past them. Returns how many were written.
//
// A cursor, not a snapshot: the pane keeps its own list and asks only for what
// it has not seen, so the per-frame cost is the number of NEW lines -- which is
// zero on a quiet frame. Copying the whole ring every frame would be ~600 KB of
// memcpy at 60 Hz to show the same text again.
//
// `dropped_out`, if given, receives the number of lines that fell out of the
// ring between this call and the last -- a pane that silently skips lines is
// worse than no pane, so it is counted and shown.
//
// Pass cursor = 0 on the first call to receive the whole ring.
uint32_t log_ring_fetch(uint32_t& cursor, LogEntry* out, uint32_t cap,
                        uint32_t* dropped_out);

} // namespace mgmp
