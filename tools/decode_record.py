#!/usr/bin/env python
"""decode_record.py -- read and diff mgmp phase-2 binary recordings (.mgr).

    python decode_record.py run_a.mgr                 # summary
    python decode_record.py run_a.mgr --dump          # every record
    python decode_record.py run_a.mgr --sites         # draws grouped by call site
    python decode_record.py run_a.mgr run_b.mgr       # diff -- the actual point
    python decode_record.py run_a.mgr --timing        # frame pacing (run D)

The diff is what phase 2 is for. Two runs of the same battle should produce
identical (kind, site, s0, result) sequences; the first record where they do not
is the exact draw at which the runs parted company, and its call-site RVA names
the function responsible. Feed that RVA back into IDA and you have the unfenced
path -- that is the whole loop.

Timing is the other half, and it is deliberately not part of the diff. Two runs
of the same battle should agree on what happened and are free to disagree about
when, so timestamps live in the record header where diff keys cannot see them.
--timing reads them instead: it reports frame pacing and per-turn wall-clock
duration, which is what run D needs to show that frames were actually starved
and by how much.

Record layout is defined in ../src/determinism/mgmp_record.h. Keep the two in step; the
version check below will refuse a mismatch rather than misparse it.
"""

import argparse
import struct
import sys
from collections import Counter, defaultdict

RECORD_MAGIC = 0x50524D47
RECORD_VERSION = 6

(EV_META, EV_CLASS, EV_THREAD, EV_FRAME, EV_TURN,
 EV_ACTION, EV_QUEUE, EV_RNG, EV_STREAM, EV_NAME) = range(10)

KIND_NAME = {
    EV_META: "META", EV_CLASS: "CLASS", EV_THREAD: "THREAD", EV_FRAME: "FRAME",
    EV_TURN: "TURN", EV_ACTION: "ACTION", EV_QUEUE: "QUEUE", EV_RNG: "RNG",
    EV_STREAM: "STREAM", EV_NAME: "NAME",
}

# Which slot of its actor an ability came from. This is the identity the
# replayer uses: authored GON data, reproducible across runs, unlike the raw
# pointers the same record still carries. See mod/src/determinism/mgmp_ability.h.
SLOT_KIND = {0: "none", 1: "move", 2: "attack", 3: "bonus", 4: "spell",
             255: "UNKNOWN"}


def slot_label(kind, index):
    if kind is None:
        return "?"                      # pre-v5 capture: no slot recorded
    if kind == 4:
        return f"spell{index}"
    return SLOT_KIND.get(kind, f"kind{kind}")

RNG_FN = {0: "randint", 1: "randfloat", 2: "rand2", 3: "RollChance"}

# Which stream a draw came from. "heap" and "image" are the ones that matter:
# they persist across calls, so they are real sim state lockstep must control.
STREAM_CLASS = {0: "tls+178", 1: "stack", 2: "HEAP", 3: "IMAGE",
                4: "other", 5: "TLS"}

TA_TYPE = {
    1: "none", 2: "ability", 3: "endturn", 6: "reaction", 7: "invoke",
}

HEAD = struct.Struct("<IBBHQ")         # v6: seq, kind, flags, len, qpc
HEAD_V5 = struct.Struct("<IBBH")       # v1..v5: no timestamp
RNG = struct.Struct("<IIQQIB3x")  # v3: site, turn, s0, result, stream_id, cls
RNG_V2 = struct.Struct("<IIQQ")   # v1/v2: site, turn, s0, result
STREAM   = struct.Struct("<IB3xQI4x")  # v4: id, cls, addr, tls_offset
STREAM_V3 = struct.Struct("<IB3xQ")    # v3: id, cls, addr
# v5 adds the slot identity + interned GON name; v4 and older end at b31.
ACTION = struct.Struct("<IIiiiiIIQQIBBBBII")
ACTION_V4 = struct.Struct("<IIiiiiIIQQIBBH")
QUEUE = struct.Struct("<IIII")         # turn, type, depth_after, site
FRAME = struct.Struct("<II")
TURN = struct.Struct("<IIQQQQ")   # v2: turn, pad, tc, total, global, skipped
TURN_V1 = struct.Struct("<IIQ")   # v1: turn, pad, tc -- no counters
THREAD = struct.Struct("<II")
# v6 META: magic, version, image_size, pad, qpc_freq. Older is the first 12.
META = struct.Struct("<IIIIQ")
META_V5 = struct.Struct("<III")


