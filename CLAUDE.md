# Mewgenics — reverse engineering for multiplayer

**Goal: add multiplayer to Mewgenics.** Everything here serves that.

**Shape: shared-run co-op.** Two players play one adventure together — the host
owns the run, both players split its cats, and they play the whole thing.

**Scope: the HOUSE is out.** Breeding, furniture placement and `HouseInventory`
are not replicated. What must be flawless is the **adventure**: map → node →
battle / shop / event / level-up → repeat.

**Mechanism: two of them, because the two layers have opposite properties.**

| layer | serializable? | deterministic? | so sync by |
|---|---|---|---|
| **meta** (map, shop, level-up, inventory) | **yes** — `SerializeCatData`, the `Inventory` serializer, `SQLSaveFile` | no | **host-authoritative state sync** |
| **battle** | no — there is no `Character`/`Status`/`TacticsGrid` serializer | **yes** — measured byte-identical under 5.4x timing variation | **deterministic lockstep over TCP** |

Each layer supports exactly one of the two, and it is not the same one. The
meta layer never has to be made deterministic, because only the host rolls it.
Don't re-litigate either half without new evidence.

**What this file is for:** things that cannot be re-derived quickly — addresses,
struct offsets, measured results, and verdicts that closed an investigation.
How a module works belongs in its source, which is heavily commented; this file
records what the **binary** does.

---

## Status

Battle lockstep, the meta layer and reconnect all run live. A real internet
session has been played end to end: 109 map nodes, 4 battles, 59 turn hashes all
agreeing, 172 actions applied per peer, 0 desyncs. Peer cursors and aim previews
work on both surfaces. There is an ImGui debug panel.

The one divergence seen in that session is open item 1 below. The client drew
2.5x the host's frames throughout — the frame-rate asymmetry the state fence
exists to catch — and the fence caught nothing across 6327 suppressor calls and
3330 `TacticsObject::Move` round trips.

**Open, in priority order:**

1. **Nothing hashes the window between a battle's last turn hash and the next
   node hash** — see "The post-battle gap" below.
2. **No turn hash has ever been compared after a reconnect.** The mechanism runs
   and the catch-up demonstrably replays, but the one run that got there ended at
   turn 2 without crossing another boundary. `0 desync(s)` with no `AGREES` after
   it means *nothing was compared*, not *everything matched*.
3. Detection debt: equipment is not in `state_hash`; **facing is not in it either
   and cannot be** (see the facing section). `STATEDUMP` is written and
   unit-tested but **has never fired in a real desync**.
4. Validation debt: `hook_timedelay` has never run against real content;
   `RollChance` is unhooked in every lockstep run, so procs are unattributed. It
   is not separately gated — `debug.record` turns on all four RNG hooks including
   it, so "unhooked in every lockstep run" is a consequence of running those
   sessions with `record` off, not a missing switch.
5. The peer pointer is resolution- and letterbox-independent but **not
   camera-independent** — a deliberate trade. Two peers with differently panned
   cameras point at different board positions; the tile reticle stays right.
6. **Signature resolution has never run against an actually-updated build.** It
   is verified against this one three ways (unit tests, an independent Python
   check of the shipped PE, and the shipping resolver run over the real image
   with every hint poisoned), but "the patterns still match after a recompile" is
   a prediction, not a measurement. First thing to do after an update: run
   `gen_sigs.py` on the new `.i64` and read its report — whatever it lists as
   FAIL is the list a human has to look at. The **three SDL_DYNAPI slots are
   still hardcoded** and will simply be wrong.
7. **Reach, not correctness:** transport is direct-IP only, so a real session
   needs port forwarding at one end — `SteamNetworkingMessages002` is linked and
   unused, and there is no lobby API to move to. And **3–4 players has never been
   run**: the peer envelope, `PEERS` and the control split are all written for it,
   but written is not measured.

### The post-battle gap — the window no hash covers

A full clean session ended with one `NODEHASH` mismatch, at the last node of the
run, in **only the `rng` component**. `history`, `cats` and `inventory` all
agreed, and the mod layer is exonerated — each of these was checked rather than
assumed:

| hypothesis | how it died |
|---|---|
| the actions diverged | 172 `APPLY` on each peer; the final turn's `APPLY`/`DOACTION`/`TRIGGER` stream diffs character-for-character, only pointers differing |
| an extra `NextTurn` (its shuffle draws) | 59 `NEXTTURN` on both |
| the cat/inventory/history serializers move the stream | earlier nodes applied cats plus history and inventory and **agreed** |
| the hold-then-drain path | an earlier node's client window is line-for-line the same shape (level-up → hold → follow → apply → hash) and agreed |
| a per-frame draw on the sim stream | the same 2.5x frame disparity was present at a node that agreed |

So the divergence is in the **game's own post-battle resolution**, in the gap
between the last per-turn hash and the next node hash. Both peers' last common
reading was an identical turn hash. **The last turn's combat resolution, the
reward cluster and the level-up are all inside that gap and nothing hashes
them** — ~88 post-battle draws are measured on the final turn of a real battle.
The one thing separating the diverging node from the identical-shaped node that
agreed is that it **followed the boss**.

**It was harmless, and the reason it was harmless is the reason it is easy to
miss.** `EnterNode` re-seeds `TLS+0x178` unconditionally, so the drift was wiped
on entering the next node — 59 turns of lockstep never saw it. Stream position
was the *only* place it was visible. But the same window writes cat state, and a
divergence there would not be wiped.

**What would name it:** `debug.record = true` on both peers over a boss fight,
diffed with `decode_record.py --sim`. That turns on `RollChance` too, so the proc
surface is attributed rather than inferred.

**Two counter bookkeeping bugs, both cosmetic**, recorded because a counter you
cannot trust is worthless on the day it matters: `LOCKSTEP done:` over-counts
applies by one on the client (most likely the own-cat pending branch) while the
actual `APPLY` lines are equal on both; and `NET closed:` disagrees by two frames
between sent and received while the **byte** counts match exactly in both
directions — framing-level counting, not lost data.

---

## The binary

- `Mewgenics.exe` (x64 PE) + `Mewgenics.exe.i64` (analyzed IDA db).
- Imagebase `0x140000000`, image size `0x156B000`, 63358 functions.
- SHA256 `c3a41e436a93fa58cd386ec46dad5c2a6f21a583d33c3a57a15a2604c726439e` —
  **every address in this file is pinned to this build.** Re-derive after any
  game update. **The MOD is not pinned**: it resolves all 49 of its addresses by
  unique byte signature and treats the pinned RVA as a hint, so a patch can move
  addresses without breaking it. The addresses *in this file* are still pinned,
  because a document cannot re-derive itself.
- Native C++, namespace `glaiel::`. No scripting VM.
- SDL2 + FAudio. Config/data is GON (`GonObject`). UI/animation is a hand-written
  **Flash SWF player** with an AVM2 interpreter (`glaiel::swf::*`). Saves are
  sqlite3 (`SQLSaveFile`) plus `ByteStream` + LZ4.
- Links `steam_api64.dll`: `SteamNetworkingSockets012`,
  `SteamNetworkingMessages002`, `SteamNetworkingUtils004`, `SteamNetworking006`,
  `SteamUser023`, `SteamFriends018`, `SteamUtils010`, `SteamInput006`.
  **No `SteamMatchmaking`** (no lobby API linked).
- **`resources.gpak` (~5 GB) is NOT loaded into RAM at startup** — verified.
  `initSystems` @ `0x1409A7D53` calls `GPak::LoadIndex` @ `0x140A434D0`, which
  opens the file as a plain `std::ifstream(in|binary)` (`GPak::OpenStream` @
  `0x140A43B00`) and reads **only the index**: `u32 count`, then per entry
  `{ u16 namelen, char name[], u32 size }`, accumulating `offset += size` into a
  `std::map`. Data-section base is `tellg()` after the index, stored at
  `this+0x304`; payloads stream on demand. High RSS is *decoded* assets.

---

Local Python 3.13 is `python` (NOT `python3`).

### `tools/`

- **`gpak.py`** — extracts the shipped `.gon` data from `resources.gpak`, which
  lives with the game install and not here. 19900 entries, **447 `.gon`**
  (5.7 MB) — every ability, character, event and item definition in plain text.

  ```python
  from gpak import GPak
  g = GPak(r"<game install>/resources.gpak")
  txt = g.read("data/abilities/finalboss_abilities.gon").decode("utf-8")
  ```

  Often the fastest way to answer "does any *content* reach this path", which is
  a different and usually more decisive question than "is this code path
  reachable". It has closed two investigations outright (fence target #1 and the
  `TimeDelayStatusApplication` scope). **A static route can beat a capture:** a
  capture only proves the paths it happened to hit are clean.

- **`ida/gen_sigs.py`** — derives a unique wildcarded signature for every hook
  and call target and writes `src/core/mgmp_sigs.generated.h`. Runs inside IDA.
  **This is the migration path after a game update**: open the new `.i64`, run
  it, rebuild. It reads the RVAs out of `mgmp_addresses.h` itself, so the two
  cannot drift apart.
- **`verify_sigs.py`** — checks the generated table against the **shipped PE**
  with an independent implementation: every pattern unique in `.text`, at the
  RVA it claims, and every data displacement recovering the right address.
  Deliberately not sharing code with the generator, per the rule that a call
  target is verified against `Mewgenics.exe` and not against IDA's notes.
- **`decode_record.py`** — `<file>` summary, `--dump`, `--sites`, `--sim` diff of
  two captures, `--timing` for frame pacing. Reads format versions 1–6.
- **`net_test.ps1`** — drives the two-instance loopback lockstep test.
- **`save_snapshot.ps1`** `-Snapshot <name>` / `-Restore <name>` / `-List`. The
  campaign save is one ~45 KB file, so A/B capture of the same battle is a file
  copy. Restore refuses while Mewgenics is running (the game rewrites the save on
  exit and would silently undo it). **Repeating a battle is the whole capture
  methodology**, which is why `tune::kHookSaveScum` exists.

Tools take paths as `argv[1]`, then `MEWGENICS_EXE` (the game) or `MGMP_ROOT`
(the mod directory), then a derived default. **None may hardcode a local path** —
it is useless to anyone else and leaks a username into a public repository.
`gen_sigs.py` needs a third fallback because it runs inside IDA, where an
`exec()`-based `run_script` may not set `__file__`: it falls back to
`idc.get_idb_path()`'s directory.

### Symbol recovery (done — 751 exact names, 3580 prototypes)

Two channels, both validated before writing:

1. **Assert `__FUNCSIG__` strings** — the release build ships asserts, so `.rdata`
   holds full C++ signatures and the original `__FILE__` paths. 194 exact names.
   A source file's xref list is the union of every function in it that asserts.
2. **`??_7?$_Func_impl_no_alloc@...` vftables** (the richer one) — the symbol
   embeds the *enclosing* function's mangled name, and that function is the only
   code xref to the vftable that isn't one of its own 6 slots. 552 exact names.

376 more are `..__inner_N` = lambda bodies where the enclosing name is known but
the address is ambiguous. **Majority-of-lambda-xrefs resolves these** (this is
how `MapScreen::EnterNode` and `House::init` were pinned).

Struct layouts confirmed empirically: `Vec2D = {double x,y}` 16B, `Vec3D` 24B,
`iVec2D = {int x,y}` 8B.

**ABI note:** MSVC x64 passes any aggregate that is not exactly 1/2/4/8 bytes by
hidden pointer, so exact sizes of `std::string`/`std::function` don't affect
argument passing — opaque blobs are safe. Only mis-declaring something as exactly
8 bytes corrupts a prototype.

**Dead end — do not retry:** vtable-slot name propagation. MSVC **ICF folds all
empty virtuals into one function** (surfaces as `FAudio_SetDebugConfiguration`
occupying ~1600 slots), so only 3 of 172 `Component` slots had a seed name.
Vtable families identify by slot count: 170/171/172 = the `Component` hierarchy
(1390 tables), 28 = another, plus sz55/sz34.

---

## Battle architecture

- Turn-based tactics. `TurnControl::NextTurn` has **no direct callers** —
  dispatched virtually. Natural command boundary is `TurnAction` /
  `Ability::trigger(TurnAction)`.
- Everything is a `Component` in a 170-slot vtable hierarchy: ~1390 classes.
- Heavily event-driven and **deferred**: `OnAppliedToCharacter`, `OnTrigger`,
  `OnTurnBeginQueued`/`OnTurnEndQueued`, `late_update`, plus `DelayedTrigger`,
  `TimeDelayStatusApplication`, `TransformInXTurns`, `QueueUseAbility`.
- Frame driver is `ApplicationBase::FrameBegin`/`FrameEnd` using
  `SDL_GetPerformanceCounter`. No fixed-timestep accumulator.
- **There is no serializer for `Character`, `TacticsGrid`, `Status`, `Passive`,
  `TacticsObject`** — no mid-battle save exists. This is the single decisive
  reason state sync was rejected for the battle layer.

### The character list, and why turn order is not an identity

**Turn order is re-shuffled every round, from the simulation stream.**
`TurnControl::NextTurn` builds a candidate list then calls `sub_140085F80` — an
**inlined Fisher-Yates shuffle** whose xoshiro256 step is written longhand and
which takes the state pointer as `*(TLS)+0x178`. Deterministic and proven so —
but a cat's position in turn order changes every round, so it can never be an
index.

**The pre-shuffle source is the battle's character list:**

```
TurnControl+0x18 -> +0x08 -> +0x20 -> +0x1F90
    { u32 refcount@0; u32 pad@4; u32 cap@8; u32 count@12; Character** data@16 }
```

`sub_14004A550` is the accessor. Its order is **spawn order** — authored, and
identical on both peers given the same save. **Verified live: two instances
printed byte-identical 29-cat rosters** in separate processes with completely
different heap addresses.

The mod snapshots this list once per battle rather than reading it live, because
summons append to it and deaths may remove from it, either of which would
renumber every cat mid-battle. Pointers are stable *within* one battle (across
runs, 0 of 21 matched).

**Summons are therefore NOT in the snapshot**, and both peers let their own brain
drive them. Correct as long as summons are AI-driven — observed working. **A
summon with a `PlayerBrain` would be an unhandled double-control desync**:
neither peer owns it, both wait for a local click. If you can ever click a
summon, that is the bug.

### Character state — the fields `state_hash` covers

Every one has **two independent readings**, pinned as IDB comments.

| Field | Offset | How it was confirmed |
|---|---|---|
| current HP | `Character+0x4B0` `int32` | `AbilityHealthThreshold::late_update` compares it to `threshold_min/max`; `Die` zeroes it; `ReceiveDamage__inner_0` touches it 21x |
| shield | `Character+0x4B4` `int32` | `GainShield` writes `this+301`; same field `late_update` adds when the GON says `count_shield` |
| max HP | `Character+0x4BC` `int32` | the `deval` base for `threshold`/`_min`/`_max`; `recompute_stats` reads it 4x |
| dead | `Character+0x4C2` `bool` | `Die`'s entry guard; set by `Die` and `OnCorpsePop`, read by 6 others |
| facing | `Character+0x388` `iVec2D` | `Character::Face`'s only durable write |
| **tile** | `TacticsObject+0x48` `iVec2D` | `::Move` copies the old value to `+0x50` then writes the destination here |
| the link | `Character+0x60` ⇄ `TacticsObject+0x98` | two readings each way |

The decisive one is `Character::Die` doing `mov [rcx+4B0h], rdi` — a **qword**
store zeroing HP and shield together, proving they are adjacent `int32`s.

**A decoy:** `GainShield` and `GainMana` both contain `cmp byte ptr [rbx+4B0h], 0`
but on a *different object* (`*(this+0x18)+0x8`). `+0x4B0` is an int, not a bool.

**Also useful:** `CatStats` is seven `int32`s — `str, dex, con, int, spd, cha, lck`
at `+0x00`…`+0x18` (`CatStats::get_stat`, CatData.cpp:201). **Not** health.

