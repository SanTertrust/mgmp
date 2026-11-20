# Mewgenics — Multiplayer Mod (`mgmp`) Guide

**Goal:** Shared-run co-op multiplayer for Mewgenics (2–4 players share one adventure run, split its cats, and play together).  
**Scope:** **Adventure only** (map → node → battle / shop / event / level-up → repeat). The **House** (breeding, furniture, `HouseInventory`) is out of scope.

## Sync Architecture

| Layer | Serializable? | Deterministic? | Mechanism |
|---|---|---|---|
| **Meta** (map, shop, level-up, inventory) | Yes (`SerializeCatData`, `Inventory`, `RunHistory`, `SQLSaveFile`) | Partial (same save + `EnterNode` seed) | **Host-authoritative state sync + choice replication** |
| **Battle** | No (no `Character`/`Status`/`TacticsGrid` serializer) | Yes (`TLS+0x178` sim stream) | **Deterministic lockstep over TCP** |

**Key files:**
- `src/core/mgmp_addresses.h` — all hook/call RVAs, struct offsets, and `SDL_DYNAPI` `.data` slot RVAs.
- `src/core/mgmp_sigs.generated.h` — generated unique byte signatures for `mgmp_resolve.cpp` (50 targets: 47 code, 3 data).
- `src/core/mgmp_tuning.h` — compile-time switches.
- `src/net/mgmp_proto.h` — wire protocol messages.

---

## Current Status & Open Items

Battle lockstep, meta sync, choice replication, reconnect catch-up, peer cursors/aim previews, and the ImGui panel work live.

**Open / Known Debt:**
1. **Post-battle hash gap:** Nothing hashes the window between a battle's last turn hash and the next `NODEHASH` (combat resolution, rewards, level-up). `EnterNode` wipes RNG drift on the next node, but cat state changes aren't caught there.
2. **Post-reconnect turn hashes:** Catch-up replays `ACTION`s, but turn-hash agreement after a reconnect hasn't been exercised past turn 2.
3. **Detection debt:** Equipment and facing are not in `state_hash` (equipment is cross-checked by GON name on ability use; facing is synced on `endturn` and traced in `delta:`/`STATEDUMP`).
4. **Validation debt:** `tune::kHookTimeDelay` (`TimeDelayStatusApplication`) has never run against real content (`AZ_LoseHead` in AstroZombie miniboss).
5. **Cursor camera:** Screen-space peer pointer (`nx, ny`) is resolution/letterbox-independent, not camera-independent (tile reticle stays accurate).
6. **Hardcoded SDL slots:** The 3 `SDL_DYNAPI` `.data` slot RVAs in `mgmp_addresses.h` are still manually migrated on game updates (not signature-resolved).
7. **Reach:** Direct-IP TCP only (`SteamNetworkingMessages002` is linked/unused; no `SteamMatchmaking`). 3–4 player envelope/split is written but untested live.

---

## Binary, Repo Conventions & Tooling