class Record:
    __slots__ = ("seq", "kind", "flags", "payload", "qpc")

    def __init__(self, seq, kind, flags, payload, qpc=None):
        self.seq, self.kind, self.flags, self.payload = seq, kind, flags, payload
        # Ticks since record_init, or None on a pre-v6 capture. Never diffed --
        # see diff_key. It answers "when", which is run D's whole question, and
        # "when" is exactly what two correct runs are allowed to disagree on.
        self.qpc = qpc


class Capture:
    def __init__(self, path):
        self.path = path
        self.records = []
        self.classes = {0: "-"}
        self.names = {0: "-"}
        self.threads = {}
        self.note = ""
        self.streams = {}
        self.version = RECORD_VERSION
        self.qpc_freq = 0        # ticks per second; 0 on a pre-v6 capture
        self._load()

    def _load(self):
        with open(self.path, "rb") as f:
            blob = f.read()

        # The header grew in v6, but the version number lives inside the first
        # record's PAYLOAD -- so the header size has to be settled before a
        # single record can be parsed. EV_META is always record 0 and always
        # begins with the magic, so the offset at which the magic appears
        # identifies the layout with no ambiguity: 8 bytes in for a pre-v6
        # header, 16 for a v6 one.
        head = None
        if len(blob) >= 20 and struct.unpack_from("<I", blob, 16)[0] == RECORD_MAGIC:
            head = HEAD
        elif len(blob) >= 12 and struct.unpack_from("<I", blob, 8)[0] == RECORD_MAGIC:
            head = HEAD_V5
        else:
            raise SystemExit(
                f"{self.path}: no EV_META magic at either header offset. Not "
                f"an .mgr capture, or truncated before the first record.")

        off, n = 0, len(blob)
        while off + head.size <= n:
            qpc = None
            if head is HEAD:
                seq, kind, flags, length, qpc = HEAD.unpack_from(blob, off)
            else:
                seq, kind, flags, length = HEAD_V5.unpack_from(blob, off)
            off += head.size
            if off + length > n:
                print(f"{self.path}: truncated record at seq {seq} "
                      f"(game still running, or it crashed mid-flush)",
                      file=sys.stderr)
                break
            payload = blob[off:off + length]
            off += length

            if kind == EV_META:
                magic, version, _image = META_V5.unpack_from(payload, 0)
                if magic != RECORD_MAGIC:
                    raise SystemExit(f"{self.path}: bad magic {magic:#x}")
                # Older captures stay readable. They lack fields, not meaning,
                # and refusing them would discard real runs for no gain.
                if version not in (1, 2, 3, 4, 5, RECORD_VERSION):
                    raise SystemExit(
                        f"{self.path}: record version {version}, decoder knows "
                        f"1..{RECORD_VERSION}. Rebuild one or the other.")
                self.version = version
                note_at = META_V5.size
                if version >= 6:
                    _m, _v, _i, _p, self.qpc_freq = META.unpack_from(payload, 0)
                    note_at = META.size
                self.note = payload[note_at:].split(b"\0")[0].decode("utf-8", "replace")
            elif kind == EV_CLASS:
                cid = struct.unpack_from("<I", payload, 0)[0]
                self.classes[cid] = payload[4:].split(b"\0")[0].decode("utf-8", "replace")
            elif kind == EV_NAME:
                nid = struct.unpack_from("<I", payload, 0)[0]
                self.names[nid] = payload[4:].split(b"\0")[0].decode("utf-8", "replace")
            elif kind == EV_THREAD:
                tid, idx = THREAD.unpack(payload)
                self.threads[idx] = tid
            elif kind == EV_STREAM:
                if len(payload) >= STREAM.size:
                    sid, cls, addr, tls_off = STREAM.unpack(payload)
                else:
                    sid, cls, addr = STREAM_V3.unpack(payload)
                    tls_off = None
                self.streams[sid] = (cls, addr, tls_off)

            self.records.append(Record(seq, kind, flags, payload, qpc))

    # -- accessors ---------------------------------------------------------

    def draws(self):
        for r in self.records:
            if r.kind == EV_RNG:
                yield r

    def of_kind(self, kind):
        return [r for r in self.records if r.kind == kind]

    def unpack_action(self, payload):
        """Always 17 fields; the v5 tail (slot/name/brain) is None before v5.

        Old captures stay readable -- runs A, C and D predate the slot scheme
        and are still the only evidence that the pointers it replaced were
        unusable, so the decoder must not lock them out.
        """
        if len(payload) >= ACTION.size:
            (turn, typ, tx, ty, dx, dy, acls, ccls, aptr, cptr, depth,
             b30, b31, skind, sidx, nid, bcls) = ACTION.unpack(payload)
            return (turn, typ, tx, ty, dx, dy, acls, ccls, aptr, cptr, depth,
                    b30, b31, skind, sidx, nid, bcls)
        (turn, typ, tx, ty, dx, dy, acls, ccls, aptr, cptr, depth,
         b30, b31, _pad) = ACTION_V4.unpack(payload)
        return (turn, typ, tx, ty, dx, dy, acls, ccls, aptr, cptr, depth,
                b30, b31, None, None, None, None)

    def unpack_turn(self, payload):
        """(turn, tc, total, global, skipped); counters are None on a v1 capture."""
        if self.version == 1:
            turn, _pad, tc = TURN_V1.unpack(payload)
            return turn, tc, None, None, None
        turn, _pad, tc, total, glb, skipped = TURN.unpack(payload)
        return turn, tc, total, glb, skipped

    def unpack_rng(self, payload):
        """(site, turn, s0, result, stream_id, stream_class).

        Pre-v3 captures know only whether a draw was global, not which stream it
        came from, so stream_id is None there and stream_class is inferred from
        the header flag."""
        if self.version < 3:
            site, turn, s0, result = RNG_V2.unpack(payload)
            return site, turn, s0, result, None, None
        site, turn, s0, result, sid, cls = RNG.unpack(payload)
        return site, turn, s0, result, sid, cls

    def stream_label(self, sid, cls=None):
        """A stream's name. For anything in the TLS block that is its offset,
        which is the only label that means anything across runs -- the address
        differs every launch, the offset does not."""
        if not sid:
            return "tls+0x178"
        cls_r, _addr, tls_off = self.streams.get(sid, (cls, 0, None))
        if tls_off:
            return f"tls+{tls_off:#x}"
        return f"{STREAM_CLASS.get(cls_r, '?')}#{sid}"

    def describe(self, r):
        k = r.kind
        if k == EV_RNG:
            site, turn, s0, result, sid, cls = self.unpack_rng(r.payload)
            fn = RNG_FN.get(r.flags & 3, "?")
            th = r.flags >> 4
            if cls is None:
                where = "global" if (r.flags & 4) else "scratch"
            else:
                where = self.stream_label(sid, cls)
            return (f"turn={turn:<4} {fn:<9} {where:<11} site=+{site:08X} "
                    f"s0={s0:016x} -> {result:016x} th={th}")
        if k == EV_ACTION:
            (turn, typ, tx, ty, dx, dy, acls, ccls, aptr, cptr, depth,
             b30, b31, skind, sidx, nid, bcls) = self.unpack_action(r.payload)
            gon = self.names.get(nid, "?") if nid else "-"
            return (f"turn={turn:<4} type={typ}({TA_TYPE.get(typ, '?')}) "
                    f"target=({tx},{ty}) dir=({dx},{dy}) "
                    f"slot={slot_label(skind, sidx):<8} gon={gon:<16} "
                    f"abil={self.classes.get(acls, '?')} "
                    f"actor={self.classes.get(ccls, '?')} "
                    f"brain={self.classes.get(bcls, '?')} "
                    f"depth={depth} b30={b30} b31={b31} "
                    f"[ptrs {aptr:#x} {cptr:#x}]")
        if k == EV_NAME:
            nid = struct.unpack_from("<I", r.payload, 0)[0]
            return f"id={nid} {self.names.get(nid)}"
        if k == EV_QUEUE:
            turn, typ, depth, site = QUEUE.unpack(r.payload)
            return (f"turn={turn:<4} type={typ}({TA_TYPE.get(typ, '?')}) "
                    f"depth={depth} site=+{site:08X}")
        if k == EV_TURN:
            turn, tc, total, glb, skipped = self.unpack_turn(r.payload)
            if total is None:
                return f"turn={turn} tc={tc:#x}  (v1: no draw counters)"
            return (f"turn={turn} draws={total} global={glb} "
                    f"scratch_skipped={skipped} tc={tc:#x}")
        if k == EV_FRAME:
            frame, turn = FRAME.unpack(r.payload)
            return f"frame={frame} turn={turn}"
        if k == EV_CLASS:
            cid = struct.unpack_from("<I", r.payload, 0)[0]
            return f"id={cid} {self.classes.get(cid)}"
        if k == EV_THREAD:
            tid, idx = THREAD.unpack(r.payload)
            return f"index={idx} tid={tid}"
        if k == EV_STREAM:
            sid = struct.unpack_from("<I", r.payload, 0)[0]
            return f"id={sid} {self.stream_label(sid)} addr={self.streams[sid][1]:#x}"
        if k == EV_META:
            return self.note
        return f"{len(r.payload)} bytes"