**Status/passive counts are deliberately NOT hashed.** The passive list at
`Character+0xE70` is `{u32 cap, u32 count, Passive** data}` but it is a **lazily
rebuilt cache** with a dirty flag at `+0xE50` and an iteration guard at `+0xEB4`.
Anything that enumerates passives can trigger the rebuild — including a tooltip
on one peer and not the other — so hashing its count would manufacture desyncs.

**The hash gates itself on evidence, not on these offsets being right.** Per cat
it walks `Character → TacticsObject → Character` and requires the round trip,
plus a range check; all cats must pass or `state_hash` stays 0 and the log names
the failures. Heap layout differs between processes, so hashing the wrong bytes
is a *guaranteed* false desync on turn 1 — strictly worse than no state hash.

**Equipment is NOT in `state_hash`, and that is how a real bug got missed.** Two
peers whose cat 19 differed only in equipment agreed on every turn hash, until
`!! HALT at turn 4: cat 19 slot 4:4 (gon 'tk_GlowingCoin') is empty on this peer`.
What caught it was the **ability cross-check** (resolve by slot, validate by GON
name) — the only thing in the battle layer that looks at what a cat *can do*
rather than what has *happened to* it.

#### Facing is simulation state that presentation writes

**`sub_14011C6F0` is the backstab test**, called from
`glaiel::Character::receive_damage_passives` @ `0x14010ECE7`. It reads
`Character+0xC80` (the GON key `can_be_backstabbed`, parsed at `Character::init`
@ `0x1400F9812`) and compares `Character+0x388` — the facing — against the
direction the hit came from. **Facing decides damage.** Neighbouring flags:
`+0xC81` immunity, `+0xC82` front, `+0xC83`/`+0xC84` the all-directions variants
(`BackstabImmunity`, `BackstabFront`, `BackstabAllDirections`, `WideBackstab` in
the shipped GON).

Facing is written by the **aim preview**, `Brain::UpdateDecision` @ `0x1401377C9`,
under a wall-clock gate (`comisd xmm7, [rdi+2A8h] / jnb`). Two peers at different
frame rates land on opposite sides of that branch. Symptom: 49 turns of
byte-identical hashes, then one melee landing for 12 on one peer and 9 on the
other with `rng` identical and the action stream character-for-character the
same, differing only in `face=(1,0)` against `face=(-1,0)` **on the defender**.

**Fix: freeze the preview.** Hook `T_UpdateDecision` @ RVA `0x001374D0` snapshots
`Character+0x388` on entry and puts it back on exit. `Character::Face` is called
**exactly once** inside that function, so the write being undone is unambiguous;
the committed direction is written by `Ability::trigger` and
`Character::DoAction`, which are outside it. Cost: the cat no longer visibly
turns while you aim it.

**Facing still cannot go in the hash, and this is not laziness.**
`CombatAnimation::update` @ `0x1402A86F8`/`0x1402A8745` also calls `Face`, toward
`CombatAnimation+0x110`. The *value* is deterministic, but the animation is in
flight at different points on two machines, so hashing facing would manufacture
mismatches. It is **traced** in the per-turn `delta:` lines instead — the one
field traced that the hash does not cover, so a future divergence shows up in a
log diff rather than as three missing HP forty turns later.

**Method for any divergence:** the hashes only say *that* the peers diverged.
Filter both logs to the turn and diff the `APPLY`/`DOACTION`/`TRIGGER` lines for
the first differing target. Note that an agreeing per-turn hash is not sufficient
evidence when `state_hash` is 0.

### Departed cats — the false desync the state hash used to manufacture

**A cat can LEAVE the live character list mid-battle, and its snapshot entry
outlives its membership.** That object is parked off the board at the
**`(-5000,-5000)` sentinel**, and `Character::get_affecting_elements` @
`0x14011A840` feeds that raw tile into `sub_14082CB30`, a grid walk **with no
bounds check**. The result is whatever the heap happens to hold. Measured on the
same boss fight twice: one run had the host read `0x00000400` while the client
faulted; the next had the two **swap roles**. Which peer "wins" is a coin flip,
which is exactly what a state hash must never contain.

Symptom: `!! HALT ... (state only)` with **`rng` identical every turn** and the
cat table differing on one row by `elem` alone.

Two fixes: `read_cat_state` no longer calls `get_affecting_elements` for a
departed cat (the fault source is removed rather than caught), and `state_hash`
hashes **membership**, then stops for that cat.

**Detect roster change by MEMBERSHIP, never by length.** The old check was
`live == g.cat_count`. A turn that appends a summon and removes something else
leaves the count unchanged, so it printed "nothing was summoned or removed"
through a real roster change. Pointer-level comparison prints `ROSTER +A/-B` and
immediately showed the true shape of a RatBomb boss fight:

```
turn 2   chars=46/45   ROSTER +1/-0     <- a bomb appears
turn 4   chars=45/45   ROSTER +1/-1     <- and a DIFFERENT entry leaves
```

### `TurnAction` — the command payload

Passed by value, so it arrives as a hidden pointer under the MSVC x64 ABI.
**`sizeof(TurnAction) == 0x88`** and it **contains a `std::function`** at `+0x78`.

| Offset | Type | Meaning |
|---|---|---|
| `+0x00` | `u32` | type: `1` no decision (poll), `2` use ability, `3` end turn, `6` reaction broadcast, `7` invoke the `std::function` |
| `+0x04` | — | **padding, uninitialized** |
| `+0x08` | `Ability*` | equals the `this` of the following `Ability::trigger` |
| `+0x10` | `iVec2D` | target tile |
| `+0x18` | `iVec2D` | direction — observed `(0,0) (1,0) (0,1) (-1,0)` |
| `+0x20` | `Character*` | the actor; equals `Character::DoAction`'s `this` |
| `+0x28` | ? | zero in every sample |
| `+0x30`, `+0x31` | `u8` | read by `Ability::trigger`; `+0x30` forwarded to `Ability::Prime` |

**The struct is copied member-wise, not memcpy'd.** `+0x04` reads `0x85` in one
copy and stale stack bytes in another. **The wire protocol must skip `+0x04`, and
so must the state hash**, or peers disagree over uninitialized padding.

`Brain::GetChoice` ABI, settled by trace: `rcx = this`, `rdx = &sret`, returned in
`rax` — not the "sret in rcx" rule of thumb, which only holds for free functions.

`TurnAction+0x20` (actor) arrives **null** from the brain; `Character::DoAction`
fills it in. The acting character comes from `TurnControl::GetCurrentActor`.

Brain classes in play: `PlayerBrain` (human), `PatternBrain` (scripted), `NoBrain`
(scenery). Others present: `GenericBrain`, `RandomBrain`, `DicerBrain`,
`MountBrain`, `DbgPlayerBrain`. `GetChoice` is non-virtual and shared by all, so
one hook covers every brain type.

