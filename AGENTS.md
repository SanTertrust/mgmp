# Mewgenics — Multiplayer Mod (`mgmp`) Guide

**Goal:** Shared-run co-op multiplayer for Mewgenics. Two (up to 4) players share one adventure run, split its cats, and play together.  
**Scope:** **Adventure only** (map → node → battle / shop / event / level-up → repeat). The **House** (breeding, furniture, `HouseInventory`) is out of scope.

## Sync Architecture

| Layer | Serializable? | Deterministic? | Mechanism |
|---|---|---|---|
| **Meta** (map, shop, level-up, inventory) | Yes (`SerializeCatData`, `Inventory`, `RunHistory`, `SQLSaveFile`) | Partial (same save + `EnterNode` seed) | **Host-authoritative state sync + choice replication** |
| **Battle** | No (no `Character`/`Status`/`TacticsGrid` serializer) | Yes (`TLS+0x178` sim stream) | **Deterministic lockstep over TCP** |

---

## Codebase Map

- `src/core/` — DLL entry (`dllmain.cpp`), hooks (`mgmp_hooks.*`), signature resolver (`mgmp_resolve.*`, `mgmp_sigscan.*`, `mgmp_sigs.generated.h`), pinned RVAs (`mgmp_addresses.h`), compile-time switches (`mgmp_tuning.h`), runtime config (`mgmp_config.*`), crash VEH (`mgmp_crash.*`), GPAK reader (`mgmp_gpak.*`), guarded memory reads (`mgmp_mem.*`), RTTI (`mgmp_rtti.*`).
- `src/net/` — TCP transport (`mgmp_net.*`), wire protocol (`mgmp_proto.h`), hash ring (`mgmp_hashring.h`).
- `src/determinism/` — RNG hooks & stream filter (`mgmp_rng.*`), `TurnAction` decoding (`mgmp_turnaction.*`), ability slot/GON resolution (`mgmp_ability.*`), `.mgr` recorder/replayer (`mgmp_record.*`, `mgmp_replay.*`), `TimeDelayStatusApplication` hook (`mgmp_timedelay.h`).
- `src/session/` — Battle lockstep & `state_hash` (`mgmp_lockstep.*`), `BattleTracker` (`mgmp_battleid.h`), join barrier (`mgmp_barrier.h`), control split (`mgmp_split.h`), session/reconnect (`mgmp_session.*`), save transfer (`mgmp_savefile.*`), cat/inventory/run-history sync (`mgmp_catsync.*`, `mgmp_invsync.*`, `mgmp_runhist.*`), `ByteStream` wrapper (`mgmp_bytestream.h`), event/level-up choice replication (`mgmp_choice.*`), map follow (`mgmp_follow.*`), node hash (`mgmp_nodehash.*`), host-left detection (`mgmp_leave.*`), combat UI lock (`mgmp_combatlock.*`), peer cursors & GL overlay (`mgmp_cursor.*`, `mgmp_overlay.*`), peer aim preview (`mgmp_aim.*`).
- `src/ui/` — ImGui debug panel (`mgmp_ui.*`, win32 + opengl3 backends).
- `loader/` — Injector (`mgmp_loader.cpp`), parks game at `FrameBegin` and injects before frame 1.
- `tests/` — Unit tests for pure logic + `test_resolve_live.cpp` (maps `Mewgenics.exe` with `LOAD_LIBRARY_AS_IMAGE_RESOURCE`).
- `tools/` — `net_test.ps1` (2-instance loopback test), `save_snapshot.ps1`, `decode_record.py` (reads `.mgr` v1–v6), `gpak.py` (reads `resources.gpak` `.gon` files), `verify_sigs.py`, `ida/recover_symbols.py`, `ida/gen_sigs.py`.

See `src/core/mgmp_addresses.h` for all hook/call RVAs, `src/core/mgmp_tuning.h` for compile-time flags, and `src/net/mgmp_proto.h` for wire messages.

---

## Current Status & Open Items

Battle lockstep, meta sync, choice replication, reconnect catch-up, peer cursors/aim previews, and the ImGui panel all work live.