# -- reports ---------------------------------------------------------------

def summary(cap):
    kinds = Counter(KIND_NAME.get(r.kind, str(r.kind)) for r in cap.records)
    print(f"{cap.path}")
    print(f"  note      : {cap.note}")
    print(f"  records   : {len(cap.records)}")
    for k, v in sorted(kinds.items(), key=lambda kv: -kv[1]):
        print(f"    {k:<8} {v}")

    if cap.qpc_freq:
        last = max((r.qpc for r in cap.records if r.qpc is not None), default=0)
        print(f"  wall      : {last / float(cap.qpc_freq):.2f} s "
              f"({cap.qpc_freq} Hz)   -- see --timing")
        if not any(r.kind == EV_FRAME for r in cap.records):
            print("  frames    : not recorded (record_frames = 0). Run D cannot")
            print("              be read from this capture -- frame pacing is")
            print("              exactly what it needs to measure.")
    else:
        print(f"  wall      : unknown -- v{cap.version} capture, timestamps "
              f"arrived in v{RECORD_VERSION}")

    draws = list(cap.draws())
    if draws:
        glb = sum(1 for r in draws if r.flags & 4)
        sites = {cap.unpack_rng(r.payload)[0] for r in draws}
        threads = {r.flags >> 4 for r in draws}
        print(f"  draws     : {len(draws)} ({glb} global) across {len(sites)} call sites")
        print(f"  threads   : {sorted(threads)} "
              f"{'<- MORE THAN ONE THREAD DRAWS' if len(threads) > 1 else ''}")

    # The last turn's cumulative counters say what the capture did NOT record.
    # A recording that filtered out most of the game's RNG looks identical to a
    # quiet one unless this is printed.
    turns = cap.of_kind(EV_TURN)
    if turns:
        _t, _tc, total, glb2, skipped = cap.unpack_turn(turns[-1].payload)
        if total is None:
            print("  hooked    : unknown -- v1 capture, counters were text-trace only")
        else:
            print(f"  hooked    : {total} calls through the three RNG entry points")
            print(f"              {glb2} on the global stream, "
                  f"{skipped} scratch (not recorded)")
            if total and skipped == 0 and glb2 == total:
                print("              -> every hooked draw was on the shared stream")
            if total < 200:
                print("              -> LOW. Most RNG in this build is likely inlined")
                print("                 into its callers and invisible here; "
                      "see mgmp_rng.h")

    stream_report(cap)
    acts = cap.of_kind(EV_ACTION)
    if acts:
        unpacked = [cap.unpack_action(r.payload) for r in acts]
        types = Counter(a[1] for a in unpacked)
        print(f"  actions   : {len(acts)}  " +
              " ".join(f"{TA_TYPE.get(t, t)}={n}" for t, n in sorted(types.items())))
        slots = Counter(slot_label(a[13], a[14]) for a in unpacked if a[13] is not None)
        if slots:
            print("  slots     : " +
                  " ".join(f"{k}={v}" for k, v in sorted(slots.items())))
            # An ability the actor does not own is the one case the replayer
            # cannot reproduce. It must be zero before run B means anything.
            unknown = sum(v for k, v in slots.items() if k == "UNKNOWN")
            if unknown:
                print(f"  !! {unknown} action(s) named an ability outside the actor's "
                      f"slots -- the slot scheme does not cover this battle")
    qs = cap.of_kind(EV_QUEUE)
    if qs:
        types = Counter(QUEUE.unpack(r.payload)[1] for r in qs)
        print(f"  queued    : {len(qs)}  " +
              " ".join(f"{TA_TYPE.get(t, t)}={n}" for t, n in sorted(types.items())))