**Queue layout** (read off `update`'s drain loop): `TurnControl+0x48` = ring
buffer, `+0x50` = capacity (mask is `cap-1`), `+0x58` = head, `+0x60` = count. The
front element is copy-assigned into the member slot `TurnControl+0x90`.

### Action types 6 and 7 — internal reactions, never player decisions

**Settled exhaustively. `std::function` is not a protocol problem.**

Type 7 means *"run this deferred lambda at the command boundary."*
`ApplyTurnAction` reads `+0x78` as the callable and calls vtable slot 2
(`_Do_call`) with no arguments — a `std::function<void()>`.

Two creation paths, both enumerated in full: 48 of 54 direct `QueueDecision`
callers build a type-7 inline; `TurnControl::QueueFunctionAction` @ `0x1408DFDD0`
accounts for 17 more. **All ~65 sites are passive/status/reaction callbacks.** Not
one is a brain.

Conclusive because `ApplyTurnAction` has **exactly two call sites**
(`TurnControl::update` @ `0x1408DF2A4`, `NextTurn` @ `0x1408E111E`) and is **not
virtual** (only data xref is `.pdata`). The queue is the sole route in, `update`
contains no imm-7 store, and `GetChoice`'s only type write is `mov ..., 3`.

**Type 6** is a reaction broadcast (walks a character list, fires event `1989`),
produced only by `AbilityReaction::OnAppliedStatuses`,
`CounterAttack::OnAppliedStatuses`, `ChanceToBlockAndCounter::OnReceivedDamage`,
`MonkCatReactionAbilities::CharacterInAuraOnCastAbility`, `sub_1400ABBA0`,
`sub_1400ABDE0`. **It bypasses the queue entirely** (measured: 31 actions applied
against 29 queued, the difference being exactly the 2 type-6s).

Type 7 observed live: 3 `invoke` actions in one capture, all from
`glaiel::MoveWhenDamaged::OnAppliedStatuses` (`0x1402BF0CF`).

**Consequences:** the wire only ever carries types 2 and 3; and **types 6 and 7
are the best cheap desync detector available**, because they exist *because* a
passive fired, so a differing proc roll diverges the queue population before any
visible state changes.

### Ability identity — `Character` ability slots

Heap pointers are unusable as identity: **0 of 21 ability pointers and 0 of 21
actor pointers matched between two runs.**

`glaiel::Character::FindAbility` @ `0x1401183E0` is the resolver. Storage,
confirmed by three independent readings (`FindAbility`,
`Character::GetAllAbilities` @ `0x140118230`, `Character::init` @ `0x1400F6830`):

| Offset | Type | Slot |
|---|---|---|
| `+0xD0` | `Ability*` | `move` |
| `+0xD8` | `Ability*` | `attack` |
| `+0xE0` | `Ability*` | `bonus` |
| `+0xE8` | `u32` | spell vector capacity |
| `+0xEC` | `u32` | spell vector count |
| `+0xF0` | `Ability**` | spell vector data — the `spellN` slots |

Name space: `none`, `move`, `attack`, `bonus`, `weapon`, `trinket`, `spellN`, else
a linear search on the GON name at `(Ability+0x28)+0x88` (a `std::string`: size
`+0x98`, capacity `+0xA0`). `weapon`/`trinket` are **not** separate slots —
flagged abilities inside the same vector (`Ability+0x8A1` / `+0x8A2`, predicates
`0x140043CD0` / `0x140043D30`).

**Slot index is stable by construction.** `Character::init` @ `0x1400FBEC0` walks
the authored GON `spells` array in file order, `SpawnDatabase::CreateAbility` per
entry, append. No sort, no RNG. Stable within a battle too: of 3401
`CustomVector<T*>::push_back` @ `0x140047FC0` sites only 12 touch `+0xE8`, and
exactly one is a `Character`.

**Actor identity is a non-problem.** `Brain::UpdateDecision` has exactly one
caller, `TurnControl::update` @ `0x1408DF2F7`, invoked as
`UpdateDecision(GetCurrentActor()->brain)`. `GetChoice` therefore only ever polls
the current actor's brain.

Wire format: `u8 slot_kind` (0 none, 1 move, 2 attack, 3 bonus, 4 spell) + `u8
slot_index`, plus the GON name as an independent cross-check. **Resolve by slot,
validate by name — a disagreement is itself a desync signal**, and it is the check
that caught the equipment bug.

`Character::GetAllAbilities` enumerates move, attack, spells, bonus — **not** the
slot index order.

---

## RNG and determinism

Real xoshiro256 API taking **state as an explicit parameter**, so independent
streams are possible:

| Address | Role |
|---|---|
| `0x14094B0B0` | `randint(int n, u64 *state)` |
| `0x140158B80` | `randfloat(double m, u64 *state)` — 104 xrefs |
| `0x14094B230` | `rand2(double, u64 *state)` — 91 xrefs |
| `0x14094B600` | random point in disc (`_mm_sqrt_pd`) |
| `0x14094ABB0` | **splitmix64 seeder** |
| `0x14094B550` | **`RollChance(double,double,u64*)` — the proc/crit gate** |

`srand(time(NULL))` is called in `initSystems` but the MSVC LCG constant 214013 is
absent, so CRT `rand()` is vestigial — ignore it.

### `RollChance` — the fourth RNG entry point

**The single most important correction this project made.** Four captures said
"combat takes no RNG". They were wrong: combat rolls go through `RollChance`,
which was unhooked, and it calls `rand2` → `randfloat`, so **one logical proc roll
produced two records whose return addresses both land inside the RNG module**. The
passive that actually rolled sat one frame further up and never appeared.

```c
bool glaiel::RollChance(double p, double scale, u64* state) {
    if (p <= 0.0) return false;
    if (p <  1.0) return p > rand2(-scale, state);
    return true;                    // p >= 1 fires and takes NO draw
}
```

Its **38 call sites are exactly the lockstep-critical proc surface**:
`Character::ReceiveDamage__inner_0` (6 sites), `ChanceToBlockAndCounter`,
`CounterAttack`, `ChanceToBackflip`, `Brittle`, `DelayedFury`, `Confusion`,
`ConfusionEffectOnTaggedAbilities`, `DejaVu`, `Shop::roll_item`, `sub_1403B73C0`.

**The `p >= 1.0` short-circuit takes no draw.** Stream *position* therefore depends
on which procs were **possible**, not merely which fired. Two peers that disagree
about whether a chance was 0.9 or 1.0 desync even when the outcome looks the same.
**This is why `HELLO` must carry a data-identity hash.**

**Detectors for RNG-consuming code:** xrefs to the `2^-53` constant at
`0x141137238`; a byte scan for `ror r64,19` (`48 C1 C8..CF 13`) AND `shl r64,17`
(`48 C1 E0..E7 11`); **and xrefs to `RollChance`** — a scan that only looks for the
three primitive entry points misses all 38 proc sites.

### The TLS block holds several streams — sim vs presentation

The global stream is **not** one address. Measured, then confirmed by a static
scan of all 224 RNG call sites:

| TLS offset | Sites | Draws in one battle | What |
|---|---|---|---|
| `+0x20` | 1 | 0 | `TrailerCatCycler::init` |
| **`+0x178`** | 26 | 44 | **simulation** |
| `+0x198` | 12 | 11739 | **presentation** |
| in a register | 55 | — | offset arrives in a reg; static scan cannot attribute |

`0x198 - 0x178 = 0x20` — adjacent slots, exactly one xoshiro256 state apart.

**The engine already separates them**, which is why this is tractable:

- **`+0x178` = simulation** (426 functions): `Character::ReceiveDamage__inner_0`,
  `TacticsObject::ReceiveDamage`, `Ability::trigger`, every `*AttackAbility` and
  `AOESpellAbility::OnTrigger`, `CounterAttack`, `ChanceToBlockAndCounter`,
  `ChanceToBackflip`, `Confusion`, `TransformInXTurns`, `Shop::roll_item`,
  `WheelOfFate::roll_to`, `CatData::breed`, `CatStats::mutate`,
  `MapScreen::select_boss_level`, `TurnControl::ApplyTurnAction`/`NextTurn`.
- **`+0x198` = presentation** (84 functions): `swf::AVM2Bytecode::RunObjectScript`,
  `swf::DoAction::simpleParse`, `HouseCat::update`, `HousePipe::update`,
  `SwfCutscene::init`, `AnimationTest`, `TrailerCatCycler`,
  `PersistentRandomizedParts::get_frame`, `CatParts::init`.

**The wire protocol only needs `TLS+0x178`, and the per-turn hash covers that
32-byte state, not `+0x198`.**

**Methodological trap:** a scan that reads only the *immediate* moved into `rdx`
reports many sites as "TLS+computed" and finds **zero** `+0x178` draws where there
are several. The offset is often staged through another register (`mov ebx, 178h`
… `add rbx, [rax]` … `mov rdx, rbx`). "Computed" means *unresolved*, never *safe*.
Widen the disassembly window to ~16 instructions and read the immediate that gets
`add`ed to `[gs:58h][0]`.

**There is no per-`Level` or per-`Character` RNG in the battle sim.** Across five
captures: no `stack`, no `IMAGE`, and exactly one `HEAP` stream — `sub_14037F860`,
the level-up option roller (state at `object+0xB8`, reached from
`LevelUpScreen::Reroll`, `WorldEvent::level_up`, `Shop::ObtainItem`). Meta, not
battle. **The lockstep surface really is `TLS+0x178` alone.**

### Battle entry re-seeds the stream from the map node

This is the mechanism behind several results that were previously only measured.

`MapScreen::EnterNode`'s prologue:

```asm
0x1403910C2  mov    eax, 178h
0x1403910C7  mov    rcx, gs:58h
0x1403910D0  mov    rdx, [rcx]
0x1403910D3  movups xmm0, [rdi+118h]      ; rdi = MapNode
0x1403910DA  movups [rax+rdx], xmm0       ; TLS+0x178 <- bytes 0..15
0x1403910DE  movups xmm1, [rdi+128h]
0x1403910E5  movups [rax+rdx+10h], xmm1   ; TLS+0x188 <- bytes 16..31
```

**`MapNode+0x118` is a stored 32-byte xoshiro256 state**, written at generation
time by `MapScreen::generate_map` (`0x14021C558`/`0x14021C563`), and entering the
node copies it wholesale into the **simulation** stream. It happens
**unconditionally, for every node type**, not just the two combat ones.

That explains, in one place: four separate launches entering the same battle at
identical `s0=967e2d6d328620b1`; two live peers whose sim streams had drifted
apart at the menu agreeing on `rng_hash = c2f8ca245330a9cd` by the first turn
boundary; and a client deliberately held 10 seconds behind the host still
producing a byte-identical turn-0 hash.

**Consequences.** The protocol never sends a seed — entering the same node is what
makes the streams equal. `ENTERNODE` carries one word of the seed purely as a
cross-check. And **the two peers do not have to navigate the menus identically**,
nor join at any particular moment. Only the save has to match.

The full chain, which is also why a restarted peer *must* get the save:
`save → chapter_map → MapNode+0x118 → EnterNode → TLS+0x178 → battle RNG`. The
map loader `sub_140227ED0` (sole caller `sub_14039DDF0`) restores each node's
stored state from that blob with the same paired `movups`:

```asm
14022824A  movups xmmword ptr [rdx+118h], xmm0
140228255  movups xmmword ptr [rdx+128h], xmm1
```

Neither `ContinueAdventure` nor the adventure save references `178h` at all. **So
the save does not set the RNG stream; it restores the per-node SEEDS.**

**Caveat, a genuine lockstep hazard:** the *first* thing `EnterNode` does is
`SDL_GetScancodeFromKey(SDLK_LSHIFT)` and, if shift is down, tail-call
`sub_1403923F0` instead of any of the above. Local keyboard state inside a command
boundary. The mod logs a warning rather than diverging silently.

### The fence — remaining targets, and why they no longer need fencing

Of 287 RNG-consuming functions, 102 already use a scratch state and 185 use the
global stream; 8 of those were reachable from a presentation/preview path.

| Address | Status |
|---|---|
| `0x1400420F0` `Ability::ResolveKnockbackDirection` | **CLOSED** — see below |
| `0x14073BD90` `CatParts::init` | cat generation cluster |
| `0x1400BBA30` `CatData::set_class` | cat generation cluster |
| `0x1400B6880`, `0x1400B6A90`, `0x1407AA220` | cat generation cluster (all three verified `mov ebx/edi, 178h`) |
| `0x140736F90` `CatVisuals::reroll_voice` | found empirically in a capture, missed by the static pass |
| `0x1403929C0` `MapScreen::TickNemesis` | meta-only |

**All remaining ones live in the *meta* layer**, so the co-op answer is host
authority rather than scratch streams. `0x14008D180` was mislisted and removed —
it loads `mov edx, 198h`, the presentation stream. Fencing only becomes necessary
if a future design has both peers generating cats independently; the fix would be
to pass a scratch stream, since the API already takes a state pointer.

**Fence target #1 is CLOSED.** `0x1400420F0` is
`Ability::ResolveKnockbackDirection`, not an "AOE tile resolver". It resolves an
ability's knockback direction from a 20-value mode enum at `Ability+0x1A0`, parsed
from the GON key `knockback_mode`. The function is a jump table and **only
`case 19` (`random`) touches RNG** — one `randint(4, TLS+0x178)` @ `0x14004266E`.

Out of 169 uses of `knockback_mode` in all 447 shipped `.gon` files, **exactly one
is `random`**: `MegaGuppy_DropTrash`, a final-boss ability with `target_mode none`
that appears in no ability pool. The player can never select or hover it.
Confirmed from the other direction too: a forward call-graph walk from
`Brain::DrawAbilityAOE` to depth 5 explores 140 functions and finds exactly this
one `TLS+0x178` site.

**Speculation is already fenced (verified).** `Character::receive_damage_passives`
@ `0x14010E600` has signature `(DamageInstance&, CustomVectorInterface<Character*>*,
bool speculative)` and gates its proc roll on that bool: real damage passes false
(`xor r9d,r9d` @ `0x14010F8DD`), `TacticsObject::SpeculativeReceiveDamage` passes
true (`mov r9b,1` @ `0x140112D27`). This is the pattern to copy.
`Brain::GetChoice` has zero TLS loads; `VirtualCharacter::deval` reaches no RNG.

**Lazy cat loading is also already fenced, and the fence is the game's.**
`glaiel::MewSaveFile::Load_0` @ `0x14022F5F0` saves and restores the whole 32-byte
simulation stream around its own body:

```asm
14022F622  mov    rax, gs:58h
14022F62E  movups xmm0, [r14]        ; save TLS+0x178 bytes 0..15
14022F637  movups xmm0, [r14+10h]    ; save bytes 16..31
      ...  sub_1400B6A90 (the CatData constructor, which DOES draw),
           Decompress_LZ4, SerializeCatData ...
14022F715  movups [r14], xmm0        ; restore bytes 0..15
14022F71E  movups [r14+10h], xmm1    ; restore bytes 16..31
```

The same paired `movups` shape as `EnterNode`, in the opposite direction. This
matters because `sub_1400D6980` — `CatData* by_id(registry, u64)`, which the
whole meta layer calls constantly — has a **cache-miss branch**:
`SQLSaveFile::DoesEntryExist("cats", id)` → `operator new(0xC58)` → constructor →
`Load_0`. The constructor reaches the cat-generation cluster
(`CatData::set_class`, `CatStats::mutate`, `sub_1400B6880`), all of which draw on
`TLS+0x178`, and the draws are then thrown away by `SerializeCatData` overwriting
the object from the stream.

Without the fence that would be a real desync source and an ugly one: the *values*
are discarded but the stream *position* would move, so a peer taking the miss
while the other took a cache hit would diverge with no visible state difference.
It is closed — a lazy load leaves the stream byte-identical — so **no detector is
warranted here**; one could only ever report "did not move". Incidentally
`0xC58` = 3160 confirms `sizeof(CatData)` independently.

This is a property of `by_id`, not of any particular caller: a forward walk from
any meta handler that touches a cat will hit it.

**Caveat on all reverse-reachability here:** static call graphs capped at 4–6
levels, and this codebase dispatches heavily through vtables and `std::function`.
Treat every count as a floor, not a proof.

### `TimeDelayStatusApplication` — the one wall-clock dependency

Queued effects advance on turn counts *except* this class, which advances on
wall-clock dt. Both facts were established by reading, not inference.

**Turn-driven, fine:** `DelayedTrigger::OnTurnBegin` @ `0x140809220`,
`TransformInXTurns::OnTurnBeginQueued` @ `0x1408D6140`,
`MoveWhenDamaged::OnAppliedStatuses` @ `0x1402BEA50`.

**Wall-clock driven:** vtable `0x140F229E0` slot 11 @ `0x14024F1C0` is the whole
tick, one statement:

```asm
mov    rax, [rcx+18h]            ; context
movsd  xmm0, qword ptr [rax+10h] ; frame delta-time
mulsd  xmm0, qword ptr [rcx+30h] ; * this->rate
mov    rax, [rcx+28h]
mulsd  xmm0, qword ptr [rax+28h] ; * scene timescale
movsd  xmm1, qword ptr [rcx+0F8h]; the countdown, in SECONDS
subsd  xmm1, xmm0
movsd  qword ptr [rcx+0F8h], xmm1
comisd xmm1, xmm0
jnb    epilogue                  ; >= 0 -> does nothing at all
```

Slot 33 @ `0x14024F0A0` parses the GON key `delay` into `+0xF8`. The `[this+0x18]
->+0x10` double load is the engine's standard delta-time access — 27 sites across
unrelated classes use the identical byte pattern — and the shipped `delay` values
are fractional seconds (`.1`, `.25`, `1.13333`, `3`), which cannot be turn counts.

**Why it is a real desync:** two peers in turn-lockstep run at different frame
rates (the same battle measured at 23,211 and 12,230 frames), so N seconds spans
a different number of *turns* on each peer.

**Scope is tiny** — 4 uses in all 447 `.gon` files:

| ability | delay | payload | reachable? |
|---|---|---|---|
| **`AZ_LoseHead`** (AstroZombie miniboss) | 3 s | `Cleanse 0` + `FullHeal 1` | **yes — ordinary run** |
| `DestroyerThrowShield` (final boss) | 1.13333 s | `FormChange DualSword` | final boss only |
| `MegaGuppy_SlamRight` (final boss) | .25 s | `DoScreenShake` | presentation only |
| `DbgBackgroundTransitionTest` | .1 s | spawn + music | test content |

**Fixed, off by default — `tune::kHookTimeDelay`.** Interception rather than
patching was viable because the not-expired path does nothing at all, both class
functions appear in precisely one vtable slot each, and state fits in the object
(`+0xF8` holds `1e6 + due_turn`, a value the game can never write). `FIRE` writes
`-1.0` before calling the original. **The hook has never run against real
content.** When an AstroZombie is next fought with it on, expect one
`TDELAY converted 3.00000s -> 1 turn(s)` and one `TDELAY firing at turn N`.

---

## The meta layer / run state

Two organizing facts:

> **The *effect* is serializable even where the *input* is not.** That is what
> made cat sync work without replaying a single click.

> **The meta layer is not as non-deterministic as it first looked.** Both peers
> share a save file and `EnterNode` re-seeds `TLS+0x178` from the node, so two
> peers standing on the same node compute the same shop stock, the same level-up
> options, the same event text and the same effect rolls. Where that holds,
> **replicate the CHOICE, not the effect**.

The table at the top is right about the *screens* (the UI is mouse-driven and has
no command boundary in most places). It does not generalise to the *state* those
screens compute.

### `SerializeCatData` is bidirectional — the way in

```c
void glaiel::SerializeCatData(CatData&, ByteStream&, bool)   // 0x14022E9A0, MewSaveFile.cpp
```

Every field branches on the stream mode at `ByteStream+0x00`:

```c
if (mode != 0) write(stream, field, n);   // 1 = write (grow), 2 = to file
else           read (stream, field, n);   // 0 = read
```

So **the same function that saves a cat loads one**. The format carries its own
version tag (`19`) as its first field.

**Polarity trap:** the mode-0 branch calls `sub_1409B3770`, whose assert string is
`"void __cdecl glaiel::ByteStream::read(void *,int)"`. **Mode 0 is READ.** Easy to
get backwards from the decompile alone.

**Cats have real identity.** `CatData+0x00` is a `u64` id, serialized right after
the version tag, and `sub_1400D6980(registry, id) -> CatData*` resolves it. No
index scheme, no pointer that dies between processes. (Note `CatData+0xC48` is a
*second*, distinct id used only as the save-slot key by the write driver.)

**`ByteStream` layout**, from its own asserts:

| offset | field |
|---|---|
| `+0x00` | `u32` mode — 0 read, 1 write, 2 file |
| `+0x08`/`+0x0C`/`+0x10`/`+0x2C` | write cap / len / buffer / pos |
| `+0x18`/`+0x24`/`+0x28` | read buffer / len / pos |
| `+0x20` | `u8` **owns the read buffer** |
| `+0x30` | an embedded `std::ofstream` |
| `+0x140` | `u32` max byte-swap element size |

Two things are load-bearing. `+0x20` is the destructor's first instruction
(`cmp byte ptr [rcx+20h], 0`) — leaving it **0** is what lets us lend the game a
buffer from the mod's `/MT` CRT heap without it calling *its* `free` on our memory.
And `+0x30` means a hand-zeroed stream **cannot** be destroyed: the dtor
dereferences a vtable that is null on a zeroed block. Construct it with
`sub_14032D0A0` first.

### The run inventory — serializer located, both directions

`Inventory` is a `Component`-vtable **singleton** (`qword_1413BE2A0`, guarded by a
"More than one inventory" assert), also cached at `MewDirector+1416`.

```
write:  sub_1402E10D0 -> sub_1402E15B0 -> sub_14022CBD0 (mode 1)   <- sub_1403B9CE0
read:   sub_1402E1370 -> sub_1402E1740 -> sub_14022CBD0 (mode 0)   <- ContinueAdventure
```

`sub_1402E10D0` writes, by GON key (all six verified present as literals at
`0x140f01608`+ and referenced from both drivers):

| what | where |
|---|---|
| `adventure_coins` (gold) | `Inventory+328` `int` |
| `adventure_food` | `Inventory+332` `int` |
| `adventure_furniture_boxes` | `Inventory+336` `int` |
| `inventory_backpack` | `Inventory+56` |
| `inventory_storage` | `Inventory+120` |
| `inventory_trash` | `Inventory+184` |

A 4th bucket at `Inventory+248` is allocated identically by `Inventory::init` @
`0x1403C8310` but never saved — almost certainly "currently equipped", redundant
with what `SerializeCatData` already writes onto the cat.

Buckets are intrusive doubly-linked lists (payload at node+24), walked in order.
`sub_14022CBD0` is the per-`Equipment` field serializer, **bidirectional**, mode-
branched exactly like `SerializeCatData`, version-gated to 5: GON name string at
`Equipment+8`, a state byte, a version-gated blob at `+40`, ints at `+72`/`+76`/
`+80`, bytes at `+84`/`+88`/`+92`.

**Item identity does not round-trip, and this is the design hazard.** On load each
`Equipment` gets `++qword_1413BD998` — a **process-global object-id counter** that
15 other classes also mint from and that is never read from or written to the
stream. An item's only persistent identity is its GON name plus its stat fields.
**Host and client mint different ids independently, so no wire protocol may key on
them.** Push the whole `Inventory` wholesale.

`glaiel::Inventory::insert_item` @ `0x1402E0920` (36 callers) is the insertion
point; case 3 of its switch resolves "which cat currently has this equipped", i.e.
it is also the unequip path.

### `MewDirector` layout

The `MewDirector*` variable is `qword_1413D1970`. Offsets are read off
`sub_1403B9CE0` (`save_adventure`, 11 callers including `MewDirector::ReturnToMap`).

| Offset | Field |
|---|---|
| `+56` | embedded `MewSaveFile` — a read/write-through cache in front of the sqlite `SQLSaveFile` |
| `+1416` | `Inventory*` |
| `+1424` | **`RunHistory*`** — a POINTER, dereferenced. See below |
| `+1432` | cat registry |
| `+1448` | `Pedigree`/`GlobalProgressionData`-ish |
| `+1456` | GON key `tutorial_tokens` |
| `+1464`/`+1468`/`+1472` | `{cap, count, data}` of the run's cat ids |
| `+1496`/`+1500`/`+1504` | a second `{cap,count,data}` over 24-byte structs — unidentified |
| `+1552` | **pending next-fight unit spawns** — a `std::vector` of 56-byte entries, written by `spawn_unit_next_fight` |
| `+1576` | familiar list, written by `Shop::ObtainItem` type 13 |
| `+1600`…`+1752` | version-gated scalar/blob tail, not individually decoded |
| `+1640` | the weather-name list appended by `add_weather`, inside that tail |
| `+1812` | touched by the event commit path |

Related: `qword_1413D16E0` is a *separate* object the shop writes to at `+176`/
`+180`/`+184` under a flag — not identified, plausibly a hub-mode inventory.

**`MewDirector::save_adventure` is write-through to the `.sav`.**
`MewSaveFile::Store` @ `0x14022C4D0` reaches
`INSERT OR REPLACE INTO <table> VALUES (:key, :data);` via `SQLSaveFile::SQL` @
`0x140A02BE0`. Keys written: `chapter_map`, `adventure_state`,
`trollengine_state`, `tutorial_tokens`, `on_adventure`, `savescumlocation`.

**But it is a NODE-BOUNDARY function.** The game calls it only from
`ReturnToMap`; every state it has ever written is one where the player stands on
the map with nothing in progress. Calling it mid-node persists *"inside node N,
unresolved"* — reload that and the run comes up standing at a node it can neither
enter (the battle is over as far as the map is concerned) nor pass (the node was
never completed). The adventure is intact; it is **wedged**. Guard on being
between nodes *and* on an adventure being loaded — a non-null `MewDirector` proves
neither, because it is a singleton that exists from startup, so a
`director != nullptr` guard never fires and will flush an *empty* director over a
save slot.

### The run history at `*(MewDirector+1424)` — the event roll is not pure

**This corrects the assumption the whole "replicate the choice, not the effect"
design rested on.**

`MapScreen::select_event` @ `0x140395D10` delegates to `sub_1408DA560`, which
reads three things before it touches the stream:

1. **A RANDOM CAT out of the run's roster.** `sub_1400AACD0` is one inline
   xoshiro step used to *index the list*, so the same stream state picks a
   different cat when the list differs in order or length. That cat is the
   event's subject.
2. **That cat's passives.** `ExcludeFromEvents` filters the pool;
   `ChanceToForceEvent` can force a specific event outright, on a `rand2` gated
   by the cat's own stat. An event can be decided by a cat rather than by a roll.
3. **The USED-EVENT LIST.** The pool draw retries until it finds one not already
   used — `sub_1408D9EB0` tests it, `sub_1408D9E50` appends to it.

So two peers with identical streams and identical cats still roll **different
events** the moment their used-event lists differ by one entry.

**`MewDirector+1424` is that list.** It is a run-history object: three string
lists (the used-event one at `+96`), two 19-int per-node-type counter arrays
bumped on every return to the map, and a scalar tail. It rides in the save —
which is why rejoining always appeared to fix a drifted run, and why a session
that never rejoined drifted further and further apart.

`sub_1408DD2F0` serializes it **bidirectionally**, mode-branched on
`ByteStream+0x00`, with its own version tag — the `SerializeCatData` shape. Both
game call sites pass the pointer **dereferenced** (`mov rcx, [rsi+590h]`), not
the address of the field. **Its 16-byte prologue occurs EXACTLY ONCE in the
shipped image**, checked against `Mewgenics.exe` itself rather than against IDA.

### What the meta screens actually write

Two comfortable assumptions were tested and both failed. Do not re-assume them.

**The shop.** `glaiel::Shop::ObtainItem` @ `0x140797DC0` switches on the item type
(constants cross-read from `Shop::roll_item` @ `0x1407942D0`), but **half the
switch is house-only**: `Shop+123` is an "is this the house shop" flag, and the
game's own assert text proves it — type 18 calls the fatal reporter with **"error:
bought blank collar while on an adventure somehow"** when `Shop+123 == 0`.

| type | GON string | target | reachable in an adventure? |
|---|---|---|---|
| 1 | `Item` | `Inventory::insert_item` | yes |
| 12 | `LevelUp` | `CatData` | yes |
| 13 | `Familiar` | `MewDirector+1576` | **no authored content, anywhere** |
| 15 | `Furniture` | house | no — house-gated, out of scope |
| 16 | `Food` | `Inventory+332`, *or* `qword_1413D16E0+176` when `Shop+123` | **no authored content** |
| 17 | `Coins` | `Inventory+328`, *or* `qword_1413D16E0+180` when `Shop+123` | **no authored content** |
| 18 | `BlankCollar` | `*(MewDirector+1448)+1536` | no — fatal-asserts mid-adventure |

**Zero of the 447 shipped `.gon` files stock a `Familiar`, `Coins` or
`BlankCollar` in an adventure shop.** So an adventure shop writes `Inventory` and
`CatData` and nothing else — both already pushed per node. **The shop needs no
work at all.**

**Level-up — same verdict.** `LevelUpScreen::select_option` is `sub_140382A00`,
and `LevelUpScreen+160` is the subject `CatData*`. All seven option types land on
that `CatData` (the `CatStats` block at `+1804`…`+1828`) or on
`Inventory::insert_item`. Its `HEAP` option roller is seeded from
`splitmix64(CatData+0x00)` then `jump()` applied `CatData+0xC30` times
(@ `0x140379CA5`) — both fields round-tripped by `SerializeCatData` (the id right
after the version tag, `+3120` at line 632).

**`Pedigree` (`+1448`) is written mid-adventure only by the `BlankCollar`
purchase — which cannot happen mid-adventure.** `+1448` stays out.

### World events

Fully **data-driven**: `data/events/*.gon` (20+ files) authored as
intro / main / options / stat-check / good-bad-reward / `random_pool`.

`glaiel::WorldEvent::spin(this, GonObject* effect_list)` @ `0x140913320` is a
~1850-line **command interpreter** that string-dispatches ~45 authored effect
commands (`gain_coins`, `learn_ability`, `permanent_stats`, `get_item`,
`get_and_equip_item`, `spawn_unit_next_fight`, `self_status_next_fight`, …).
`WorldEvent::setupResult` @ `0x1409185D0` reads the outcome class from
`WorldEvent+248` and calls `spin` on the option's good/bad/crit sub-tree.

**The command boundary** is `sub_140937F30` — three statements, one code caller
(`setupActionChoice__inner_1` @ `0x140917960`), not virtual, and unlike
`InventoryItemBox::click` it **takes the choice as an argument**. The argument is
a `GonObject*`, a per-process heap pointer, so it is resolved by authored option
*index* instead.

**The client DOES open its own `WorldEvent` screen.** It is driven into the same
node, so it runs the same `WorldEvent`, sees the same options, and can click any
of them. The whole reason `CHOICE` exists is that the client reaches this screen
and must be stopped from pressing anything.

**`self_status_next_fight` writes a cat, and cats are synced.** `WorldEvent+296`
is the subject `CatData*`, so the target is `CatData+1976`, which
`SerializeCatData` already round-trips.

`WorldEvent+0x1A10` holds the chosen event's name: `select_event`'s result is
stored there, it is tested against the literal `"random"`, and it is used as a GON
key (`this` is `r15`, pinned by `mov r15, rcx` at `0x14090FA30`). Confirmed live —
`NODEHASH event on node <seed> is '<name>'` appears in both peers' logs.

### Choice replication

**What crosses the wire is which button the host pressed. Nothing else.** No
effect, no run-state field, no serializer.

The claim it rests on is four determinism results, all checked rather than assumed:

1. **The options are the same** — same save, same node, and `EnterNode` re-seeds
   unconditionally for every node type.
2. **The effect rolls are the same** — every RNG site inside `WorldEvent::spin`
   and the shop's item pool loads `178h`; the item-pool draw at `0x1408DB3FE` is
   `mov r8d, 178h; mov rax, gs:58h; add r8, [rax]`.
3. **The level-up roller is a pure function of synced state** (above).
4. **The effects land on already-synced objects** (the shop table and
   `self_status_next_fight` above).

**Two boundaries, four hooks.** Both option arrays are `{begin, end}` pointer
pairs with a **240-byte stride** — `WorldEvent+224/+232` and
`LevelUpScreen+864/+872` — so an index is all the identity needed.

| boundary | host | client |
|---|---|---|
| `sub_140937F30`, the event commit | index = `(entry - begin)/240`, publish, click through | **swallow the click** |
| `LevelUpScreen::select_option` @ `0x140382A00` | match by type + name, publish, click through | **swallow the click** |
| `WorldEvent::update` @ `0x1409122C0` | — | cache the screen, apply a held choice |
| `LevelUpScreen::update` @ `0x140382640` | — | cache the screen, apply a held choice |

The client applies by **re-entering the game's own commit path** with the index
substituted: a 24-byte synthesised capture for the event
(`{nullptr, WorldEvent*, begin + 240*index}`), and `sub_140386810`'s
`{vt, screen, i32 index}` for the level-up. So `MewDirector+1812`,
`sub_14091AA00` and the `sub_14037BBB0` copy-construct all still happen exactly
as they would for a local click.

**Three failure modes, deliberately different loudnesses:** a **count** mismatch
refuses (the index means nothing against a different option list); a **name or
type** mismatch warns and proceeds (the host really did press that button, and
stalling the run over a label is worse); and a client whose call targets fail
their prologue check **disarms the whole feature** rather than swallowing clicks
it cannot replace — an armed client that cannot inject is a permanently dead
screen, which is worse than a desync because nothing times out.

Proved live: real decisions of both kinds published every session, up to 9 per
run, with no count-mismatch refusals and no disarms.

#### Every effect command that rolls, and which stream it uses

**`WorldEvent::spin` has ZERO TLS loads in its own body** — 9147 bytes, 120 call
targets, and not one `gs:58h`. Every command is a single call to a handler, so
ten small functions answer the whole question. A forward call-graph walk is not
needed and actively misleads (see `weather_roll`).

| command | live `.gon` uses | rolls? | stream | handler |
|---|---|---|---|---|
| `weather_roll` | 1 | yes, weighted pool ×2 | **`TLS+0x178`** | `0x140931CF0` |
| `add_weather` | 67 | **no** | — | `0x140924220` |
| `next_fight_from_set` | **0** | no | — | `0x14092DDA0` |
| `next_event_from_set` | 7 | **no** | — | `0x14092E030` |
| `scramble_abilities` | 1 | yes | **`TLS+0x178`** → `sub_14094C280` | `0x140921D20` |
| `scramble_passives` | **0** | yes | **`TLS+0x178`** → `sub_14094C280` | `0x1409228C0` |
| `scramble_basic_attack` | 1 | yes | **`TLS+0x178`** → `sub_14094BBB0` | `0x140923060` |
| `make_old` | 3 | **no** | — | `0x140930090` |
| `upgrade_ability` | 3, all `random` | yes | **`TLS+0x178`** → `sub_1400AB2C0` | `0x140920F50` |
| `upgrade_passive` | 2, all `random` | yes | **`TLS+0x178`** → `sub_1400AB2C0` | `0x1409216A0` |

Every one that rolls loads `376 = 0x178`, the **simulation** stream that
`EnterNode` re-seeds identically on both peers. Not one reaches `+0x198`, a
scratch state or a heap stream.

Two are closed by content alone: `next_fight_from_set` and `scramble_passives`
appear only in `data/event_rewards_samples.gon`, and one of the two
`next_fight_from_set` arguments is literally `putalevelhere.lvl`.

**`weather_roll` is why a call-graph walk was the wrong instrument.** Its handler
is 22 lines: load the stream, draw twice from a weighted pool through
`sub_14094BFD0` (which takes the state as its 4th argument), then call
**`spin` again** on the chosen weather's effect subtree. That recursion inflates
a depth-6 walk from its handler to 849 functions whose RNG sites all belong to
*other* commands. Reading the body took one decompile.

**`add_weather` takes no draw but does write run state** — it appends the weather
name to the list at `*(MewDirector+1640)`, inside the version-gated tail that
`save_adventure` round-trips. Both peers run `spin` on the same choice, so both
append the same entry.

### Two designs that were ruled out — do not retry

**The whole-run blob.** `glaiel::MewDirector::ContinueAdventure` @ `0x1403BB120`
is the exact read counterpart of `save_adventure` (cats via `sub_1400D8270`,
Inventory via `sub_1402E1370`, `+1448` via `sub_1401D2030`, plus the whole
`+1468`…`+1752` tail). **It cannot be applied to a live run:**
`Director::DestroyScene("House")` is its first statement, unconditional; and
`sub_1400D8270` **frees the entire live cat registry** — walks the intrusive list
and destructs every payload — also before any repopulation. Every
`CatData*`/`Character*` held by a battle, a UI screen or the mod goes dangling.
It has exactly one caller and was built to run once, at adventure start. Getting
bytes in and out was never the gap; **live-apply is the gap, and it is closed
off.**

**The inventory's own boundary.** `glaiel::InventoryItemBox::click()` @
`0x14034DF60` passes every structural test (one code caller, only data xref is
`.pdata`) and is still unusable: it takes **no arguments** — what was clicked
lives in `this`, a UI box that exists only while the inventory screen is open —
and it only *starts* a flow (confirmation popup → `AbilityChooser` → mutations
fanning out over ~8 `CatData` helpers, the bottom of which, `sub_1400B3920`, has
40 call sites). Shipping the serialized result is strictly better.
Related: `InventoryScreen2+0x20` is a `CustomVector<InventoryItemBox*>` in
creation order; an item's authored GON name sits at `item+0x08`.

**And there is no `RUNSTATE` message.** It was planned as "the director-level
fields nothing covers"; those fields turned out to be covered by determinism plus
`CHOICE`, and the one piece determinism did not cover turned out to be a
serializable object of the game's own — the run history.

### The house — serializable, but NOT choice-replicable

Out of scope by decision, not by capability, and it is worth recording which is
which so the question does not get re-asked from scratch.

**`house_state` is a save key** (`0x140ECF3C8`) with a read/write pair in exactly
the `save_adventure` / `ContinueAdventure` shape:

| | address | size | shape |
|---|---|---|---|
| **write** | `sub_1401E5DF0` | 1349 B | constructs a `ByteStream` (the `ofstream` ctor at `+0x30`), mode-branched fields, ends in `MewSaveFile::Store` → sqlite. Caller `sub_1403BCE20` |
| **read** | `sub_1401E6340` | 1528 B | same shape, no `Store`. Caller `House::init__inner_0` |

So getting house state in and out is already solved by the game.

**What the house does NOT have is a re-seed, and that is the decisive fact.**
"Replicate the CHOICE, not the effect" is valid in the adventure *only* because
`EnterNode` re-seeds `TLS+0x178` from `MapNode+0x118` at every node. Measured by
TLS access (`gs:58h`), which does not depend on the offset immediate being
visible — the documented staging trap:

```
house_state WRITE sub_1401E5DF0 : 0 TLS site(s)
house_state READ  sub_1401E6340 : 0 TLS site(s)   <- the blob carries NO seed
House::init__inner_0            : 1 site, and it is +0x198 (presentation)
MapScreen::EnterNode (control)  : 2 sites          <- the re-seed
```

And the house genuinely draws on the **simulation** stream: `CatData::breed` and
`HouseInventory::AddRandomFurniturePiece` both load `178h`, and `sub_1401E8120` —
the 23.7 KB day-cycle driver behind `House::EndDay` — loads **both** streams. So
two peers in a house drift on the first roll and **nothing ever resynchronises
them.** In the adventure every node entry wipes drift; in the house it would be
permanent.

**Verdict: host-authoritative state sync, the same mechanism as the meta layer,
and never choice replication.** Two things to weigh before anyone starts:

- **In favour:** the house is not real-time. Its command boundary is the day
  cycle (`House::EndDay`, 33 bytes, exactly one caller — `sub_1401E8120`), and the
  game already destroys and rebuilds the House scene on entry, so "apply by
  rebuilding" is something it does normally. That is the *opposite* of
  `ContinueAdventure`.
- **Against:** the read path is called from `House::init` — built to run once at
  construction, so live-apply is unproven, and `HouseCat` objects would hold
  pointers across it. Same class of blocker as `ContinueAdventure`; **not yet
  established as fatal, and not yet established as safe either.**

### The save file — how a client acquires the run

The host publishes at the slot click; the client's save-selection screen is
driven for it and the load redirected to `mgmp_coop.sav`, so the client's own
saves are never written.

`SaveSelection+0x38` is a `std::vector<std::string>` of slot filenames — proven
statically off the transition lambda, then confirmed live (both peers printed
`slot 0/1/2 = steamcampaign01/02/03.sav`). Redirecting via
`MewDirector::init(std::string)` is safe because the parameter is **by value**
and the game itself passes `&names[slot]`, an element of a vector whose scene is
marked for destruction two instructions earlier — if `init` retained it, it would
dangle in the shipped game.

**Save sharing has no switch and is not a mode.** The save is how a client
acquires the run, and with it the per-node seeds that make both peers roll the
same battle; "off" only ever meant "this session cannot reach a shared battle".

### Where the run is standing — `MapScreen+0xA0` is the map MARKER

`glaiel::MapNode::Click` @ `0x140227C90` **selects** a node, it does not enter
one. It sets the current node with

```c
*(*(MapNode+0x170 /* its MapScreen */) + 0xA0) + 0x60) = node;
```

and guards its own entry on reading the same slot back — two readings, which is
what makes it trustworthy. `sub_14038DE60` calls `MapMarker::CanReachNode` on
`MapScreen+0xA0` and derives a facing from `sub_140223E60(...)+92`.

| offset | slot |
|---|---|
| `MapScreen+0xA0` | the map marker |
| marker `+0x50` | the node the marker **is on** — where the cats stand |
| marker `+0x60` | the node `Click` **selected** |

Both hold pointers directly comparable to the elements of `MapScreen+0x80`.

**Both are validated, never trusted** — the pointer is run through `index_of_node`
first, so a drifted offset reports "unknown" instead of a confident wrong number.

**Read the game's state, not the mod's memory of it.** What the module watched
someone enter during a session is nothing at all after a reload — and a run
reloaded onto a wedged node is exactly when you need to know which node that is.
The marker slots survive a reload because they are the game's state.

### `MapNodeType`, from `MapNode::str_to_type` @ `0x140224C10`

| | | | |
|---|---|---|---|
| 0 `none` | 1 *(3-char)* | 2 `enter` | 3 `exit` |
| 4 `home` | **5 `battle`** | **6 `hard`** | 7 `miniboss` |
| 8 `boss` | 9 `event` | 10 `optional_event` | 11 `special_event` |
| 12 `shop` | 13 `treasure` | 14 `furniturebox` | 15 `foodbox` |
| 16 `bonus` | 17 `empty` | 18 *(3-char)* | |

Note 7/8 are **not** in string order: `boss` returns 8, `miniboss` returns 7.
Stored at `MapNode+0x138`; `EnterNode`'s `sub eax,5 / cmp eax,1` selects the two
ordinary combat types.

Node identity is the index into `MapScreen+0x78`, the standard
`CustomVector<MapNode*>` = `{u32 cap@0x78, u32 count@0x7C, MapNode** data@0x80}`.

---

## Rendering — how to draw something

**First decide WHICH SURFACE, because there are two and they are not
interchangeable.**

| you want | use | why |
|---|---|---|
| something anchored to a TILE or a world position | the game's immediate-mode UI | it is camera-correct, depth-sorted into the scene, and costs no GL |
| something at a SCREEN position, on top of everything | our own GL overlay | the board path snaps to tiles, sorts behind scenery, and is authored at board scale |
| a DEBUG control the person at this keyboard clicks | the ImGui panel | it already owns a context, input and a font |

Picking the wrong one fails in three different ways that each look like a
separate bug.

**Do NOT build components.** The obvious route is to clone what `StatusMenu::init`
@ `0x140816F60` does for the local cursor (two `RendererIso` components from the
animations `GroundMouseCursorPip` and `MouseCursorPip3D`). That means allocating
through the component system, parenting into a scene, and owning a lifetime that
must survive a battle ending, a peer disconnecting and a scene teardown we do not
control.

### The immediate-mode UI

`Brain::UpdateDecision` draws the local target cursor with one call (id `target`,
anim `TargetCursor`, layer 6, frame -1); `Brain::DrawAbilityAOE` @ `0x14013A030`
draws whole tile sets the same way (`AreaIndicator`, `KnockbackArrow`,
`PathIndicator`).

**Note which argument is which**: `rdx` carries the immediate-mode IDENTITY,
spelled lowercase, and `r9` carries the ANIMATION, the CamelCase symbol exported
from `swfs/ui.swf`. Reading this backwards throws a fatal `std::string` from
`0x14094DC50` (via `Renderer::init` @ `0x1409724D0` failing to resolve the
MovieClip) — an MSVC throw code, not an access violation, unwinding to
`std::terminate` **with no `mgmp.dll` frame on the stack**, because the lookup
happens in the UI consumer pass long after our submit returned.

| Address | What |
|---|---|
| `0x14013C570` | `Component* -> ImmediateModeGameUI*`. Reads `Component+0x18` (scene), walks `scene->+32->+18080`, falls back to the parent scene. **Returns null off the battle screen.** |
| `0x14033FFD0` | `tile_piece(...)` — draw a named sprite on a TILE |
| `0x14033FA70` | the general form; takes a `Vec3D` position instead of a tile |
| `0x140340EC0` | id-string -> instance counter (the identity map) |
| `0x140341060` | push the built `UIPiece` |

`sub_14033FFD0` ABI, read off the call sites (Hex-Rays mis-assigns these — trust
the disassembly):

```
rcx   ImmediateModeGameUI*
rdx   std::string* id      <- the IDENTITY. Same id next frame = same piece, so
                              it persists and animates; stop submitting and it
                              disappears by itself. Nothing to free, ever.
r8d   int layer            (6 for the target cursor, 4/7 for AOE)
r9    std::string* anim    <- a SWF animation name
[5]   iVec2D tile          BY VALUE, packed {i32 x, i32 y}; callee adds 0.5 to
                           each to centre on the square
[6]   float rgba[4]        by pointer
[7]   int frame            -1 = no particular frame
[8]   double scale[3]      by pointer, {1,1,1} = native
```

**Its two pointer arguments are read with `MOVAPS`, so they must be 16-byte
aligned:**

```asm
0x14034007F  mov    rcx, [rsp+arg_28]      ; the rgba float[4]
0x140340087  movaps xmm0, xmmword ptr [rcx]
0x14034005E  mov    rcx, [rsp+arg_38]      ; the scale double[3]
0x140340066  movaps xmm0, xmmword ptr [rcx]
```

A bare `float[4]` gets 4-byte alignment and a `double[3]` 8-byte, so passing plain
locals is a **coin flip decided by the rest of the stack frame** — adding one call
site reshuffles the frame and both peers take an AV at `0x140340066` on the first
battle frame. The game's own callers never hit this because their colours and
scales are members of aligned objects.

**BOTH std::strings are CONSUMED** — `_Tidy_deallocate` runs on each before
return. Keep every name ≤15 chars so that is a no-op on a small-string image.

**Alpha is a plain parameter.** The consumer premultiplies rgb by `a` and writes
`a` into `Renderer+0x60` — a double the base ctor `sub_14005A580` sets to `1.0`.
So element 3 of the colour IS the alpha: no shader, no blend state, no second
draw. Writing `Renderer+0x60` directly is also how you fade a renderer the game
owns.

`sizeof(UIPiece) == 248`: seq `+0`, id string `+8`, anim string `+0x30`, pos
Vec3D `+0x50`, scale Vec3D `+0x68`, **rgba `+0x80`**, layer/kind `+0x98`, frame
`+0xA8`, tile-space flag `+0xAC`, `std::function` `+0xB0`.

**SWF clips come from two different coordinate worlds, and the SWF says which.**
Reading the shipped bounds out of `swfs/ui.swf` answers "why is nothing visible"
without running the game at all:

| clip | bounds | authored for |
|---|---|---|
| `TargetCursor` | 140 x 70 px, centred on the origin | one BOARD tile |
| `MewCursor` frame 0 | 12 x 22 px, anchored top-left | SCREEN pixels |

`MewCursor` drew correctly the entire time it appeared broken — as a speck in the
middle of a 140-pixel tile. **Measure the asset before blaming the call site.**

### Drawing in SCREEN space — the GL overlay

**There is no `SDL_Renderer` in this process.** `ApplicationBase::RefreshWindow`
@ `0x1409A9110` asks for GL `3.2 CORE` and presents with `SDL_GL_SwapWindow`;
nothing calls `SDL_CreateRenderer`. So the drawing API is **raw GL 3.2 core** — no
fixed-function pipeline, no `glBegin`, no matrix stack.

- **Interception is a jump-table WRITE, not a hook** (see the SDL section).
  The previous value is the trampoline.
- **Draw BEFORE the previous implementation.** The swap presents the back buffer,
  so drawing immediately before it is drawing last.
- **GL entry points** come from `opengl32.dll` (1.1) and `wglGetProcAddress`,
  resolved on the first swap because that is the first moment a context is current.
- **Save and restore everything**: program, VAO, array buffer, draw framebuffer,
  viewport, blend/depth/cull/scissor. **Bind framebuffer 0 explicitly** — the game
  may have left an offscreen target bound, and drawing into that is invisible with
  nothing to report it.
- **Rebuild on context loss.** GL names are per-context and `RefreshWindow`
  recreates the context on a resolution change; `glIsProgram` is the only
  notification available, so it is asked every frame.
- **Bind attribute locations explicitly** (`glBindAttribLocation` before the link)
  rather than assuming them from declaration order.

**The engine's cached mouse is two doubles at `0x1412F2E80`** (RVA `0x012F2E80`),
logical window coordinates, filled by the input tick `sub_14097E3D0` from
`SDL_GetMouseState`. Reading the cache rather than calling SDL again is reading
exactly the position the game is acting on — the input tick deliberately reuses
the stored value on frames where it does not ask.

#### `glTexImage2D` obeys pixel-store state that is not yours

**A general rule, not a cursor rule: we upload into someone else's GL state, and
the upload path fails SILENTLY when that state is dirty.** Two ways it goes wrong
and neither sets an error: a buffer bound to `GL_PIXEL_UNPACK_BUFFER` turns the
pixel pointer into an **offset into that buffer**; a stale `UNPACK_ROW_LENGTH` /
`ALIGNMENT` / `SKIP_*` shears or shifts the image.

Measured on this build: `pbo 0, store dirty`. The resulting texture sampled to
zero alpha — a quad drawn perfectly, containing nothing — while `glGetError` was
clean around `glTexImage2D`, a texture name was allocated, and alpha 255 was
verified in the decoded PNG buffer.

"The buffer I handed over has pixels" and "the texture has pixels" are different
claims, and the bug lives exactly in the gap. Neutralise all eight `UNPACK_*`
switches plus any bound PBO, restore them after, and **read the level back with
`glGetTexImage`** to compare the hotspot alpha against the decode. **A readback
that disagrees is the diagnosis, not a hint. Do this for any texture the mod ever
uploads.**

#### How big is the window — THREE rectangles, and they are all different

| rectangle | units | origin | how to get it |
|---|---|---|---|
| **window, logical** | SDL logical units | top-left | `SDL_GetWindowSize` |
| **drawable** | framebuffer pixels | top-left | `SDL_GetWindowSizeInPixels` |
| **content** | framebuffer pixels | **centred in the drawable** | size from `glGetIntegerv(GL_VIEWPORT)`, origin derived |

**The mouse is in the FIRST and the drawing is in the THIRD.** Every positional
bug in this feature was some pair of those three being mixed.

**The content rectangle is the one that matters**, because the game letterboxes:
resize the window away from its aspect and it renders a fixed-aspect image into a
centred sub-rectangle with black bars outside. That rectangle — not the OS window
— is what two peers have in common, so the pointer fraction must be measured
against it at **both** ends.

**The viewport tells the truth about its SIZE and lies about its ORIGIN.**
Measured on a 958x1120 window, the game reports viewport `0,0 958x539`: the right
size, but an origin of `(0,0)`, which in GL's bottom-left convention is the
*bottom* of the window rather than the centre. The game is almost certainly
rendering the scene to an offscreen target of that size and compositing it centred
afterwards, so the viewport observed at swap time belongs to the offscreen pass.
**Take the size from the viewport and derive the origin by centring it in the
drawable.**

**Never normalise against the largest mouse coordinate ever seen.** It can only
ever GROW the divisor, and **a stale maximum survives a resize** — after shrinking
a 1280-wide window to 958, a leftover max of 1240 scaled every x by `958/1240`. A
stale measurement is worse than no measurement.

The pointer's ink height is quoted at a reference content height and scaled by
`content_h / ref_h`; the content is always the same aspect, so its height is the
whole scale factor. Each receiver scales by **its own** content rect, so two
players on different resolutions each see a correctly-sized arrow.

### The game's own cursor is a TEXTURE, not a clip

`glaiel::SetCursor(std::string state, int priority)` @ `0x1409B09B0` keeps the
highest-priority claim on the `Cursor` singleton (`qword_141416590`); the Cursor's
late-update @ `0x1409B0900` copies the winner to **`ApplicationBase+3392`** (a
`std::string`) and resets the priority so the next frame re-elects from scratch.
`+3424` is whether it is shown. `Cursor::init` @ `0x140757B70` is engine-level and
asserts "MADE 2 CUSTOM CURSORS" if two exist.

The states are the file names in **`textures/cursor/`**: `default`, `attack`,
`spell`, `move`, `invalid`, `examine`, `question`, `grab`, `grabr`, `heal`,
`btn_over`, `pet_frame1..4`, plus `_hastargets` variants.

- 19 files, **128x128 with heavy transparent padding**; `default`'s ink is
  (29,2)-(107,107). **Scale by the INK box, not the image**, or the cursor comes
  out a third of the requested size.
- `hotspots.gon` lists only the **exceptions**: `default [34 7]`, `grab [17 58]`,
  `grabr [110 58]`, `pet_frame1..4 [32 52]`. Everything else shares default's, and
  the pixels agree — `attack`, `spell`, `move`, `invalid`, `examine`, `question`,
  `heal` all start their ink at exactly (29,2), i.e. the same arrow with a
  different badge.
- All 20 are unpacked to `tools/cursor_assets/`.
- **Tint is a plain multiply.** The art is white with a black outline, so
  `t.rgb * tint` colours the fill and leaves the outline black. No second pass.
- `src/core/mgmp_gpak.cpp` reads them at runtime. The index walk **cannot `break`
  early** — the data section starts after the WHOLE index, so the base is not
  known until the walk finishes.

`MewCursor` in `swfs/ui.swf` is a *clip* with six frames labelled `default`,
`attack`, `spell`, `move`, `invalid`, `btn_over` — the same idea in the other
representation, useless for drawing for the coordinate-world reason above.
**Frame numbers are 0-based and clamped**, read off `sub_14099EC40`:
`v5 = frame_count - 1; if (frame <= v5) v5 = frame`.

### `StatusMenu` — the battle HUD, and the only hook a cursor needs

`glaiel::StatusMenu::update` @ `0x140817320`, vtable slot 11 of `0x141118CD8`.
14849 bytes, virtual-only (one `.rdata` xref, rest `.pdata`), so not ICF-folded
and safe to splice. `sizeof(StatusMenu) == 0x108`.

| Offset | Field |
|---|---|
| `+72` | the tactics grid — **width `+184`, height `+188`** |
| `+80` | `RendererIso*`, `GroundMouseCursorPip` |
| `+88` | `RendererIso*`, `MouseCursorPip3D` |
| `+124` | **iVec2D, the board tile under the mouse — UNALIGNED** |

**`+124` is the whole reason the mouse→isometric projection never has to be
reimplemented.** `update` computes it, bounds-checks it against the grid, hides
both pips when it falls outside, and caches it.

On a `Renderer`: `+80` enabled, `+81` visible (update writes this every frame — do
not fight it), `+84` animation state, `+96` alpha, `+128` must be non-null to
render.

---

## SDL3 is statically linked WITH ITS DYNAMIC-API SHIM

**This invalidates the obvious way to both hook and call any SDL function.** Every
`SDL_*` name in the binary is a thunk through a table in `.data`:

```
SDL_GL_SwapWindow  @ 0x140B9B7D0   jmp cs:off_1412DE650
off_1412DE650   statically ->      sub_140B92850, the DEFAULT stub:
                                     SDL_InitDynamicAPI();
                                     return off_1412DE650();   // re-reads its slot
```

The stub exists to force initialisation on first use and then get out of the way:
`SDL_InitDynamicAPI` overwrites the whole table with the real implementations.
**So the address sitting behind a thunk statically is the one address that stops
being called.** A MinHook splice on it installs cleanly, the banner says so, and
it never fires again.

It bites twice, and the second bite is worse. Resolving `SDL_GetWindowSize` by
*counting* table entries yields a stub belonging to a *neighbouring* function —
and because every DEFAULT stub has the **same prologue**, a pinned-build check
passes. It then runs the wrong SDL function and throws `std::bad_alloc` from
inside the game's allocator on the first frame.

- **To intercept an SDL function, write its jump-table slot.** It is the
  indirection the shim exists to provide, the table is in `.data` and already
  writable, and "call the previous value" is a trampoline for free.
- **To CALL an SDL function, READ its slot.** Same table, other direction.
- **Decode the RVA from the thunk's own `jmp cs:off_...` operand**, never by
  counting. The same decode reproduces `kRva_SdlSwapSlot` exactly, which is what
  makes these trustworthy.

| function | thunk | slot RVA |
|---|---|---|
| `SDL_GL_SwapWindow` | `0x140B9B7D0` | `0x012DE650` |
| `SDL_GetWindowSize` | `0x140B9CF10` | `0x012DF170` |
| `SDL_GetWindowSizeInPixels` | `0x140B9CF20` | `0x012DF178` |

Real implementation, for reference: `SDL_GL_SwapWindow` is `0x140BDF740`
(identifiable by its own error strings).

**This is also why the ImGui panel uses the win32 + opengl3 backends, not
imgui_impl_sdl3**: the SDL3 backend makes ~40 direct `SDL_*` calls, each needing
its slot RVA decoded by hand. The win32 backend needs one `HWND` and a `WndProc`
subclass, and `imgui_impl_opengl3` carries its own GL loader.

---

## Mod architecture

```
L0  Loader          proxy DLL or injector; MinHook inline hooks at resolved RVAs
L1  Determinism     RNG stream control + the fences
L2  Transport       TCP, length-prefixed frames, behind an interface
L3  Scheduler       command queue, turn barrier, hash exchange
L4  Session         host/join, run state push, seed agreement
L5  Diagnostics     desync detect -> halt + dump
```

L2/L3/L4 exist and pass in-game. L5 halts and freezes both peers on a hash
mismatch and prints a cross-peer state diff — see `STATEDUMP` below. A *crash*
dump exists (`mgmp_crash.cpp`, a first-chance VEH printing exception code, C++
type name, `module+rva` stack, and whether `mgmp.dll` is on it). L5 also has a
live face: the ImGui panel.

**Pure logic is extracted into headers and unit-tested** whenever the hook itself
needs two live peers (or an AstroZombie) to exercise — `td_decide`, `HashRing`,
`barrier_decide`, `BattleTracker`, the config reader.

**Know the limit of those tests:** they pin *what a function returns*, never
*where it is called from* — which is exactly the bug the join barrier shipped
with, and exactly why the epoch counter passed every test it had while being
unable to survive a reconnect.

### Configuration

`mgmp.json` beside the DLL holds only what is actually turned: `game`, `log`,
`net.{role,addr,port,control}`, `ui.{enabled,visible,key}`, and a `debug` block
(`follow`, `follow_delay_ms`, `join_barrier`, `desync_halt`, `record`,
`record_note`, `replay`). Everything else is a `constexpr` in
`src/core/mgmp_tuning.h`, which also records why each one is not in the file.
`tests/test_config.cpp` covers the reader.

**Two settings change the game rather than observe it**, and both shout in the
startup banner while on:

- **`tune::kHookSaveScum`** (ON) no-ops `MewDirector::ApplySaveScumPenalty` @
  `0x1408DD9C0`. The only write it makes to the run object is `++*(u32*)(run+0xE0)`
  — the scum counter; every other reference is a *read* of it picking an
  escalation tier. It spends that on per-cat penalty fields at `Cat+0x980`/`+0x9A8`
  and on queueing `steven_savescum_100` / `steven_savescum_houseboss_100`. `void`
  return, both call sites in `ContinueAdventure`, not virtual, so swallowing the
  call removes it completely. **The reason to disable it is the capture
  methodology**: the penalty mutates cat state as a function of the reload count,
  so with it live no A/B capture of the same battle means anything. The same
  argument applies to multiplayer, which relaunches the game for reasons that have
  nothing to do with the run — a rebuilt DLL, a peer that dropped, a deliberate
  reconnect — so the penalty was measuring the tooling. It only stops the counter
  and the debuffs from here on; **what earlier reloads already wrote to `run+0xE0`
  and to the cats stays written.**
- **`tune::kHookTimeDelay`** — see `TimeDelayStatusApplication` above.

**`tune::kConsole` is OFF.** Everything the console window showed is readable
somewhere better: the stamped file log keeps the whole run, the ImGui panel shows
it live and filtered, and failures that happen before the logger exists go to
`mgmp_boot.log`, which the console never saw either. The one thing it was the sole
sink for — the name of the stamped log file — is the first line *inside* that file
(`fprintf` with `%ls`, not `fwprintf`: the first call sets the stream's
orientation and every other write is byte-oriented `fwrite`, which a wide-oriented
stream would refuse).

### Repository conventions

The repository root is `mod/`, and this file lives in it — so a reference to it
from inside the mod is `CLAUDE.md`, never `../CLAUDE.md`. **`README.md` is for a
person who wants to play; this file is for a person who wants to change it.**

- **`src/core/mgmp_sigs.generated.h` MUST be committed.** Everything else
  generated is ignored and the reflex is to ignore this too — but
  `mgmp_resolve.cpp` `#include`s it, and regenerating it needs `Mewgenics.exe.i64`,
  which nobody cloning the repository has. Ignoring it produces a repository that
  cannot build. Its report, `tools/ida/sigs_report.txt`, is ignored; the header is
  not.
- **Vendored dependencies are committed as upstream shipped them.** A nested
  `.git` makes `git add .` silently record a **gitlink** rather than the files, so
  a clone gets an empty directory and a failed `add_subdirectory` while the
  machine that has the directory sees nothing wrong. `.gitignore`'s build patterns
  are therefore anchored (`/build/`, not `build/`) and it ends with
  `!third_party/**`, because the unanchored forms reach inside the vendored copies
  and cut holes in them.
- **`traces/`, `*.log` and `*.mgr` are ignored for leakage, not clutter** — each
  embeds the absolute path of the executable that produced it. Anything committed
  as evidence has to be scrubbed by hand first.

### Surviving a game update — signature resolution

The RVAs below are **hints**, not answers.

**A prologue guard could never have done this job, and it was weaker than it
looked even at its own job.** A fixed-RVA 16-byte compare answers "is this still
the pinned build" and cannot answer "where is this function". Measured against
the shipped PE, the prologues of both `MewSaveFile::Load_0` and `sub_1400B6A90`
occur **six times each** in the image. A resolver that refuses anything matching
more than once cannot make that mistake: "these two are indistinguishable" and
"this pattern matched twice" are the same fact.

**Resolution order, and the point is that the common case costs nothing:**

1. Verify the pattern at `base + rva_hint`. Hit → done, **no scan at all**.
2. Otherwise scan `.text`. Exactly one match → resolve and log the drift.
   Zero → refuse. **Two or more → refuse**, never pick: MSVC ICF folds identical
   functions, so ambiguity is an expected outcome, not a broken pattern.

**Refusal is per-target and graded by how the failure would present**, not by how
much the feature is liked. A missing draw call turns the cursor off and says so —
visible. A missing `GetChoice` means remote decisions are never injected and both
peers quietly play their own game — silent, which is worse than a stall. Only
`NextTurn`, `GetChoice`, `ApplyAction` and `FrameBegin` are critical enough to
refuse to load.

**Wildcards are derived, not guessed:** `call/jmp rel32`, RIP-relative `disp32`,
and immediates holding an in-image address. Struct displacements (`[rcx+0E8h]`)
are **kept** — they are semantic, they carry most of the uniqueness, and if
`+0xE8` moves the signature should break loudly. Patterns grow one instruction at
a time and stop at the first **minimal-unique** length: shorter is less fragile
across a rebuild. Result on this build: 12–53 bytes, most with zero wildcards.

**Anchor choice is the whole performance story.** The scan broadcasts one pattern
byte and lets AVX2 find candidates. Anchoring on byte 0 gives a median of
**1,328,519** candidate positions; anchoring on the pattern's rarest byte (from a
256-entry histogram of `.text`, built once) gives **15,622** — **85x fewer**
verifications.

