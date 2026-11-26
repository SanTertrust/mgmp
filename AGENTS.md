# Mewgenics Multiplayer Mod (`mgmp`)

Shared-run co-op multiplayer for Mewgenics (2–4 players, direct-IP TCP, injected DLL `mgmp.dll` + `mgmp_loader.exe`).

## Architecture

| Layer | Mechanism |
|---|---|
| **Battle** | Deterministic lockstep over TCP (`TLS+0x178` sim RNG, human inputs synced via `PlayerBrain::OnRequestAction`, AI & status overrides run locally) |
| **Adventure Meta** (map, shop, events, level-up, inventory) | Host-authoritative state sync (`CatData`, `Inventory`, `RunHistory`, `SQLSaveFile`) + choice replication |
| **House & Prep** | Host-authoritative entity/pose/drag sync + checkpoint reload (`mgmp_house.cpp`) |

## Project Layout

- `src/core/` — `mgmp_addresses.h` (all hook/call RVAs, struct offsets, `SDL_DYNAPI` `.data` slots), `mgmp_sigs.generated.h` (122 byte signatures: 117 code, 5 data), `mgmp_resolve.cpp`, `mgmp_hooks.cpp`, `mgmp_tuning.h`, `mgmp_config.*`, `mgmp_gpak.*`, `mgmp_log.*`, `mgmp_crash.*`.
- `src/determinism/` — `TurnAction` layout (`0x88` B), sim RNG stream (`TLS+0x178`), ability slot/GON resolution, `TimeDelayStatusApplication` turn conversion.
- `src/net/` — TCP transport (`mgmp_net.*`), wire protocol (`mgmp_proto.h`), consistent hash ring.
- `src/session/` — core gameplay sync:
  - Battle: `mgmp_lockstep.*`, `mgmp_barrier.*`, `mgmp_split.*`, `mgmp_draft.*`, `mgmp_aim.*`, `mgmp_combatlock.*`, `mgmp_cursor.*` (board reticles).
  - Meta: `mgmp_session.*`, `mgmp_follow.*`, `mgmp_choice.*`, `mgmp_catsync.*`, `mgmp_invsync.*`, `mgmp_runhist.*`, `mgmp_savefile.*`, `mgmp_nodehash.*`, `mgmp_leave.*`, `mgmp_bytestream.*`.
  - House & UI overlay: `mgmp_house.*`, `mgmp_overlay.*` (GL 3.2 core screen-space cursors/pings across all screens).
- `src/ui/` — ImGui panel (`mgmp_ui.*`) and game UI helpers (`mgmp_gameui.*`).
- `tools/` — `diff_desync.py`, `verify_sigs.py`, `gpak.py`, `net_test.ps1`, `save_snapshot.ps1`, `ida/gen_sigs.py`, `ida/recover_symbols.py`.

## Build, Verify & Debug Workflow

- **Build:** `cmake --build build --config Release` → outputs `build\Release\mgmp.dll` and `build\Release\mgmp_loader.exe`. Runtime trace logs are written to `build\Release\mgmp_trace_*.log`.
- **Desync triage:** `python tools/diff_desync.py <host_trace.log> <client_trace.log>` — pinpoints the first diverging turn hash (`rng`/`state`/`queue`) or `HALT`/`CRASH` and dumps paired context.
- **Verify signatures:** `python tools/verify_sigs.py "<path-to-Mewgenics.exe>"` — run whenever `mgmp_addresses.h` or `mgmp_sigs.generated.h` changes.
- **Inspect game data:** `python tools/gpak.py "<path-to-resources.gpak>" <filter>` to inspect `.gon` files inside the ~5 GB archive.
- **Local 2-instance test (`tools/net_test.ps1`):**
  - Derives `build_host/` and `build_client/` configs from `build\Release\mgmp.json` (not `build\mgmp.json`).
  - Both instances share `%APPDATA%` saves — run `save_snapshot.ps1 -Snapshot` first.

## Adding a Hook / Updating for a New Game Build

1. Open `Mewgenics.exe.i64` in IDA; on a new game build run `tools/ida/recover_symbols.py` first (treat recovered `glaiel::` names as leads — MSVC inlining/outlining can misattribute).
2. Update RVAs/offsets in `src/core/mgmp_addresses.h` (and on game updates, manually update the 3 `SDL_DYNAPI` `.data` slot RVAs by decoding `jmp cs:off_...` from each SDL thunk).
3. Run `tools/ida/gen_sigs.py` in IDA to regenerate `src/core/mgmp_sigs.generated.h`.
4. Verify with `python tools/verify_sigs.py "<Mewgenics.exe>"` and rebuild `Release`.

## Repo Rules & Non-Obvious Dead Ends

- **Use `python` (3.13), not `python3`.**
- **Commit `src/core/mgmp_sigs.generated.h`** (regenerating requires `.i64`, which clones don't have).
- **No hardcoded local paths** in `tools/` scripts (use `argv[1]`, `MEWGENICS_EXE` / `MGMP_ROOT`, or derived defaults).
- **Vendored `third_party/`** must be committed as files (no nested `.git`).
- **Dead ends (do not retry):**
  - Hooking static `SDL_*` stubs instead of `SDL_DYNAPI` `.data` slots (overwritten at runtime by `SDL_InitDynamicAPI`).
  - `MewDirector::ContinueAdventure` live-apply (unconditionally destroys `House` and frees the live cat registry).
  - Vtable-slot name propagation across game updates (MSVC ICF folds ~1600 empty virtuals into one function).
  - Detailed per-module invariants live in the module headers/comments (`mgmp_lockstep.cpp`, `mgmp_catsync.cpp`, `mgmp_house.cpp`, `mgmp_overlay.cpp`, `mgmp_cursor.cpp`) — read them before editing.