def stream_report(cap):
    """Which streams the draws came from.

    This is the question that decides the shape of phase 3. A draw on a stack
    temporary cannot desync anyone -- the state dies with the call. A draw on a
    heap or in-image stream persists, which makes it real sim state that both
    peers have to keep identical, and a filter that only watches the TLS address
    would never have seen it.
    """
    draws = list(cap.draws())
    if not draws or cap.version < 3:
        if draws and cap.version < 3:
            print("  streams   : unknown -- pre-v3 capture only recorded "
                  "global vs not-global")
        return

    by_cls = Counter()
    by_stream = defaultdict(int)
    for r in draws:
        _site, _t, _s0, _res, sid, cls = cap.unpack_rng(r.payload)
        by_cls[cls] += 1
        # Everything except a stack temporary persists across calls: TLS
        # streams, heap streams and in-image globals alike.
        if cls in (0, 2, 3, 5):
            by_stream[(cls, sid)] += 1

    print("  streams   : " + "  ".join(
        f"{STREAM_CLASS.get(c, c)}={n}" for c, n in sorted(by_cls.items())))

    persistent = sum(n for c, n in by_cls.items() if c in (0, 2, 3, 5))
    if not persistent:
        print("              -> every non-global draw was a stack temporary.")
        print("                 Those cannot desync: the state dies with the")
        print("                 call. Only the seeding path matters.")
        return

    print(f"              -> {persistent} draws on {len(by_stream)} persistent "
          f"stream(s). Each is state")
    print("                 that survives across calls, so lockstep must keep")
    print("                 every one of them identical on both peers.")
    print()
    print(f"  {'draws':>8}  {'stream':<14} {'class':<9} address")
    for (cls, sid), n in sorted(by_stream.items(), key=lambda kv: -kv[1]):
        addr = cap.streams.get(sid, (cls, 0, None))[1] if sid else 0
        label = cap.stream_label(sid, cls)
        print(f"  {n:>8}  {label:<14} {STREAM_CLASS.get(cls, cls):<9} "
              f"{addr:#x}" if addr else
              f"  {n:>8}  {label:<14} {STREAM_CLASS.get(cls, cls):<9} -")