**AVX (1) is useless here and checking for it would be a mistake.** Its 256-bit
operations are float-only; the byte compare needed is `_mm256_cmpeq_epi8`, which
is AVX2. So the check is `cpuid` leaf 7 EBX bit 5, gated on leaf 1 ECX bits 27/28
**and** on `xgetbv` XCR0 bits 1–2 — that last step is not optional, because a CPU
can report AVX2 while the OS does not preserve YMM across a context switch. SSE2
is architectural on x86-64 and needs no check.

**`hooks_verify_module` does not refuse on `SizeOfImage`.** It used to, which
means that on the day the game patched, the mod would not have loaded — and the
one mechanism built to survive an update would have been skipped by the check
guarding it. It is a log line.

**Measured, running the shipping resolver against the shipping `Mewgenics.exe`**
(`tests/test_resolve_live.cpp` maps it with `LOAD_LIBRARY_AS_IMAGE_RESOURCE`,
which lays the sections out at their VAs without executing anything, so every
`base + rva` is genuinely rebased rather than accidentally right):

| | |
|---|---|
| pinned build | 49 resolved, **0 scans**, 2.26 ms |
| every hint poisoned (`--force-scan`) | 46 full `.text` sweeps, **28.57 ms** |

~650 MB of scanning in 28.5 ms, so the fallback is a viable startup path and not
a last resort. Under `--force-scan` all 46 still resolved **uniquely**, which
re-confirms uniqueness through the C++ path and not only through Python.

