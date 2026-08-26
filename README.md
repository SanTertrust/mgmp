# mgmp — co-op multiplayer for Mewgenics

An unofficial mod that lets two people play one Mewgenics adventure together.

The host starts a run, the other player joins, and from then on you share
everything: the same map, the same cats, the same gold, the same fights. Your
cats are split between you — you each take turns with your own half — and the
run continues until one of you wins it or loses it. Together.

It is not an official feature and has nothing to do with the developers of
Mewgenics. It is a hobby project built by reading the game's compiled code.

Thanks [Claude](https://claude.ai/). The human didn't write even a single line of code.

---

## Status

**It works, and it has been played over the internet for a full run.**

There is still a probability of encountering desynchronization, so report host logs AND clients logs.

What that means in practice:

- Battles are properly synchronised. Both players see the same dice rolls, the
  same damage numbers, the same enemy decisions.
- The map, shops, level-ups, events and your inventory all stay in step.
- You can see each other's mouse cursor, and see what your partner is aiming at
  before they commit to it.
- If someone's connection drops, they can rejoin — even in the middle of a
  fight — and the mod replays the battle up to the current turn to catch them
  up.

**Still rough:**

- **Connecting is manual.** There is no lobby or friends list. One of you needs
  to be reachable on the internet, which usually means forwarding a port on
  your router. See [Connecting](#connecting-to-each-other).
- **Two players only, really.** The code was written with three and four
  players in mind, but that has never actually been tested. Assume two.
- **The house is not shared.** Breeding, furniture and everything you do at
  home is yours alone. Only the adventure is co-op. This was a deliberate
  decision, not an oversight. So, make sure to join the host **while they are in the Adventure mode**.
- **It is pinned to one version of the game.** The mod tries hard to survive a
  game update on its own, but a big patch may still break it until someone
  updates it. See [When the game updates](#when-the-game-updates).
- **Expect bugs.** This is a reverse-engineered mod hooking into a game that
  was never designed to be played this way.

---

## What you need

- **Windows**, 64-bit.
- **Mewgenics**, the same version on both machines, installed from the same
  place. If one of you has a different build of the game, the mod will notice
  and complain — that mismatch would eventually cause the two games to drift
  apart. Currently supported `1.1 Build 21039`.
- A way to reach each other over the network (see below).

To build it yourself you also need **Visual Studio 2022/2026** (or just the MSVC
build tools) and **CMake 3.21+**.

---

## Getting it running

### Prebuilt

The easiest way. Download the archive from the [Releases](https://github.com/SanTertrust/mgmp/releases).

### 1. Build

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

That produces two files in `build\Release\`:

| file | what it is |
|---|---|
| `mgmp_loader.exe` | what you run. Starts the game with the mod loaded. |
| `mgmp.dll` | the mod itself. The loader puts it into the game. |
| `mgmp.json` | your settings. Created automatically the first time. |

The three need to sit in the same folder. Everything else — Dear ImGui,
MinHook — is compiled straight into `mgmp.dll`, so there is nothing else to
install or copy around.

### 2. Point it at your game

Open `mgmp.json` and set `game` to wherever Mewgenics actually is:

```json
"game": "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Mewgenics\\Mewgenics.exe"
```

(Windows paths in JSON need doubled backslashes.)

### 3. Choose who hosts

The **host** owns the run. Their save file is the one you both play.

In the host's `mgmp.json`:

```json
"net": { "role": "host", "addr": "0.0.0.0", "port": 27600 }
```

In the other player's:

```json
"net": { "role": "client", "addr": "<the host's IP address>", "port": 27600 }
```

Leave `role` as `"off"` to play alone with the mod loaded but dormant.

### 4. Play

Both of you run `mgmp_loader.exe`. It will open the Mewgenics based on config.

You can also just **drag and drop** `Mewgenics.exe` into `mgmp_loader.exe`.

I'd recommend launch it from a command line/powershell, because it logs stuff, such as needing the `steam_appid.txt` file near the `Mewgenics.exe`.

The host picks a save slot and starts playing. Clients connected to the host cannot select save or start battle in Adventure mode, nor can they perform other actions that alter cat data.

From there you just play. When it is your cat's turn, you move it. When it is
your partner's cat, you watch. Cats controlled by the computer are handled by
both machines identically and need no input from anyone.

If you would rather attach the mod to a copy of the game that is already
running:

```powershell
.\mgmp_loader.exe --attach <process id>
```

---

## Connecting to each other

The mod talks over a plain TCP connection on port **27600** by default. There
is no matchmaking server, no relay, and no Steam lobby — the joining player
connects straight to the host's IP address.

That means one of two things:

- **On the same network** (same house, same Wi-Fi), it just works. The joining
  player uses the host's local IP, something like `192.168.1.42`.
- **Over the internet**, the host has to be reachable. In practice that means
  forwarding TCP port 27600 on their router to their PC, and the joining player
  using the host's public IP.

If port forwarding is not an option, any tool that puts you both on one virtual
network (Hamachi, Tailscale, ZeroTier, Radmin) works fine — from the mod's
point of view that is just a LAN.

There are **no timeouts anywhere**, on purpose. If your partner's game is slow
to respond, yours waits instead of guessing. A game that visibly pauses is much
better than two games quietly playing different stories.

---

## While you are playing

### The debug panel

Press **F1** to show or hide an overlay panel. It reports whether you are
connected, which cats each player controls, how many turns have been agreed on,
and a live log. If something looks wrong, this is the first place to look.

You can change the key, or turn the panel off entirely, in `mgmp.json` under
`ui`.

### Seeing your partner

Their mouse cursor appears on your screen, and when they are choosing where to
attack, you see the same targeting highlight they do. None of this affects the
game — it is purely so you can tell what they are thinking.

One quirk: the cursor follows the board tile they are pointing at, not the
exact pixel, if the two of you have panned the camera differently. The tile is
always right.

### If the two games disagree

Every turn, both machines compare notes on the state of the battle. If they
ever come out different, the mod **stops both games immediately** and writes
what differed to the log, rather than letting you play on in two increasingly
different worlds.

If that happens, the log files are the interesting part — see below.

### Logs

Every session writes a timestamped log next to the loader
(`mgmp_host_20260826-143000.log` and similar). If you want to report a problem,
that file from *both* players is what makes it diagnosable. A single side's log
can usually only show that something went wrong, not what.

---

## Settings

Everything lives in `mgmp.json`, beside the loader.

| setting | what it does |
|---|---|
| `game` | full path to `Mewgenics.exe` |
| `log` | log file name |
| `net.role` | `off`, `host`, or `client` |
| `net.addr` | who to connect to (client), or what to listen on (host) |
| `net.port` | default `27600` |
| `net.control` | which cats you control. Leave on `"auto"` — it splits them evenly by itself. |
| `ui.enabled` | whether the debug panel exists at all |
| `ui.visible` | whether it starts visible |
| `ui.key` | key to toggle it, default `F1` |

The `debug` block is for development and is best left alone. `record` in
particular makes the mod log an enormous amount of detail about the game's
random number generator, which is useful when chasing a bug and pure overhead
otherwise.

Only settings that are genuinely meant to be changed live in this file.
Everything else is a compile-time constant in `src/core/mgmp_tuning.h`, each
one with a note saying why it is not user-facing.

---

## How it works, briefly

Two players staying in sync is not one problem, it is two, and they need
opposite solutions.

**Battles are deterministic.** Given the same starting position, the game plays
out identically every time, right down to the individual dice rolls — this was
measured, not assumed. So the mod does not send the *result* of anything.
It sends only what each player clicked, and both machines work out the
consequences independently and arrive at the same place. This is called
lockstep, and it is why battles cost almost no bandwidth.

**Everything outside battle is not deterministic**, but it is saveable. The
game already knows how to write a cat, an inventory and a run's history to
disk. So for the map, shops, level-ups and events, the host simply decides and
sends the result over.

Both halves are checked constantly. Every turn of every battle, and every time
you enter a map node, the two machines exchange a fingerprint of their state
and compare it. Agreement is proven, not hoped for.

The full technical story — every address, struct offset, measurement and dead
end — is in `CLAUDE.md`. It is long, and it is the real documentation.

---

## Project layout

```
src/core/          loading, hooking, logging, config, crash reporting
src/determinism/   random number tracking, recording and replay
src/net/           the network transport and the message format
src/session/       everything that actually keeps two games in step
src/ui/            the debug panel and the on-screen overlay
loader/            mgmp_loader.exe
tests/             unit tests for the logic that can be tested alone
tools/             Python and PowerShell helpers used while developing
third_party/       Dear ImGui, MinHook, stb_image, nlohmann/json
```

Run the tests with:

```powershell
ctest --test-dir build --build-config Release
```

---

## A note on what this is

This mod contains no Mewgenics code, art or data. It is an independent program
that attaches to a legally purchased copy of the game on your own machine. You
need to own the game; nothing here will help you play it if you do not.

It is not affiliated with, endorsed by, or supported by the developers or
publisher of Mewgenics. If it breaks your save, that is on you — back up your
saves before using it. If you report a bug to the game's developers while this
mod is loaded, please mention that it is loaded.

Mewgenics is by Tyler Glaiel and Edmund McMillen. It is a very good game and
you should buy it.