def sites_report(cap, only_stream=None):
    """Draws grouped by call site, split by which stream they came from.

    Every stream is shown, not just the TLS one. Filtering to global here was a
    holdover from when "not global" was assumed to mean "harmless" -- run C
    showed 11739 of 11783 draws coming from a single persistent heap stream,
    which is the one that actually matters.
    """
    by_site = defaultdict(lambda: [0, Counter(), Counter()])
    for r in cap.draws():
        site, _turn, _s0, _res, sid, cls = cap.unpack_rng(r.payload)
        key = (cls, sid)
        if only_stream is not None and sid != only_stream:
            continue
        by_site[site][0] += 1
        by_site[site][1][RNG_FN.get(r.flags & 3, "?")] += 1
        label = STREAM_CLASS.get(cls, "?") if cls is not None else (
            "GLOBAL" if (r.flags & 4) else "scratch")
        if sid:
            label += f"#{sid}"
        by_site[site][2][label] += 1

    print(f"{cap.path}: {len(by_site)} call sites\n")
    print(f"  {'draws':>8}  {'site (RVA)':<14} {'stream':<12} functions")
    for site, (count, fns, streams) in sorted(by_site.items(),
                                              key=lambda kv: -kv[1][0]):
        mix = ",".join(f"{k}x{v}" for k, v in fns.items())
        sm = ",".join(sorted(streams))
        print(f"  {count:>8}  +{site:08X}     {sm:<12} {mix}")
    print("\nResolve an RVA in IDA with:  idaapi.get_func_name(0x140000000 + rva)")


# -- timing ----------------------------------------------------------------

def pct(sorted_vals, q):
    """Nearest-rank percentile. No numpy -- this tool has no dependencies and
    a frame-dt list is a few thousand entries at most."""
    if not sorted_vals:
        return 0.0
    i = min(len(sorted_vals) - 1, max(0, int(round(q * (len(sorted_vals) - 1)))))
    return sorted_vals[i]