**The three SDL_DYNAPI slots are the known gap and are still hardcoded.** No byte
pattern can find them: every thunk is `jmp cs:off_...` and every default stub
shares one shape, so a slot is indistinguishable from its neighbour. They need a
third mechanism, designed but not written: locate the real implementation by a
unique pattern (its own error strings identify it), then find the single qword in
`.data` holding that address. Sound because the mod injects at `FrameBegin`, long
after `SDL_InitDynamicAPI` has overwritten the table.

### Hook points

**These RVAs are the `rva_hint` column.** They are exact for this build and are
what a human reads; what the mod hooks is whatever the signature resolves to.

| Address | Symbol | Purpose |
|---|---|---|
| `0x1409A7890` | `ApplicationBase::initSystems` | init hook — but we are injected after it has run |
| `0x1408E0010` | `TurnControl::NextTurn` | turn boundary: barrier + hash |
| `0x1408E1190` | `TurnControl::ApplyTurnAction(TurnAction)` | **the command boundary.** Only two callers, non-virtual |
| `0x1408DFC20` | `TurnControl::QueueDecision(TurnAction)` | pushes decisions into the ring; `operator new(0x88)` fixes `sizeof(TurnAction)` |
| `0x1408DFDD0` | `TurnControl::QueueFunctionAction(std::function<void()>)` | the generic type-7 factory; 17 callers |
| `0x1408DEDE0` | `TurnControl::update` | drains the queue; frame driver for turns |
| `0x1401374D0` | `TurnAction Brain::UpdateDecision(void)` | per-frame wrapper; caches the pending choice at `Brain+0x220`, gates release on a **wall-clock dt** timer at `Brain+0x2A8`. **Hooked**: freezes the aim preview's write to facing, and drives the peer's aim preview. `Brain+0x38` is the `Character*`. **Returns a `TurnAction` BY VALUE — `rcx = this`, `rdx = &sret`** |
| `0x14010DB40` | `Character::DoAction(TurnAction, bool)` | ability actions only — an end-turn never reaches it |
| `0x140032050` | `Ability::trigger(TurnAction)` | command execution point |
| `0x140137B70` | `Brain::GetChoice` | **a poll, not a decision point** — 1695 of 1711 calls in one battle returned `type=1` |
| `0x1401183E0` | `Character::FindAbility(const std::string&, bool)` | the identity scheme |
| `0x140118230` | `Character::GetAllAbilities(out, bool)` | enumeration order is move, attack, spells, bonus |
| `0x1408DEDA0` | `TurnControl::GetCurrentActor` | `TurnControl+0x68` |
| `0x14094B550` | `RollChance(double,double,u64*)` | the proc/crit gate |
| `0x1408DD9C0` | `MewDirector::ApplySaveScumPenalty(RunState*, int)` | no-op'd when enabled |
| `0x14024F1C0` | `TimeDelayStatusApplication` vtable slot 11 | the dt countdown → turn countdown |
| `0x140109FA0` / `0x14010B250` | `Character::BeginTurn(int)` / `EndTurn()` | per-character turn bracket |
| `0x140817320` | `StatusMenu::update` (vtable slot 11) | the battle HUD tick; the BOARD half of the cursors |
| `0x012DE650` (a slot, not a function) | `SDL_GL_SwapWindow`'s SDL_DYNAPI entry | the SCREEN half, taken over by writing the jump table. Also drives the ImGui panel, after the peer pointer |
| `0x140391050` | `MapScreen::EnterNode(MapScreen*, MapNode*)` | **the meta-layer command boundary.** One code caller, not virtual |
| `0x140937F30` | the world-event commit | **the event choice boundary.** Takes the chosen entry as an argument |
| `0x1409122C0` | `WorldEvent::update` | the client's apply tick; the only reliable source of a live `WorldEvent*` |
| `0x140382A00` | `LevelUpScreen::select_option` | **the level-up choice boundary.** `LevelUpScreen+160` is the subject `CatData*` |
| `0x140382640` | `LevelUpScreen::update` | the client's apply tick |
| `0x14038E7D0` | `MapScreen::update` | the follow tick; only reliable source of a live `MapScreen*` |
| `0x1401BCE90` | `SaveSelection::ContinueSlot(int slot, bool play_sound)` | **the save-screen boundary.** Calling it *is* clicking that slot |
| `0x1401BAD60` | `SaveSelection::update` | the auto-continue tick |
| `0x1403A5FC0` | `MewDirector::init(std::string)` | takes the filename **by value**, so substituting our own redirects the load |
| `0x1409A9D80` / `0x1409AA290` | `FrameBegin` / `FrameEnd` | socket pump; `FrameBegin` is also the loader's park point |

