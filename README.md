# mgmp — mew genics multi player

an awesome and very cool mod that allows you to play with your friends

you can watch the gameplay [here](https://youtu.be/tgWJtkz0ujo)

currently it uses own loader (cause i wasn't into mewgenics modding)

## Status

desyncs are MOSTLY fixed but there still may occur some

and you can't play via steam (please contribute im poor)


## What you need

- **Windows** 64-bit
- **Mewgenics** `1.1.21239`

## Getting it running

### Prebuilt

download the latest one from [releases](https://github.com/SanTertrust/mgmp/releases)

### 1. Build

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

### 2. Point it at your game

Open `mgmp.json` and set `game` to wherever Mewgenics actually is:

```json
"game": "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Mewgenics\\Mewgenics.exe"
```

(Windows paths in JSON need doubled backslashes.)

### 3. Choose who hosts

change the `"role"` to `"host"` if you're the one who hosting:

```json
"net": { "role": "host", "addr": "0.0.0.0", "port": 27600 }
```

and change the `"role"` to `"client"` if you're connecting:

```json
"net": { "role": "client", "addr": "<the host's IP address (not the 0.0.0.0)>", "port": 27600 }
```

### 4. Play

now open the mewgenics using `mgmp_loader.exe` (it will open `"game"` in the config)

or you can attach into an already running proccess (no idea why would you need that)

```powershell
.\mgmp_loader.exe --attach <process id>
```

## While you are playing

### The debug panel

F1 shows and hides

### Seeing your FRIENDS

the other player cursor will appear in the battle so you can see what is your FRIEND doing

### Logs

every session writes a timestamped log next to the loader (`mgmp_host_20260826-143000.log` and similar)

report BOTH host logs AND other player logs

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

there's no mewgenics code and data. to use mgmp you HAVE to buy the game

Tyler Glaiel and Edmund McMillen clearly have nothing to do with this project

(but ​​AI certainly does)