def timing_report(cap, stall_ms=50.0):
    """Frame pacing and per-turn wall-clock duration -- run D's instrument.

    Run D asks whether queued effects (TimeDelayStatusApplication and friends)
    advance on wall-clock dt or on turn/frame counts. Answering it needs two
    things no capture before v6 could supply: evidence that frames were in fact
    starved, and by how much. EvFrame was {frame, turn} with no clock at all,
    and record_frames defaulted off, so every earlier capture is silent on the
    question however laggy the session felt at the time.

    Read it as a pair: run the same battle calm and starved, and compare the
    per-turn draw counts from --sim against the per-turn *durations* here. If a
    turn's simulation behaviour tracks its wall-clock length, something in the
    battle is integrating against dt and lockstep needs a fixed logical step.
    """
    if not cap.qpc_freq:
        print(f"{cap.path}: no timestamps -- this is a v{cap.version} capture.")
        print("  Timing arrived in v6. Re-record with the current build.")
        return 1

    hz = float(cap.qpc_freq)
    def ms(ticks):
        return ticks * 1000.0 / hz

    last = max((r.qpc for r in cap.records if r.qpc is not None), default=0)
    print(f"{cap.path}")
    print(f"  note      : {cap.note}")
    print(f"  clock     : {cap.qpc_freq} Hz, {ms(last) / 1000.0:.2f} s wall")

    frames = [r for r in cap.records if r.kind == EV_FRAME]
    if not frames:
        print()
        print("  frames    : NONE RECORDED -- record_frames was off for this run.")
        print("              Frame pacing is the whole point of run D, and")
        print("              without EV_FRAME there is nothing to measure it")
        print("              with. Set record_frames = 1 in mgmp.ini and")
        print("              re-record; it implies hook_framebegin.")
    else:
        dts = [frames[i].qpc - frames[i - 1].qpc for i in range(1, len(frames))]
        sdt = sorted(dts)
        mean = sum(dts) / len(dts) if dts else 0
        fps = f"  ({hz / mean:.1f} fps mean)" if mean else ""
        print()
        print(f"  frames    : {len(frames)}{fps}")
        print(f"  frame dt  : p50 {ms(pct(sdt, .50)):7.2f} ms   "
              f"p90 {ms(pct(sdt, .90)):7.2f} ms")
        print(f"              p99 {ms(pct(sdt, .99)):7.2f} ms   "
              f"max {ms(sdt[-1]):7.2f} ms")

        # The stalls are the payload of a run D capture: they are the evidence
        # that the perturbation actually happened. A "starved" run whose p99
        # matches the calm run's did not starve anything, and any conclusion
        # drawn from it would be about nothing.
        stalls = [(frames[i].qpc - frames[i - 1].qpc, frames[i])
                  for i in range(1, len(frames))
                  if ms(frames[i].qpc - frames[i - 1].qpc) >= stall_ms]
        print(f"  stalls    : {len(stalls)} frame(s) over {stall_ms:.0f} ms")
        for d, r in sorted(stalls, key=lambda kv: -kv[0])[:10]:
            fno, turn = FRAME.unpack(r.payload)
            print(f"              {ms(d):9.2f} ms  at frame {fno} (turn {turn})")
        if not stalls:
            print("              -> nothing was starved. If this was meant to be")
            print("                 the perturbed half of run D, it is not one.")

    # Per-turn wall clock. EV_TURN marks a boundary, so turn N spans from its
    # own record to the next one; the last turn has no closing boundary and is
    # left out rather than guessed at.
    turns = [r for r in cap.records if r.kind == EV_TURN]
    if len(turns) >= 2:
        print()
        print(f"  {'turn':>4}  {'wall':>9}  {'frames':>7}  {'sim draws':>9}")
        # One pass, bucketed by turn. The nested-scan version was O(turns x
        # records), which a capture with EV_FRAME on makes real: frames roughly
        # double the record count that a long battle already had.
        bounds = [t.qpc for t in turns]
        nframes = [0] * len(turns)
        ndraws = [0] * len(turns)
        i = 0
        for r in cap.records:
            if r.qpc is None or r.kind not in (EV_FRAME, EV_RNG):
                continue
            while i + 1 < len(bounds) and r.qpc >= bounds[i + 1]:
                i += 1
            if r.qpc < bounds[0]:
                continue
            if r.kind == EV_FRAME:
                nframes[i] += 1
            # Sim-stream draws only: the presentation stream is driven by
            # cursor and frame pacing and is meant to move here.
            elif not cap.unpack_rng(r.payload)[4]:
                ndraws[i] += 1

        for i in range(len(turns) - 1):
            tno = cap.unpack_turn(turns[i].payload)[0]
            dur = ms(turns[i + 1].qpc - turns[i].qpc) / 1000.0
            print(f"  {tno:>4}  {dur:>8.3f}s  {nframes[i]:>7}  {ndraws[i]:>9}")
    return 0