**A DETOUR MUST REPRODUCE THE WHOLE ABI, AND AN MSVC SRET IS AN ARGUMENT.**
`Brain::UpdateDecision` is `TurnAction UpdateDecision(void)` — a 0x88-byte return
by value, so the caller passes the destination in `rdx`:

```asm
1408DF2EF  lea    rdx, [rbp+170h+var_170]     ; the sret buffer
1408DF2F3  mov    rcx, [rax+68h]              ; the Brain
1408DF2F7  call   glaiel::Brain::UpdateDecision
```

Typed as `void __fastcall h(void* self)`, the detour's own C++ clobbers `rdx`
before it reaches the trampoline, and the game then writes a whole `TurnAction` —
**`std::function` at `+0x78` included** — over whatever `rdx` happened to hold,
every frame a decision is pending. It presents as a `C0000005` in
`sub_1408E7DE0+0x1AD` (the turn-order portrait) on a pointer some *other* function
returned, **on both peers at the same instruction**, with no `mgmp.dll` frame on
the stack. Two peers crashing identically is not a desync; it is the same
deterministic corruption running twice.

**Check the IDA prototype of every hook target, not just the interesting ones** —
a name does not carry a return type. All 27 were re-checked after this and no
other mismatch exists.

**Functions the mod CALLS rather than hooks** (`kCalls` in `mgmp_addresses.h`,
signature-resolved, because a bad call address runs an unrelated function with
our arguments on the game's stack — and a call target that does not resolve turns
its own feature off by name rather than being called anyway):