- **Target stack:** `Mewgenics.exe` (x64 PE, `glaiel::`), MSVC x64 ABI, SDL3 (static with `SDL_DYNAPI` shim), FAudio, GON (`GonObject`), custom Flash SWF/AVM2, SQLite (`SQLSaveFile`) + `ByteStream` + LZ4, `steam_api64.dll`.
- **`resources.gpak` (~5 GB):** Streams on demand (`GPak::LoadIndex`). Use `python tools/gpak.py <path-to-resources.gpak> <filter>` to inspect `.gon` files.
- **Python:** Use `python` (3.13), not `python3`.
- **`src/core/mgmp_sigs.generated.h` MUST be committed** (regenerating requires `.i64`, which clones don't have).
- **Vendored `third_party/`** must be committed as files (no nested `.git`). `.gitignore` uses anchored `/build/` and `!third_party/**`.
- **No hardcoded local paths** in `tools/` scripts (use `argv[1]`, `MEWGENICS_EXE` / `MGMP_ROOT`, or derived defaults).
- **After a game update:**
  1. Open new `Mewgenics.exe.i64` in IDA → run `tools/ida/recover_symbols.py` (restores `glaiel::` names from asserts & `_Func_impl_no_alloc` vftables; treat recovered names as navigational leads since MSVC inlining/outlining can misattribute).
  2. Manually update the 3 `SDL_DYNAPI` `.data` slot RVAs in `mgmp_addresses.h` by decoding `jmp cs:off_...` from each SDL thunk.
  3. Update any shifted RVAs in `mgmp_addresses.h`, run `tools/ida/gen_sigs.py` (writes `mgmp_sigs.generated.h`), then `python tools/verify_sigs.py <Mewgenics.exe>`.
  4. **Dead end:** Vtable-slot name propagation does not work (MSVC ICF folds empty virtuals into one function across ~1600 slots).
- **Operational traps (`tools/net_test.ps1`):**
  - `net_test.ps1` derives `build_host/` and `build_client/` configs from `build\Release\mgmp.json` (not `build\mgmp.json`).
  - Both instances share `%APPDATA%` save directory — use `save_snapshot.ps1 -Snapshot` before testing.
  - PowerShell `= if (...)` enumerates single-element arrays into scalars; assign from statements instead.
  - **Pause → Abandon Run** during the tutorial calls `MewDirector::SkipTutorial`.

---

## Hard-Won Invariants & Traps

### Battle Layer
- **Injection point:** Cannot inject at PE entry point (`DLL_THREAD_ATTACH` faults before startup). `mgmp_loader` parks at `FrameBegin` and injects before frame 1.
- **Roster snapshot:** Turn order re-shuffles every round (Fisher-Yates). Snapshot once per battle; summons append and run local AI (a summon with `PlayerBrain` would deadlock). Detect roster changes by pointer **membership**, never list length.
- **Off-board sentinel `(-5000, -5000)`:** Departed/dying cats still in the live list can sit at `(-5000, -5000)`. `Character::get_affecting_elements` feeds this into an unbounded grid walk and faults. Always gate `plausible()` and `read_cat_state()` on `off_board()`.
- **Do NOT hash `Character+0xE70` (Passives):** Lazily rebuilt cache (`+0xE50` dirty, `+0xEB4` guard).
- **Facing (`Character+0x388`):** Read by backstab test and AI targeting. Excluded from `state_hash` because `CombatAnimation::update` writes it asynchronously, and `Brain::UpdateDecision` writes it under a wall-clock gate (`T_UpdateDecision` restores it). `PlayerBrain::update` also lets the active player click-to-turn (`og_orientation` at `+0x41C`) without a `TurnAction`, so `endturn` carries final facing in `ActionMsg::dx, dy`.
- **`TurnAction` (`0x88` bytes):** `+0x04` is uninitialized padding (skip in wire/hash). Types `6` and `7` are generated locally by reactions (never sent). Resolve abilities by `(slot_kind, slot_index)` and cross-check by GON name.
- **RNG (`TLS+0x178` sim, `TLS+0x198` presentation):** `RollChance(p, ...)` short-circuits `p >= 1.0` without drawing, so `HELLO` hashes `resources.gpak` size + first 1 MiB. `MapScreen::EnterNode` re-seeds `TLS+0x178` from `MapNode+0x118` (holding `SDLK_LSHIFT` takes a separate branch).

### Meta Layer, Rendering & SDL3
- **`ByteStream`:** `+0x00` `0 = READ`, `1 = write`, `2 = file`. Set `+0x20` (`owns_read_buf`) to `0` when lending mod memory; construct embedded `std::ofstream` before `~ByteStream`, never zero-init.
- **Equipment IDs:** Minted process-locally on load — never key wire messages on item ids; push whole `Inventory`.
- **`save_adventure` is node-boundary only:** Calling mid-node wedges the save. Guard on `savefile_adventure_is_loaded()` and not on a menu scene (`MapScreen` and `TitleScreen` are NOT scene names; `House` loaded ⇔ not in adventure).
- **Dead ends (do not retry):**
  - `MewDirector::ContinueAdventure` live-apply: unconditionally destroys `House` and frees the entire live cat registry.
  - `InventoryItemBox::click()`: takes no arguments, UI-state-coupled.
  - `House` choice replication: `house_state` has no `EnterNode` re-seed, so peers diverge permanently on the first `CatData::breed` / `House::EndDay` roll.
- **Rendering & `ImmediateModeGameUI`:** Use `ImmediateModeGameUI` for tile pieces (`rgba` and `scale` pointers **must be 16-byte aligned** for `MOVAPS`; keep strings ≤15 chars), and the GL 3.2 core overlay for screen-space UI (derive 16:9 rect mathematically from drawable size — never from `glGetIntegerv(GL_VIEWPORT)`; bind FBO `0` explicitly).
- **`SDL_DYNAPI` shim:** Every `SDL_*` thunk jumps through a `.data` slot overwritten at runtime by `SDL_InitDynamicAPI` — always hook the `.data` slot RVA, not the static stub.