def dump(cap):
    hz = float(cap.qpc_freq) if cap.qpc_freq else 0.0
    for r in cap.records:
        t = f"{r.qpc * 1000.0 / hz:10.2f}ms " if (hz and r.qpc is not None) else ""
        print(f"{r.seq:08} {t}{KIND_NAME.get(r.kind, r.kind):<7} {cap.describe(r)}")


# -- the diff --------------------------------------------------------------

def diff_key(cap, r, sim_only=False):
    """What must match between two runs. Deliberately excludes anything
    instance-specific: heap pointers, thread ids, and the TurnControl pointer.

    `sim_only` restricts the comparison to the simulation stream (tls+0x178)
    plus the action/queue/turn structure. That is not a convenience: the
    presentation stream is *supposed* to differ between two runs -- it is driven
    by cursor movement, camera panning and frame pacing, and run D's perturbed
    capture took 23536 presentation draws against run C's 11739. Comparing them
    would report a divergence on the first hover and bury the only comparison
    that means anything."""
    k = r.kind
    if k == EV_RNG:
        site, _turn, s0, result, sid, cls = cap.unpack_rng(r.payload)
        if sim_only and sid:          # sid 0 == tls+0x178, the sim stream
            return None
        # stream_id is assigned in first-appearance order, so it compares across
        # runs even though the underlying address does not.
        return ("RNG", r.flags & 7, site, s0, result, sid, cls)
    if k == EV_ACTION:
        (turn, typ, tx, ty, dx, dy, acls, ccls, _aptr, _cptr, depth,
         b30, b31, skind, sidx, nid, bcls) = cap.unpack_action(r.payload)
        # Pointers are deliberately excluded: they do not reproduce (0 of 21
        # matched between runs C and D) and would fail every diff on principle.
        # The slot and the GON name are the reproducible identity.
        return ("ACTION", turn, typ, tx, ty, dx, dy,
                cap.classes.get(acls), cap.classes.get(ccls), depth, b30, b31,
                skind, sidx, cap.names.get(nid) if nid else None,
                cap.classes.get(bcls) if bcls else None)
    if k == EV_QUEUE:
        turn, typ, depth, site = QUEUE.unpack(r.payload)
        return ("QUEUE", turn, typ, depth, site)
    if k == EV_TURN:
        # Counters are cumulative and deterministic given identical behaviour,
        # so they belong in the key -- a run that took a different number of
        # draws has diverged even if every recorded draw matched. Under
        # sim_only they cannot be used: they count every stream, including the
        # presentation one that is meant to differ.
        turn, _tc, total, glb, skipped = cap.unpack_turn(r.payload)
        if sim_only:
            return ("TURN", turn, glb)   # glb counts tls+0x178 only
        return ("TURN", turn, total, glb, skipped)
    # EV_FRAME is excluded outright, not compared as a count. Frame *numbers*
    # obviously differ between runs, but so does the frame *count* -- a starved
    # run has fewer frames than a calm one by construction, and that difference
    # is run D's measurement, not a divergence. Comparing frames positionally
    # made every run-D pair "diverge" at the first frame the interleaving
    # shifted, which is the loudest possible way to report nothing at all.
    # Frames are timing; timing belongs to --timing.
    return None                    # META/CLASS/THREAD/FRAME are bookkeeping