| Address | Symbol |
|---|---|
| `0x1403B9CE0` | `MewDirector::save_adventure(MewDirector*)` |
| `0x14022E9A0` | `SerializeCatData(CatData&, ByteStream&, bool)` — bidirectional |
| `0x1409B3130` | `ByteStream::~ByteStream` — frees the write buffer on the **game's** heap |
| `0x14032D0A0` | `std::ofstream::ofstream` — the one embedded at `ByteStream+0x30` |
| `0x1400D6980` | `CatData* by_id(registry, u64)` |
| `0x1408DD2F0` | `RunHistory::serialize(RunHistory*, ByteStream*)` — bidirectional |
| `0x14013C570` | `Component* -> ImmediateModeGameUI*` — null off the battle screen |
| `0x14033FFD0` | `ImmediateModeGameUI::tile_piece(...)` — `MOVAPS` alignment applies |
| `0x14013A030` | `Brain::DrawAbilityAOE(Ability*, iVec2D, iVec2D, int)` — the AOE shape under the cursor. `rcx` brain, `rdx` ability, `r8`/`r9` the two `iVec2D` **by value in registers** (8 bytes each), `[rsp+0x20]` the layer, `0x13` at the game's own site. Null-checks the ability itself in its first instruction |
| `0x140138A10` | the **range/reachable** tile highlight. **NOT a draw** — see below. `rcx` brain, `rdx` ability, `r8d` an int, `-1` at every site. Five callers: `Brain::UpdateDecision` and **four** in `PlayerBrain`'s update (`0x140776163`, `0x140776577`, `0x1407770C1`, `0x14077717A`) |
| `0x14082E520` | `TacticsObject::Move(iVec2D, bool, bool)` — `rcx` tobj, `rdx` tile **by value**, `r8b` 1, `r9d` 0. Half of the Move preview's displacement |
| `0x140101C60` | `Character::recompute_stats(Ability*, bool)` — `rcx` char, `edx` 0, `r8b` 1. The other half |

**A 16-byte prologue guard is not enough for a call target.** `sub_1403B9CE0` and
`MewDirector::ContinueAdventure` are **byte-identical for their first 21 bytes**,
diverging only in the displacement of the `lea rbp, [rax-N]` that sizes the frame:

```
save_adventure   : 48 8B C4 48 89 58 20 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 68 FE FF
ContinueAdventure: 48 8B C4 48 89 58 20 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 D8 FC FF
```

A 16-byte compare would have **accepted `ContinueAdventure`** — the function whose
first statement is `DestroyScene("House")` and which frees the entire live cat
registry. Same shape as the SDL neighbouring-stub trap, far worse landing.
Uniqueness replaced it: the generated signature keeps growing until it matches
exactly once, so it either names that function or resolves nothing.
**Verify a new call target against the shipped PE, not against IDA notes** — that
is what `tools/verify_sigs.py` automates.

**Injection is not possible before the entry point on this build** (measured). A
remote thread created at the PE entry point faults during `DLL_THREAD_ATTACH` — a
loaded DLL's thread callback needs state that only exists after startup. The
loader parks the game at `FrameBegin` and injects there: before frame 1, after
`initSystems`.

### Protocol

The messages and their fields are defined in `src/net/mgmp_proto.h`; what follows
is only what the header cannot say.

**`battle_id` is the node seed, and there is no `ACTIONLOG` message.** Both are
the same economy: identity comes from something both peers already read
(`MapNode+0x118`), and catching a peer up is done by re-sending messages that
already exist — `ENTERNODE` for where the run is, and the battle's `ACTION`s
replayed one at a time.

**`ACTION` only ever carries type 2 or type 3.**

**A counter is not an identity — the fourth time this project learned it**, after
ability slots, `CatData` ids and `ChoiceMsg::node_seed`. `battle_id` used to be a
per-process counter starting at 0. After a reconnect the client sat at 0 while the
host was at 108, and three receive paths that all want exact equality discarded
everything: **the first battle after any reconnect would have stalled in
silence.** Adopting the host's counter on join is worse, because the bump is
*observational* — it fires when lockstep notices the character list changed at a
turn boundary, so "which epoch is this battle" is not answerable at the moment of
adoption. A host mid-fight has already bumped, a host on an event node has not,
and the off-by-one lands on exactly the two cases that matter. The node seed has
no such moment.

Ordering came free with a counter and does not with an id, so each peer keeps a
ring of the `battle_id`s it has **retired** (16 deep). That set is the only thing
separating two cases that need opposite handling: a retired battle is **dropped**,
anything else unrecognised is **held**. Deliberately **not** a halt condition — a
peer legitimately a battle ahead or behind produces both. An id that falls off the
ring degrades to *hold*, never to a wrong *drop*.

**`CONTROL` is symmetric** rather than host-authoritative: both peers send their
own half and check the other's, which puts the result in both logs and makes
"claimed by neither" detectable. `NODEHASH` is symmetric for the same reason — a
host-authoritative meta hash could only ever report that the client disagreed, and
it is the host that is authoritative about the run.

**`HELLO` must carry a `resources.gpak` hash, and it is not optional.** A single
differing proc chance desyncs the stream *even when the outcome looks the same*,
because `RollChance` takes no draw at `p >= 1.0`. Current implementation is file
size + first 1 MiB (which covers the whole index), **not** a content hash —
hashing 5 GB at startup is not viable. It will not catch a byte edited inside a
payload without changing its length.

**`CURSOR` and `AIM` are the two messages that exist to be SEEN, and both are
deliberately outside the lockstep contract.** Nothing either carries is hashed,
replayed or acknowledged, so a dropped or late one cannot desync anything; they
are therefore the only messages allowed to be sent on a timer (on change, ≥50 ms
apart, plus a 500 ms heartbeat) rather than at a command boundary.

`AIM` is the peer's aim preview, and it costs almost no new code because the game
already draws it. The whole feature is to put the aim on the wire (as a **slot**,
never an ability pointer, plus the GON name as the usual cross-check) and make the
game's own draw calls on the peer that does not own the cat, from the same hook,
in the same frame slot, at the same layer. An `AIM` is **not** a decision: the
same one is sent dozens of times and then abandoned, and the decision still
arrives as an `ACTION`.

#### The source is the SELECTION, not the cached decision

`Brain+0x220/+0x228/+0x230/+0x238` — what `Brain::UpdateDecision` draws from at
`0x140137798` — is gated on `cmp [rdi+220h], 2`, the **cached committed
decision**. That state exists for the sliver between a click and the action being
applied, not for the seconds a player spends choosing. Reading it measured **121
aims sent, 0 frames drawn**: every send was an inactive heartbeat.

What a player is looking at lives on the `PlayerBrain` and is drawn by its own
update, **`sub_140775EB0`** (vtable `0x140F82D78`; `DbgPlayerBrain` `0x140F82830`
and `MountBrain` `0x140F82958` share the implementation). `this` is `r14` at both
of its `DrawAbilityAOE` sites, `0x14077659C` and `0x1407771A4`:

| offset | field |
|---|---|
| `PlayerBrain+0x3D8` | the selected `Ability*` (null = nothing selected) |
| `PlayerBrain+0x358` | `iVec2D` target tile |
| `PlayerBrain+0x360` | `iVec2D` direction |

These offsets are only ever touched through the human-cat gate, so the object is
a `PlayerBrain` when they are read.

#### `sub_140138A10` IS NOT A DRAW

One instruction above each `DrawAbilityAOE` site sits
`sub_140138A10(brain, ability, -1)`, which looks exactly like "the range half" —
the green reachable set under a Move. It collects a set of objects, sets
`[obj+0x118]` on each, and calls `sub_140151CE0` four times (`0x140138C7A`,
`D4A`, `E3A`, `E9A`), and that function on a flag change runs

```c
v16 = glaiel::apply_status(&name, v11, v14, v12, v11, nullptr, true, true);
```

on a **real `Character*`** (`v11 = *(a1+56)`) and pushes the returned `Status*`
onto a vector on the object. The tiles are a side effect of those statuses.
`sub_140138A10 → sub_140151CE0 → apply_status → sub_14062F050 →
Character::Face` is a four-hop path to a write of `Character+0x388`.

On the peer that does not own the cat that is a **simulation mutation, thousands
of times a battle**. Measured in the first session it ran: a turn agreed on every
hashed component while cat 31's facing read `(0,-1)` on the host and `(-1,0)` on
the client; the very next AI decision diverged from an identical board —
`Move (5,7)` / `attack (5,2)` against `Move (6,8)` / `attack (8,5)` — and the next
turn halted. **The RNG fence around the call could not see it: the fence guards
the stream, and this moved state.**

**A later read narrows the status pathway and DOES NOT close the case.** All four
`sub_140151CE0` sites sit inside one branch of `sub_140138A10`, guarded by
`memcmp(ability_name, "tk_MonkStyleChange", 0x12) == 0` — so on any ordinary
ability the status half never runs at all. That is worth knowing and it is *not*
an all-clear: the run that died had `Character+0x388` differing on cat 31, and
either that cat was carrying MonkStyleChange or the write came from somewhere else
in the function. **Nobody has established which**, and until someone does the
guards stay. The obvious other candidate is the `[obj+0x118]` write the function
makes directly, which the suppressor does not cover and the state fence exists to
catch.

**It is called anyway, with the mutation switched off rather than argued about.**
Three guards, and the middle one is the actual fix:

| guard | what it covers |
|---|---|
| the RNG fence | `TLS+0x178` moving under the call |
| **`T_HighlightRefresh` swallows `sub_140151CE0`** while `aim_highlight_suppressed()` is true | the whole status pathway — the apply, the removal, the vector bookkeeping — does not execute |
| **`lockstep_state_fence_*` across the call** | everything the suppressor does not: `sub_140138A10` still writes `[obj+0x118]` itself, and nobody has established what reads it |

The suppressor is scoped to the duration of one call, on the game's own thread,
so the game's own highlight can never be caught by it. The state fence snapshots
the same `CatState` the per-turn hash uses for every snapped cat, **puts facing
back** — correct for the same reason the aim-preview freeze is — and *reports*
anything else, because inventing an HP or a tile would hide a divergence rather
than fix one. `AIM done:` carries both counts, so a session that drew thousands of
highlights and caught nothing is the evidence, and a session that caught something
says so in the first battle rather than on turn 49.

A rejected middle step is worth recording: reproducing the game's *threat*
indicator at `0x140343CDB` (attack slot, own tile, facing, layer `0x14`) instead.
It draws a single white exclamation tile that reads as a map event, appears only
in narrow circumstances, and is not the reachable set anyone was asking for.

`DrawAbilityAOE` itself was then walked properly: **154 functions to depth 6, and
it reaches none of `apply_status`, `Character::Face` or `ReceiveDamage`.** That is
the audit every call into the game on one peer only has to pass.

#### The attack range under a Move is not a draw either — the game moves the cat

Selecting Move shows two things, and only the first comes from `sub_140138A10`:
the green reachable set, and the tiles the cat could **attack from wherever the
mouse is hovering**. The second is produced by `PlayerBrain`'s own update,
`sub_140775EB0` @ `0x14077636B`…`0x140777403`, by **displacing the cat and putting
it back**:

```asm
inc  [Character+0xEC0]                      ; a re-entrancy / dirty guard
call glaiel::TacticsObject::Move            ; tobj, hovered tile, 1, 0
dec  [Character+0xD10] / inc [Character+0xD3C]   ; spend the move
call glaiel::Character::recompute_stats     ; char, 0, 1
call [ (*(Character+0xD8))->vtable + 0xA0 ] ; the ATTACK slot's range from there
inc  [Character+0xD10] / dec [Character+0xD3C]
call glaiel::TacticsObject::Move            ; and back
call glaiel::Character::recompute_stats
dec  [Character+0xEC0]
```

Two things fall out. It is the **attack slot** (`Character+0xD8`), not "the
longest-ranged ability" — the range simply grows with `bonus_range`, which is what
makes it look chosen. And **the game already does this on one peer only**: the
owner's machine displaces the cat every frame the mouse moves and the other does
not, so `TacticsObject+0x50` (the previous tile `::Move` writes) and both counters
*already* differ between two peers of a working session, and have through every
byte-identical 49-turn run measured. That is evidence the round trip is clean, not
proof — which is why the mod mirrors it inside the same state fence, re-reads the
tile afterwards, and **turns the feature off for the session** on a trip that
fails to come home rather than leaving a cat somewhere it never walked to.

**One substitution in the mirror:** where the game calls the attack ability's
vtable slot `0xA0` and then hand-builds the tile pieces through
`sub_14013B840`/`sub_14013B7C0` (a chain of six opaque string/colour calls), the
mod calls `sub_140138A10` with the **attack** ability while the cat is displaced.
Same set, computed from wherever the cat is standing, and it reuses a call target
that is already signature-resolved, already fenced and already suppressed.

The shape of this risk is *not* the shape `sub_140138A10` had. There the mutation
was hidden four hops below a function named like a draw. Here it is explicit,
symmetric and authored by the game as a preview; the danger is an incomplete round
trip, which is exactly what the fence reports. The call is fenced for the same
reason as everything else made on **one** peer, and a slot that resolves to a
differently-named ability draws **nothing** rather than previewing the wrong tiles.

#### `CURSOR` carries both a tile and a screen fraction

| field | unit | correct when |
|---|---|---|
| `x, y` | tile index (`StatusMenu+124`) | always — same square on every machine |
| `nx, ny` | fraction of the sender's window | the two cameras agree |

`nx, ny` is resolution-independent by construction (the receiver uses it directly
as NDC, so no pixel size ever crosses the wire) but **not** camera-independent.
That is a real limitation, accepted knowingly, and it is why the reticle stays.
Making the pointer camera-correct needs a continuous mouse→world unprojection, and
the only thing the game exposes is already rounded to a tile — the very rounding
the pointer exists to avoid.

`nx, ny` are bit-cast floats rather than fixed point: both peers are x86-64
running the same build, and quantising the one value whose whole point is smooth
motion would be self-defeating. The receiver smooths them with an exponential time
constant — time-based, not per-frame, so it looks the same on two machines running
at different frame rates. The first sample snaps, or a peer would sail in from the
corner on arrival. `mode` is an INDEX into the cursor-ART table rather than the
state string, because the string is a texture name the receiver would have to
trust enough to build a path from; the cost is that the table's ORDER is wire
format — appending is safe, reordering is a protocol change.

#### Version history

**A change to what a peer *computes* from a message is a protocol change even
when every byte stays put.** That lesson cost a session at version 8, and 12 is
the same lesson again.

| v | change |
|---|---|
| 8 | state hash drops facing; roster cap 254 |
| 9 | peer envelope + `PEERS` (up to 4 players) |
| 10 | state hash gains `ElementList` |
| 11 | `CURSOR` |
| **12** | state hash gains live-list membership and stops hashing departed cats — **not one byte moved** |
| 13/14 | `CURSOR` gains a pointer icon and a normalised mouse position |
| 15 | the icon is dropped when the pointer became hand-built geometry |
| 16 | it returns as an index into the cursor-ART table |
| 17 | `CHOICE` |
| 18 | the per-battle `epoch` COUNTER becomes a `u64 battle_id` |
| 19 | `CHOICE` carries the node seed it was made on |
| 20 | `RUNHIST` + `NODEHASH` |
| 21 | `AIM` |
| **22** | `AIM` reads the PlayerBrain's SELECTION instead of the cached decision, and the receiver draws range tiles as well as the AOE — no byte moved |
| 23 | the range-tile call from 22 is REMOVED (it applies statuses); the threat ring is drawn instead. The bump exists so a peer still running 22 cannot join and mutate this one's simulation |
| 24 | the highlight is back, with `sub_140151CE0` swallowed for the duration and the roster's cat state fenced across the call |
| **25** | a Move aim also shows the ATTACK RANGE from the hovered square, so the receiver makes two `TacticsObject::Move` calls per frame — again no byte moved |
| 26 | `STATEDUMP` |