**Open / Known Debt:**
1. **Post-battle hash gap:** Nothing hashes the window between a battle's last turn hash and the next `NODEHASH` (combat resolution, rewards, level-up). `EnterNode` wipes RNG drift on the next node, but cat state changes aren't caught there. Use `debug.record = true` + `decode_record.py --sim` to inspect.
2. **Post-reconnect turn hashes:** Catch-up replays `ACTION`s, but turn-hash agreement after a reconnect hasn't been exercised past turn 2.
3. **Detection debt:** Equipment and facing are not in `state_hash` (equipment is cross-checked by GON name on ability use; facing is traced in `delta:` and `STATEDUMP`). `STATEDUMP` is unit-tested but has never fired in a live desync.
4. **Validation debt:** `tune::kHookTimeDelay` (`TimeDelayStatusApplication`) has never run against real content (`AZ_LoseHead` in AstroZombie miniboss).
5. **Cursor camera:** Screen-space peer pointer (`nx, ny`) is resolution/letterbox-independent, not camera-independent (tile reticle stays accurate).
6. **Hardcoded SDL slots:** The 3 `SDL_DYNAPI` `.data` slot RVAs are still manually migrated on game updates (not signature-resolved).
7. **Reach:** Direct-IP TCP only (`SteamNetworkingMessages002` is linked/unused; no `SteamMatchmaking`). 3–4 player envelope/split is written but untested live.

---

## Binary & Tooling

- **Target binary:** `Mewgenics.exe` (x64 PE, imagebase `0x140000000`, `SizeOfImage 0x1574000`, linked 2026-09-05, SHA256 `4127cd6a792ae528bca6f65a8873dd61789591937d87656c2b586a5e30eb77ea`).
  - **All raw RVAs/addresses in this file are pinned to this build.** The mod resolves 54 targets by unique byte signature (`mgmp_sigs.generated.h`) and treats pinned RVAs as fast-path hints.
- **Tech stack:** Native C++ (`glaiel::`), MSVC x64 ABI, SDL3 (statically linked with `SDL_DYNAPI` shim), FAudio, GON (`GonObject`), custom Flash SWF/AVM2 (`glaiel::swf::*`), SQLite (`SQLSaveFile`) + `ByteStream` + LZ4, `steam_api64.dll` (no `SteamMatchmaking`).
- **`resources.gpak` (~5 GB):** Streams on demand (`GPak::LoadIndex` reads only the index at startup; data section follows the full index). Contains 447 `.gon` files — use `tools/gpak.py` to check whether any game content actually reaches a code path.
- **Python:** Use `python` (3.13), not `python3`.