def diff(a, b, context, sim_only=False):
    sa = [(r, diff_key(a, r, sim_only)) for r in a.records]
    sb = [(r, diff_key(b, r, sim_only)) for r in b.records]
    sa = [(r, k) for r, k in sa if k is not None]
    sb = [(r, k) for r, k in sb if k is not None]

    scope = ("simulation stream (tls+0x178) + action/queue/turn structure"
             if sim_only else "every recorded stream")
    # Timestamps are in the record header, and diff keys are built from
    # payloads, so timing is excluded here by construction -- as it must be:
    # two runs of the same battle are *expected* to disagree about when things
    # happened. Use --timing to look at the clock; use this to look at the sim.
    print(f"comparing: {scope}")
    print(f"A: {a.path}  ({len(sa)} comparable records)  {a.note}")
    print(f"B: {b.path}  ({len(sb)} comparable records)  {b.note}")
    print()

    n = min(len(sa), len(sb))
    first = None
    for i in range(n):
        if sa[i][1] != sb[i][1]:
            first = i
            break

    if first is None:
        if len(sa) == len(sb):
            if sim_only:
                print("IDENTICAL -- the simulation stream did not diverge.")
                print()
                print("Presentation activity (cursor, camera, frame pacing) did")
                print("not perturb tls+0x178. That is the separation holding.")
                return 0
            print("IDENTICAL -- no divergence in the recorded stream.")
            print()
            print("Note what this does and does not prove: the recorder sees the")
            print("~210 call-based RNG sites, not draws the compiler inlined into")
            print("their callers. See mgmp_rng.h.")
            return 0
        longer, name = (sa, "A") if len(sa) > len(sb) else (sb, "B")
        extra = abs(len(sa) - len(sb))
        print(f"Common prefix matches, but {name} has "
              f"{extra} extra record{'' if extra == 1 else 's'}:")
        cap = a if name == "A" else b
        for r, _k in longer[n:n + context]:
            print(f"  {r.seq:08} {KIND_NAME.get(r.kind, r.kind):<7} {cap.describe(r)}")
        return 1

    print(f"DIVERGED at comparable record {first}.")
    print()
    lo = max(0, first - context)
    hi = min(n, first + context + 1)
    for i in range(lo, hi):
        mark = ">>" if i == first else "  "
        ra, rb = sa[i][0], sb[i][0]
        print(f"{mark} [{i}]")
        print(f"     A {KIND_NAME.get(ra.kind, ra.kind):<7} {a.describe(ra)}")
        print(f"     B {KIND_NAME.get(rb.kind, rb.kind):<7} {b.describe(rb)}")

    ra = sa[first][0]
    if ra.kind == EV_RNG:
        site = a.unpack_rng(ra.payload)[0]
        print()
        print(f"First divergent draw is at call site +{site:08X}.")
        print(f"Resolve it:  idaapi.get_func_name(0x{0x140000000 + site:X})")
        print("That function is drawing from the shared stream on a path that")
        print("differs between the two runs -- give it a scratch stream.")
    elif ra.kind == EV_QUEUE:
        site = QUEUE.unpack(ra.payload)[3]
        print()
        print(f"First divergence is a queue push from +{site:08X}.")
        print(f"Resolve it:  idaapi.get_func_name(0x{0x140000000 + site:X})")
        print("A passive fired in one run and not the other, which means")
        print("something upstream of it already diverged -- look at the draws")
        print("immediately before this point.")
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", metavar="FILE")
    ap.add_argument("--dump", action="store_true", help="print every record")
    ap.add_argument("--sites", action="store_true", help="group draws by call site")
    ap.add_argument("--timing", action="store_true",
                    help="frame pacing and per-turn wall-clock duration. This "
                         "is run D's instrument: it shows whether frames were "
                         "actually starved, and how long each turn took in real "
                         "time. Needs a v6 capture with record_frames = 1.")
    ap.add_argument("--stall-ms", type=float, default=50.0,
                    help="frame dt at or above which --timing calls a stall "
                         "(default 50)")
    ap.add_argument("-C", "--context", type=int, default=6,
                    help="records of context around a divergence (default 6)")
    ap.add_argument("--sim", action="store_true",
                    help="diff only the simulation stream (tls+0x178) plus the "
                         "action/queue/turn structure. Use this when the two "
                         "runs deliberately differ in presentation activity -- "
                         "the tls+0x198 stream is driven by cursor and frame "
                         "pacing and is MEANT to differ.")
    args = ap.parse_args()

    caps = [Capture(p) for p in args.files]

    if len(caps) == 1:
        if args.dump:
            dump(caps[0])
        elif args.sites:
            sites_report(caps[0])
        elif args.timing:
            return timing_report(caps[0], args.stall_ms)
        else:
            summary(caps[0])
        return 0

    if len(caps) != 2:
        raise SystemExit("give one file to inspect, or two to diff")
    if args.timing:
        # Two timing reports side by side, not a diff: timestamps are the one
        # thing two correct runs are expected to disagree about.
        rc = 0
        for c in caps:
            rc |= timing_report(c, args.stall_ms)
            print()
        return rc
    return diff(caps[0], caps[1], args.context, args.sim)


if __name__ == "__main__":
    sys.exit(main())