TCP is deliberate: head-of-line blocking is irrelevant when turn-based (you are
blocking on that message anyway), and a turn is a few hundred bytes you want
reliable and ordered. Cost is losing NAT punch/relay. Keep transport behind an
interface. `SteamNetworkingMessages002` is linked for a future move; there is no
lobby API, so direct-IP stays.

### The control split

Derived independently on both peers at the first turn boundary of each battle,
then cross-checked — no negotiation, no waiting:

1. **Find the humans:** every cat whose RTTI brain class contains
   `tune::kReplayBrains` (`PlayerBrain`).
2. **Split by roster index, host first, rounding up:** `to_host = (humans+1)/2`.

Contiguous by index rather than interleaved. A real 4-human battle splits
19,20 → host / 21,22 → client, which is why the host genuinely cannot act for the
second ally cat — that is player 2's.

`net.control` as an explicit index list overrides it, but indices are into *this
battle's* roster, so an explicit list is stale the moment you fight anything else.

**The split is three-way, not two-way**, and getting it wrong is a **deadlock
rather than a desync**:

- **AI** (brain does not match) → decided locally on **both** peers. Never sent,
  never injected, **never suppressed**.
- **human and in this peer's control set** → the local brain decides, decision is
  sent.
- **human and not in it** → `GetChoice` is overwritten unconditionally, with the
  peer's decision or with `type=1`. Unconditionally is also what suppresses a
  stray click on a cat you do not own.

An early version had only the last two branches, so AI cats were treated as
"remote" and both peers sat waiting for a decision neither would send.

### The join barrier — and why it must NOT hold AI brains

Neither peer may take a **human** decision until both have snapshotted the same
battle's roster.

**The barrier applies to human-driven cats ONLY. This is the load-bearing
detail**, and the first version got it backwards at the cost of a run:

- **Holding a human brain is free.** `GetChoice` returns `type=1` by itself while
  waiting on a person.
- **Holding an AI brain is NOT free.** `Brain::UpdateDecision` calls `GetChoice`
  only when nothing is cached, so overwriting the decision it just released makes
  the AI **derive a new one** next frame — and deriving draws from the simulation
  stream. Measured: a host parked 529 polls re-derived that many times, ran its
  stream past the client's, and the two peers' AI then chose different targets
  from the same board:

  ```
  host:    RangedAttackAbility target=(3,8) dir=(-1,0)
  client:  RangedAttackAbility target=(7,3) dir=(0,-1)   <- the correct one
  ```

  turn 0 hashes matched byte for byte; turn 1 disagreed on **rng alone** with
  `state_hash` still identical — the signature of extra draws with no divergent
  outcome yet.
- **Letting the AI run ahead is safe**, because AI decisions are deterministic and
  re-derived identically on both peers (a replay run left 12 of 29 decisions to
  `PatternBrain` and all 12 matched).

**Result after the fix:** a 10-second deliberate late join, 13-turn miniboss
battle, **13/13 hashes agreeing on both peers, 0 desyncs**.

**No disconnect timeout, deliberately.** If the peer never sends `CONTROL` the
barrier never opens and the battle stalls — a visible stall beats a silent wrong
game. **Waiting costs nothing** and that is why the transport needs no timeouts
anywhere: a remote decision that has not arrived is handled by returning `type=1`,
so the game waits exactly as it already waits for a person.

### The per-turn hash is checked in both directions

Comparison used to happen only at a peer's **own** turn boundary, against hashes
that had already arrived. The peer running *ahead* therefore found an empty ring
every time and compared nothing — measured on a clean 4-turn run: client four
`AGREES`, host **zero**. Correctness survived (the trailing peer still catches a
mismatch) but only the trailing peer would halt while the leading one played on.

Fixed by matching in **both** directions. The rings overflow **differently on
purpose**: our own evict the oldest (only recent turns can still have a
counterpart in flight); the peer's refuse the newest and say so (the boundary
consumes them in turn order, so the held run must stay contiguous).

### `STATEDUMP` — the desync dump, and the instant it describes

A hash says *that* two peers diverged and can never say *what by*. Both peers send
the table their own hash was taken over, on **any** mismatch, and each prints a
field-by-field diff. Free, because it goes out once, after the run is already
lost. Gating it on a state-only mismatch was wrong for a reason worth keeping:
*"the streams differ"* is a symptom, and the cat that took different damage two
turns ago is the cause.

Three details carry the whole thing:

- **It is the state AS OF THE HASHED TURN, not the live state.** A mismatch is
  noticed either at our own boundary or when the peer's hash *arrives*, and only
  the first is the instant the numbers were read. `build_hash` therefore keeps the
  rows in the same loop that hashes them. If the turn numbers do not line up, the
  dump **refuses to send** and says so — a table labelled turn N holding turn N+1's
  numbers would produce a diff full of differences that were never in the hash,
  and it would look exactly like a real one.
- **Facing is in the diff although it is not in the hash.** The exclusion is about
  what may *halt* a run; it is not a reason to hide the field once the run has
  already halted. Facing is what explained the turn-49 backstab desync.
- **`stride` is checked, not assumed.** Two peers built from different revisions
  of `CatState` would otherwise reinterpret each other's bytes and print a
  confident diff of nonsense.

`MSG_STATEDUMP` is handled **after** a halt on purpose, since that is the only
time it arrives; the peer's `HALT` normally beats it there. A dump in which
**every row agrees** gets its own loud line: the divergence is then somewhere the
state hash does not look — the stream, the queue, or the summon tail the snapshot
does not cover.

### Reconnect and mid-fight join

A peer can drop and come back, including **into a battle already in progress**.
On HELLO the host sends, in this order — the same order the live path uses:

1. `SAVEFILE`, flushed from the live run first, for a peer whose process
   restarted;
2. `CATDATA` + `INVENTORY`, after forgetting the dedupe caches — **both
   publishers dedupe against the last push and that cache is per-RUN, not
   per-peer**, so without the forget a joiner hears only about what changed since
   the last node;
3. `ENTERNODE` for the node the run is standing in;
4. every `ACTION` of the battle in progress, to that peer only.

A client that only lost the socket **declines** the save and keeps its run: there
is no way to apply a save off the selection screen anyway.

**The load-bearing detail of the replay: a joining client's own human cats would
hang.** AI re-derives identically, but a *human* decision cannot be re-derived,
and the client's own past clicks died with its process. So the choice filler
checks the pending queue for **our own** cat before consulting the brain. In a
normal battle that branch is inert; during catch-up it is the entire reason the
replay completes. No "catching up" flag is needed — *"there is a decision for this
cat that we did not make"* **is** the condition.

**Known limits.** The joiner visibly replays the fight from turn 0 (fast, but
visible). A battle past the pending cap cannot be caught up — it says so and
replays what it has.

---

## Determinism evidence (the load-bearing measurements)

- **Combat RNG is not inlined.** 11142 draws through the API entry points in one
  battle; 44 on the sim stream, the rest on presentation. The API-hook approach
  sees essentially all of the game's RNG.
- **Presentation does not perturb simulation.** Five captures of the same tutorial
  battle span 7,465 → 23,536 presentation draws (3.2x, one deliberately churned)
  and **all five produced exactly 44 sim draws**, byte-identical in function, call
  site, pre-state, result and order — including mid-battle draws, not just setup.
- **The decisive capture pair.** An ordinary non-tutorial battle: 15 turns, 49
  actions, 308 sim draws. Played in 301.64 s / 23,211 frames, replayed in
  121.28 s / 12,230 frames, with a worst single frame of 10,304 ms against
  1,083 ms. `--sim` over both: **IDENTICAL, 419 comparable records.**

  The per-turn split is the actual finding: turns *with* a decision ran 2.6x–5.4x
  longer and produced identical sim draws every turn; turns *without* one ran
  **1.00x, to the centisecond**. So ordinary combat is immune to frame starvation,
  the decision queue is order-driven rather than time-driven, and the 1.00x is the
  internal check that the instrument is sound.
- **The AI is deterministic across runs**, which is what lockstep actually needs.
  A replay injected only 17 of 29 recorded decisions and deliberately left 12 to
  `PatternBrain` to re-derive; **all 12 matched.** Reproduced across processes in
  the first live two-peer run.
- **Where the draws are, in a real battle:** turn 0 = 46 (cat generation), turns
  3/8/12/13 = 10 total mid-battle (every one a `RollChance` pair), turn 16 = 88
  post-battle (`CatData::set_class` cluster, combat rewards, level-up). Combat
  resolution is a handful of draws — but they exist, which no tutorial ever showed.

**Why record/replay was the right experiment:** two live instances differ in
mouse, timing, camera and hover all at once, so a log diff tells you *that*
something diverged, not *what*. Sequential runs pin every variable and move one.
Bonus: injecting recorded actions at `GetChoice` is structurally identical to
injecting *remote* actions, so it built the lockstep seam. (The dev box runs two
instances fine — a claim that it could not was never measured and is not part of
this argument.)

### Record format v6

`EvHead` carries `uint64_t qpc` (ticks since `record_init`) in the **header**, not
per payload, for two reasons: the single emit choke point every record passes
through means one clock read covers every kind; and diff keys are built from
payloads, so a header field is excluded from comparison **by construction**.
`EV_META` carries `qpc_freq` — nothing else can convert ticks to seconds. The clock
is `QueryPerformanceCounter` deliberately, because the game derives its own dt from
`SDL_GetPerformanceCounter`, which wraps QPC.

The decoder **probes header size before parsing** (the version lives inside the
first record's payload), so captures v1–v5 still read.

**`EV_FRAME` is excluded from the diff outright.** A starved run has fewer frames
*by construction* — that difference is the measurement, not a divergence.

`EV_QUEUE`'s call site is the sharpest signal in the format: types 6 and 7 are
never player decisions, so they are *derived* from proc rolls. One appearing in one
run and not the other is the earliest possible evidence of divergence, and the RVA
turns that evidence straight into a class name.

**It does not see inlined draws.** Hooking the entry points catches call-based
sites and is blind to inlined xoshiro rounds, so *a clean diff is not proof of
determinism* — it is proof that the ~210 call-based sites agree.

**The global-stream filter is self-checking:** one pointer comparison against
`[gs:0x58][0] + 0x178`. If that address is wrong, every draw records as `scratch`
and the trace reports `0 on the global stream` at each turn — so a wrong guess
announces itself in the first battle instead of producing a quietly empty
recording.

**Capture notes lie — trust the filename and contents.** Several `.mgr` notes name
the wrong run, and one pair has "played" and "replayed" the wrong way round. The
per-turn timing table is what identifies which is which, unambiguously.

---

## Running the loopback test

`tools/net_test.ps1` maintains `build_host/` and `build_client/`.

| flag | what |
|---|---|
| `-Build` | build and deploy |
| `-LateClient <ms>` | **client lags every map node by that long**, manufacturing the late-join gap on demand |
| `-NoBarrier` | turn the join barrier off on both peers — the control half of that experiment |
| `-NoHalt` | report hash mismatches instead of halting, on both peers |
| `-Record` | turn the RNG hooks on |
| `-NoFollow` | drive both instances to the same battle by hand |
| `-HostCats`/`-ClientCats` | explicit split; refuses overlapping lists up front |
| `-Summary` | read the newest log pair and print a verdict |
| `-Stop` | kill both |

`-Summary` deliberately ignores the old un-stamped log name so it cannot read a
stale file and present it as this run's, and it **says so when only one direction
was exercised**, so a run where only the host acted cannot be mistaken for a pass.

### Operational traps

- **Each peer needs its own directory.** The DLL reads `mgmp.json` from beside
  itself, so one build directory means one config.
- **`net_test.ps1` derives both configs from `build\Release\mgmp.json`** — the
  CMake output directory — **not** from `build\mgmp.json`. CMake ships the
  template there via `copy_if_missing`, so it is never clobbered once it exists.
- **Logs are timestamped** (`mgmp_host_YYYYMMDD-HHMMSS.log`). Overwriting loses
  the run that explained a desync the moment you relaunch to look again.
- **Both instances share one save directory** — the game resolves it through
  `SDL_GetPrefPath` → `SHGetFolderPathW(CSIDL_APPDATA)`, which no environment
  variable can redirect. That is what makes the loopback test easy and why you
  should `save_snapshot.ps1 -Snapshot` first: the two instances overwrite each
  other's save on exit.
- **PowerShell enumerates the output of an `if` used as an expression.** A
  single-element cat list came out as a bare number, so `"control": 0` reached the
  DLL, which refused it and fell back to auto. Assign from a statement, not from
  `= if (...) {...} else {...}`.

### Known noise (not bugs, do not chase)

- The client logs **`peer 0 'host' accepted (proto N)` twice.** Both drain sites
  route to the same handler and `go_ready` is idempotent.
- `control = (nothing -- observer)` in the banner shows the *explicit* index list,
  which is empty under the default `auto`. The real split is printed at the first
  turn boundary.

### Leaving the tutorial

**`MewDirector::SkipTutorial` @ `0x1403AE940` is shipped and supported.**
`PauseMenu::TryAbandonRun` @ `0x1402964E0` swaps its confirmation text to
`{POPUP_TRY_SKIP_TUTORIAL}` whenever `MewDirector::IsTutorialRunActive` @
`0x1403AECB0` is true, and `AbandonRun` then calls `SkipTutorial`. So **Pause →
Abandon Run during the tutorial *is* Skip Tutorial.** No patching.

---

## Rules that generalise

Each of these was learned the expensive way and applies to work that has not been
done yet.

1. **Every drop site must say what it dropped.** A silent overwrite in a drain or
   a slot is this project's most expensive recurring bug — it has cost sessions at
   `MSG_HELLO`, at `SAVEFILE`, and a whole run at map-node following.
2. **"Do not apply" and "discard" are different.** The host publishes the instant
   it clicks, routinely before the client has entered that node. A seed mismatch
   means **hold**; only the module that can pass a node over may declare one stale.
3. **A check that cannot measure must not report agreement.** `0 desync(s)` with
   no `AGREES` behind it means nothing was compared. Detection must survive its
   subject being disabled, and agreement over unmeasured components must say so.
4. **A predicate named for what you WANT is not a predicate for what it MEANS.**
   `lockstep_in_battle()` reads as "a battle is running" and computes "a roster
   was snapshotted and not yet replaced" — true on the map between fights. Two
   modules guarded on it and silently refused every per-node push for the rest of
   a run. Check what a predicate returns at the moment you are calling it.
5. **An exclusion is an assumption, and it decays.** Facing left the state hash
   with a written justification that was true about the acting cat and false about
   the defending one, and nothing rechecked it for eighteen versions — a passing
   test suite says nothing about a field nobody hashes. Re-read the reason for
   anything deliberately left out of a check whenever you learn something new
   about what reads that field.
6. **A function reached from a drawing path is not therefore a drawing function.**
   The audit before calling into the game on one peer only is a callee walk for
   `apply_status`, `Face` and `ReceiveDamage` — not just for `gs:58h`. "Does this
   reach RNG?" is the right question for determinism and the wrong one for a
   one-sided call, where any state write is a divergence.
7. **A guard that watches the wrong quantity is silent in exactly the case it was
   trusted to cover.** The RNG fence reported nothing, correctly, while the call
   it guarded was mutating facing.
8. **Mirroring a feature means finding the state a PLAYER sees**, not the state
   the function you already hooked reads. A publisher and a receiver that agree
   perfectly about the wrong field are indistinguishable from a working feature
   until someone looks at the screen.
9. **Measure the sender before touching the receiver**, and **read the asset and
   the game's own state before blaming your code.** One log line proved a publish
   side innocent and moved a whole investigation; SWF bounds answered "why is
   nothing visible" with no game running.
10. **Logic that is correct in isolation, called at a moment nobody checked**, is
    the failure mode unit tests cannot reach — re-arming lockstep wiped a roster
    the peer had never left, catch-up applied `CatData` into a live fight, and
    `save_adventure` ran mid-node. Tests pin what a function returns, never where
    it is called from.
11. **A default that no caller uses is not a default.** A setting documented ON in
    the header and forced OFF by the test script is a setting nothing ever ran
    with.