### Repo Conventions & Game Update Migration
- **`src/core/mgmp_sigs.generated.h` MUST be committed** (regenerating requires `.i64`, which clones don't have).
- **Vendored `third_party/`** must be committed as files (no nested `.git`). `.gitignore` uses anchored `/build/` and `!third_party/**`.
- **No hardcoded local paths** in `tools/` scripts (use `argv[1]`, `MEWGENICS_EXE` / `MGMP_ROOT`, or derived defaults).
- **After a game update:**
  1. Open new `Mewgenics.exe.i64` in IDA → run `tools/ida/recover_symbols.py` (restores `glaiel::` names from asserts & `_Func_impl_no_alloc` vftables; treat recovered names as navigational leads since MSVC inlining/outlining can misattribute).
  2. Manually update the 3 `SDL_DYNAPI` slot RVAs by decoding `jmp cs:off_...` from each SDL thunk.
  3. Run `tools/ida/gen_sigs.py` (reads RVAs from `mgmp_addresses.h`, outputs `mgmp_sigs.generated.h`), then `python tools/verify_sigs.py` against `Mewgenics.exe`.
  4. **Dead end:** Vtable-slot name propagation does not work (MSVC ICF folds empty virtuals into one function across ~1600 slots).
- **Operational traps (`tools/net_test.ps1`):**
  - `net_test.ps1` derives `build_host/` and `build_client/` configs from `build\Release\mgmp.json` (not `build\mgmp.json`).
  - Both instances share `%APPDATA%` save directory — use `save_snapshot.ps1 -Snapshot` before testing.
  - PowerShell `= if (...)` enumerates single-element arrays into scalars; assign from statements instead.
  - **Pause → Abandon Run** during the tutorial calls `MewDirector::SkipTutorial` (`0x1403AE940`).

---

## Battle Layer & Determinism

- **Frame driver:** `ApplicationBase::FrameBegin` / `FrameEnd` (`SDL_GetPerformanceCounter`). No fixed-timestep accumulator.
- **Injection point:** Cannot inject at PE entry point (`DLL_THREAD_ATTACH` faults before startup). Loader parks at `FrameBegin` and injects before frame 1.
- **Character list (roster):**
  ```
  TurnControl+0x18 -> +0x08 -> +0x20 -> +0x1F90
      { u32 refcount@0; u32 pad@4; u32 cap@8; u32 count@12; Character** data@16 }
  ```
  Order is deterministic spawn order (turn order re-shuffles every round via inlined Fisher-Yates `sub_140085F80`). Mod snapshots this once per battle because summons append and deaths remove. Summons are not in the snapshot and run local AI — a summon with `PlayerBrain` would be an unhandled double-control deadlock.
  Detect roster changes by pointer **membership**, never list length (a summon + death in the same turn leaves length unchanged).

### Key `Character` / `TacticsObject` Offsets
| Field | Offset | Notes |
|---|---|---|
| HP | `Character+0x4B0` (`i32`) | Zeroed together with shield as qword in `Die` |
| Shield | `Character+0x4B4` (`i32`) | |
| Max HP | `Character+0x4BC` (`i32`) | |
| Dead | `Character+0x4C2` (`bool`) | |
| Facing | `Character+0x388` (`iVec2D`) | Read by backstab test (`Character+0xC80`); excluded from `state_hash` because `CombatAnimation::update` writes it asynchronously |
| Link | `Character+0x60` ⇄ `TacticsObject+0x98` | Used to validate pointer sanity |
| Tile | `TacticsObject+0x48` (`iVec2D`) | `+0x50` holds previous tile |
| Passives | `Character+0xE70` | `{u32 cap, u32 count, Passive** data}` — **lazily rebuilt cache** (dirty `+0xE50`, guard `+0xEB4`), **do NOT hash** |
| `CatStats` | `CatData+0x00..+0x18` | 7 `i32`s (`str, dex, con, int, spd, cha, lck`) |

- **Off-board sentinel `(-5000, -5000)`:** Both departed cats and dying cats still in the live list can sit at `(-5000, -5000)`. `Character::get_affecting_elements` (`0x14011A840`) feeds this tile into an unbounded grid walk (`sub_14082CB30`), reading garbage/faulting. Always gate `plausible()` and `read_cat_state()` on `off_board()`.
- **Facing & Aim Preview:** `Brain::UpdateDecision` (`0x1401374D0`) calls `Character::Face` under a wall-clock gate (`Brain+0x2A8`). `T_UpdateDecision` snapshots/restores `Character+0x388` around it.

### `TurnAction` & Abilities
- **`sizeof(TurnAction) == 0x88`**, passed by value (MSVC x64 hidden `sret` pointer in `rdx` for `Brain::GetChoice` / `Brain::UpdateDecision`; `rcx = this`, returns `rax`).
  - `+0x00` `u32` type (`1` poll/none, `2` use ability, `3` end turn, `6` reaction broadcast, `7` invoke `std::function`)
  - `+0x04` **uninitialized padding** — skip in wire/hash
  - `+0x08` `Ability*`, `+0x10` target `iVec2D`, `+0x18` dir `iVec2D`, `+0x20` `Character*` actor (null from `GetChoice`, filled by `DoAction`), `+0x30/+0x31` `u8`, `+0x78` `std::function<void()>`.
- **Types 6 & 7:** Produced only by passive/status reactions (never brains); wire only ever carries types `2` and `3`.
- **Queue (`TurnControl`):** `+0x48` ring buffer, `+0x50` cap, `+0x58` head, `+0x60` count, `+0x90` front slot.
- **Ability slots (`Character`):** `+0xD0` `move`, `+0xD8` `attack`, `+0xE0` `bonus`, `+0xE8/+0xEC/+0xF0` `{cap, count, data}` spell vector (`weapon`/`trinket` are flagged abilities inside this vector at `Ability+0x8A1`/`+0x8A2`). Authored GON name is at `(Ability+0x28)+0x88` (`std::string`). Resolve by `(slot_kind, slot_index)`, cross-check by GON name.

### RNG Streams & Fences
- **Entry points:** `randint` (`0x14094B0B0`), `randfloat` (`0x140158B80`), `rand2` (`0x14094B230`), `RollChance` (`0x14094B550`), `splitmix64` (`0x14094ABB0`). CRT `rand()` is unused.
- **`RollChance(p, scale, state)`:** Short-circuits `p >= 1.0` without drawing — so peers must have identical `.gon` data (`HELLO` checks `resources.gpak` size + first 1 MiB).
- **TLS streams:** `TLS+0x178` = simulation, `TLS+0x198` = presentation (adjacent 32-byte xoshiro256 states). Note: functions often stage `178h` into a register before adding `[gs:58h][0]`.
- **Re-seeding:** `MapScreen::EnterNode` (`0x140391050`) unconditionally copies `MapNode+0x118` (32 bytes) into `TLS+0x178` (caveat: holding `SDLK_LSHIFT` takes a separate tail-call branch). `MewSaveFile::Load_0` (`0x14022F5F0`) saves/restores `TLS+0x178` around lazy cat loads (`by_id`). `Character::receive_damage_passives` (`0x14010E600`) gates proc rolls on `bool speculative`.
- **Wall-clock timer:** `TimeDelayStatusApplication` (vtable `0x140F229E0` slot 11 @ `0x14024F1C0`) is the only dt-driven combat countdown (`+0xF8` seconds).

---

## Meta Layer, Saves & UI

- **`SerializeCatData` (`0x14022E9A0`) & `RunHistory::serialize` (`0x1408DD2F0`):** Bidirectional, branching on `ByteStream+0x00`: **`0` = READ**, `1` = write, `2` = file.
- **`ByteStream`:** `+0x20` (`u8` `owns_read_buf`) must be `0` when lending mod-allocated memory; `+0x30` embeds `std::ofstream` (construct with `0x14032D0A0` before `~ByteStream` @ `0x1409B3130`, never zero-init).
- **`CatData`:** Persistent `u64` id at `+0x00`, resolved via `by_id(registry, id)` (`0x1400D6980`).
- **`Inventory` (`qword_1413BE2A0`, `MewDirector+1416`):** Buckets at `+56/+120/+184`; coins/food/boxes at `+328/+332/+336`. `Equipment` mints process-local id `++qword_1413BD998` on load — never key wire messages on item ids; push whole `Inventory` via `Store`/`Load` hooks.
- **`MewDirector` (`qword_1413D1970`):** `+56` `MewSaveFile`, `+1416` `Inventory*`, `+1424` `RunHistory*` (used-event list at `+96`), `+1432` cat registry, `+1464..+1472` `{cap, count, data}` run cat ids, `+1640` weather list.
  - **`save_adventure` (`0x1403B9CE0`) is node-boundary only:** Calling mid-node wedges the save. `MewDirector` exists from startup, so guard on `savefile_adventure_is_loaded()` (cat-id list non-empty) and not on a menu scene.
- **Choice replication:** `WorldEvent` (`+224/+232` options, 240-byte stride, `+296` subject `CatData*`, `+0x1A10` event name) and `LevelUpScreen` (`+160` subject `CatData*`, `+864/+872` options, 240-byte stride). All rolling `WorldEvent::spin` effect handlers use `TLS+0x178`; level-up roller (`sub_14037F860`, `HEAP` stream `+0xB8`) is seeded from `CatData+0x00` + `CatData+0xC30` jumps.
- **Saves & Menus:** `SaveSelection+0x38` is `std::vector<std::string>` of slot names; `MewDirector::init(std::string)` takes filename by value (safe to redirect to `mgmp_coop.sav`). `MainMenu` has no `update` override — click `MainMenu_Button_Play` via `Button::Click(Button*, 0)` (`0x1409764B0`, callback `Button+240`, name `Button+504`) from `Button::update`.
- **Loaded scenes:** `Component+40` -> `Director*`; `Director+0/+8` `Scene**`; `Scene+1208` `std::string` name; `Scene+1243` `u8` marked-for-destruction. Confirmed scene keys: `House`, `MainMenu`, `SaveSelectionScreen`, `Cutscene`, `ClassChooser`, `ActSelection`, `StorageItems` (`MapScreen` and `TitleScreen` are NOT scene names). `House` loaded ⇔ not in adventure.
- **`MapScreen` / `MapNode`:** `MapScreen+0x78` `CustomVector<MapNode*>`; `MapScreen+0xA0` marker (`+0x50` current node, `+0x60` selected node). `MapNode+0x138` `MapNodeType` (`5` battle, `6` hard, `7` miniboss, `8` boss).
- **Dead ends (do not retry):**
  - `MewDirector::ContinueAdventure` (`0x1403BB120`) live-apply: unconditionally destroys `House` and frees the entire live cat registry.
  - `InventoryItemBox::click()` (`0x14034DF60`): takes no arguments, UI-state-coupled.
  - `House` choice replication: `house_state` has no `EnterNode` re-seed, so peers diverge permanently on the first `CatData::breed` / `House::EndDay` roll.

---

## Rendering, Cursors & SDL3 Shim

- **Surfaces:** Use `ImmediateModeGameUI` for tile/world-anchored pieces; use our GL 3.2 core overlay for screen-space UI (there is no `SDL_Renderer`). Do not construct `RendererIso` components.
- **`ImmediateModeGameUI`:** Getter `0x14013C570` (null off battle screen), `tile_piece` `0x14033FFD0` (`rdx` = lowercase id, `r9` = CamelCase SWF anim; `rgba` and `scale` pointers **must be 16-byte aligned** for `MOVAPS`; keep strings ≤15 chars as callee destroys them; `Renderer+0x60` is alpha).
- **`PlayerBrain` aim state (`sub_140775EB0`):** `+0x3D8` selected `Ability*`, `+0x358` target `iVec2D`, `+0x360` dir `iVec2D` (`Brain+0x220` is only the post-click cached decision). `sub_140138A10` (range highlight) mutates status via `sub_140151CE0` (swallowed when mirroring). Move attack-range preview displaces the cat via `TacticsObject::Move` + `Character::recompute_stats` (fenced).
- **`StatusMenu::update` (`0x140817320`):** `+72` tactics grid (`+184` width, `+188` height), `+124` hovered tile `iVec2D` (unaligned).
- **GL overlay & cursors:** Cached mouse at RVA `0x012F2E80` (`double[2]`). Derive 16:9 (`1280x720`) content rect mathematically from drawable size — **never** from `glGetIntegerv(GL_VIEWPORT)` (which switches between offscreen target and full window). Cursor textures in `textures/cursor/` are 128×128 padded; scale all states by `default`'s ink height (`106.0f`). Save/restore GL state and bind framebuffer `0` explicitly.
- **SDL3 `SDL_DYNAPI` shim:** Every `SDL_*` thunk jumps through a `.data` table overwritten at runtime by `SDL_InitDynamicAPI` (the static default stubs are identical and stop being called). Always read/write the `.data` slot RVA directly:
  - `SDL_GL_SwapWindow`: slot RVA `0x012E7650` (thunk `0x140BA5550`)
  - `SDL_GetWindowSize`: slot RVA `0x012E8170` (thunk `0x140BA6C90`)
  - `SDL_GetWindowSizeInPixels`: slot RVA `0x012E8178` (thunk `0x140BA6CA0`)
