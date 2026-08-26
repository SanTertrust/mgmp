# net_test.ps1 -- drive the two-instance loopback lockstep test.
#
# Two Mewgenics instances on one box, host and client, talking over 127.0.0.1.
# CLAUDE.md used to rule this out on memory grounds; measured on 2026-08-23 it
# works, and it is a far better first milestone than a mock peer because both
# sides are the real game.
#
# The peers get their own directories (build_host, build_client) because the DLL
# reads mgmp.json from beside itself -- one shared build directory means one
# shared config, and the two peers need different roles and different cat lists.
#
#   .\tools\net_test.ps1 -Build                     build, deploy, launch both
#   .\tools\net_test.ps1                            deploy + launch (no rebuild)
#   .\tools\net_test.ps1 -Record                    also capture .mgr files
#   .\tools\net_test.ps1 -HostCats 19,20 -ClientCats 21,22   (manual override)
#   .\tools\net_test.ps1 -Summary                   read the newest log pair
#   .\tools\net_test.ps1 -Stop                      kill both instances
#
# THE CAT SPLIT IS AUTOMATIC by default (net_control = auto): each peer finds
# the human-brained cats in the battle's own roster and takes half, host first.
# Both peers derive it independently from a roster that is byte-identical by
# measurement, then exchange CONTROL messages and halt if the two halves do not
# add up -- which covers the failure that used to be invisible: a cat claimed by
# NEITHER peer, which simply never acts while both sides politely wait for it.
#
# -HostCats/-ClientCats still force explicit indices, which is what you want to
# put a specific cat on a specific side. They index the battle's character list
# and cannot be known before the battle exists: launch once, read the
# `LOCKSTEP battle roster:` block, take the PlayerBrain rows.
[CmdletBinding()]
param(
    [switch]$Build,
    [switch]$Stop,
    [switch]$Summary,
    # The RNG hooks are pure overhead unless a capture is being taken, so they
    # follow `record` and are OFF in a normal lockstep run -- which is why no
    # lockstep log so far can name the passive behind a proc. Turn this on when
    # the question is 'which RollChance fired', not 'did the peers agree'.
    [switch]$Record,
    # Stop the client following the host through the map screen, i.e. drive
    # both instances to the same battle by hand. That is how phase 4 was
    # tested, and it is still the way to tell a battle-layer problem from a
    # map-layer one.
    [switch]$NoFollow,
    # Share the host's save file with the client, and drive the client past its
    # save-selection screen. Off by default because the two instances share one
    # save directory, so this is a transfer between two files on the same disk
    # rather than between two machines -- it exercises the plumbing (publish,
    # frame, hash, write, redirect, auto-click), not the case where the two
    # peers really did start from different saves.
    #
    # It is non-destructive either way: the client's run is written to
    # saves\mgmp_coop.sav and the load is redirected there, so neither
    # instance's steamcampaignNN.sav is written by the mod.
    # Make the CLIENT lag every map node by this many milliseconds, so the host
    # walks into a battle alone and the client turns up late. That is the
    # late-join gap on demand -- the scenario net_join_barrier exists to survive,
    # and otherwise reproducible only by luck.
    #
    # Pair it with -NoBarrier to see the failure it prevents: without the
    # barrier the host plays turns by itself and the client halts on the first
    # hash that disagrees, its rng_hash byte-identical to its own previous turn.
    [int]   $LateClient = 0,
    # Turn the join barrier OFF on both peers. Only useful next to -LateClient,
    # as the control half of that experiment.
    [switch]$NoBarrier,
    # Do not halt on a hash mismatch -- report it and keep playing, on BOTH
    # peers. This is how you find out whether a state-only mismatch is a real
    # divergence or an effect that landed on opposite sides of a turn boundary:
    # a run that halts cannot show you that it re-converged, because it stopped.
    # Watch for the '^^ TRANSIENT' line. Nothing about such a run proves the
    # peers stayed in sync, which is why it is not the default.
    [switch]$NoHalt,
    # Empty = auto: derive the split from the roster. See the header.
    [int[]] $HostCats   = @(),
    [int[]] $ClientCats = @(),
    [int]   $Port       = 27600,
    [string]$Addr       = '127.0.0.1',
    # Seconds to let the host bind and start listening before the client dials.
    # The client does not retry: a refused connect is fatal to that run.
    [int]   $HostLead   = 14,
    # CMake build tree and config -Build drives, e.g. build\Release\mgmp.dll on
    # the default (multi-config, Visual Studio) generator.
    [string]$BuildDir   = 'build',
    [string]$Config     = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent      # ...\mod
$peers = @{ host = 'build_host'; client = 'build_client' }

# CMake's default generator here is multi-config (Visual Studio), which puts
# binaries under build\<Config>\; a single-config generator (Ninja, NMake)
# puts them straight under build\. Check both rather than assume one.
function Get-CmakeOutDir {
    foreach ($c in (Join-Path $root "$BuildDir\$Config"), (Join-Path $root $BuildDir)) {
        if (Test-Path (Join-Path $c 'mgmp_loader.exe')) { return $c }
    }
    throw "mgmp_loader.exe not found under '$BuildDir' -- build first: .\net_test.ps1 -Build"
}

function Stop-Peers {
    $p = Get-Process Mewgenics -ErrorAction SilentlyContinue
    if ($p) { $p | Stop-Process -Force; Write-Host "stopped $($p.Count) instance(s)" -ForegroundColor Yellow }
    else    { Write-Host 'no instances running' }
}

# Newest timestamped log for a peer. The plain mgmp_<peer>.log name is a
# pre-timestamp leftover and is deliberately NOT matched -- reading a stale log
# and believing it is this run's is the exact mistake the timestamps prevent.
function Get-PeerLog([string]$peer) {
    $dir = Join-Path $root $peers[$peer]
    Get-ChildItem (Join-Path $dir "mgmp_${peer}_2*.log") -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime | Select-Object -Last 1
}

function Show-Summary {
    foreach ($peer in 'host', 'client') {
        $log = Get-PeerLog $peer
        Write-Host "`n=== $peer ===" -ForegroundColor Cyan
        if (-not $log) { Write-Host '  (no timestamped log yet)' -ForegroundColor DarkGray; continue }
        Write-Host "  $($log.Name)" -ForegroundColor DarkGray

        $lines = Get-Content $log.FullName
        # The roster is long and identical on both peers; the counts are what
        # you actually compare. Print it only when it is missing or disagrees.
        $roster = $lines | Select-String -Pattern 'battle roster:'
        if ($roster) { Write-Host "  $($roster[-1].Line.Trim())" }

        $lines | Select-String -Pattern 'LOCKSTEP\s+(->|<-|turn \d+ (hash|AGREES)|done:|!!|state hash|control split)' |
            ForEach-Object { Write-Host "  $($_.Line.Trim())" }

        # Deliberately narrow. Matching the bare word "desync" also matches the
        # `done: ... 0 desync(s)` summary line, which then prints itself a
        # second time in red -- a clean run that looks alarming is worse than
        # no highlight at all.
        $halts = $lines | Select-String -Pattern '!! HALT|!! MISMATCH|peer halted|DIVERGED|[1-9]\d* desync'
        if ($halts) { $halts | ForEach-Object { Write-Host "  $($_.Line.Trim())" -ForegroundColor Red } }

        # The line -NoHalt exists to produce. Yellow rather than red: a mismatch
        # that re-converged is a finding, not a failure, and colouring it like a
        # failure is how it gets skimmed past.
        $trans = $lines | Select-String -Pattern '\^\^ TRANSIENT'
        if ($trans) { $trans | ForEach-Object { Write-Host "  $($_.Line.Trim())" -ForegroundColor Yellow } }
    }

    # The check that matters: same turn count, same hashes, nothing outstanding.
    $hl = Get-PeerLog 'host'; $cl = Get-PeerLog 'client'
    if ($hl -and $cl) {
        # BOTH halves, and they are counted separately on purpose. This used to
        # read only rng=, which means the desync class this whole tool exists to
        # find -- identical stream, divergent state -- was reported as "all N
        # turn hashes IDENTICAL". A verdict that cannot fail on the failure you
        # are hunting is worse than no verdict.
        $rx = 'turn (\d+) hash rng=(\w+) state=(\w+)'
        $hh = (Get-Content $hl.FullName | Select-String -Pattern $rx) |
              ForEach-Object { ,@($_.Matches[0].Groups[1].Value,
                                  $_.Matches[0].Groups[2].Value,
                                  $_.Matches[0].Groups[3].Value) }
        $ch = (Get-Content $cl.FullName | Select-String -Pattern $rx) |
              ForEach-Object { ,@($_.Matches[0].Groups[1].Value,
                                  $_.Matches[0].Groups[2].Value,
                                  $_.Matches[0].Groups[3].Value) }
        $common = [Math]::Min($hh.Count, $ch.Count)
        $badRng = 0; $badState = 0; $firstBad = -1
        for ($i = 0; $i -lt $common; $i++) {
            $r = ($hh[$i][0] -ne $ch[$i][0]) -or ($hh[$i][1] -ne $ch[$i][1])
            $s = ($hh[$i][2] -ne $ch[$i][2])
            if ($r) { $badRng++ }
            if ($s) { $badState++ }
            if (($r -or $s) -and $firstBad -lt 0) { $firstBad = $i }
        }
        Write-Host "`n=== verdict ===" -ForegroundColor Cyan
        Write-Host "  turns hashed: host $($hh.Count), client $($ch.Count), compared $common"
        if ($badRng -eq 0 -and $badState -eq 0 -and $common -gt 0) {
            Write-Host "  all $common turn hashes IDENTICAL (rng and state)" -ForegroundColor Green
        } elseif ($common -eq 0) {
            Write-Host '  nothing to compare -- did both peers reach a battle?' -ForegroundColor Yellow
        } else {
            Write-Host "  DIFFER: $badRng rng, $badState state, of $common compared (first at turn $($hh[$firstBad][0]))" -ForegroundColor Red
            if ($badRng -eq 0) {
                Write-Host '      state-only: the two streams took the same draws, so nothing' -ForegroundColor Yellow
                Write-Host '      ROLLED differently. Diff the two logs `delta:` lines around' -ForegroundColor Yellow
                Write-Host '      that turn (net_state_trace), then the cat tables.'           -ForegroundColor Yellow
            }
        }

        # Direction coverage. A run where only one peer sent proves only one
        # direction of the transport -- easy to celebrate by mistake.
        $hs = ([regex]::Matches((Get-Content $hl.FullName -Raw), 'LOCKSTEP\s+->')).Count
        $cs = ([regex]::Matches((Get-Content $cl.FullName -Raw), 'LOCKSTEP\s+->')).Count
        Write-Host "  actions sent: host $hs, client $cs"
        if ($hs -eq 0 -or $cs -eq 0) {
            Write-Host '  [!] only one direction was exercised -- act with BOTH peers'  -ForegroundColor Yellow
            Write-Host '      cats to cover the client->host path too.'                 -ForegroundColor Yellow
        }
    }
}

if ($Stop)    { Stop-Peers; return }
if ($Summary) { Show-Summary; return }

if ($HostCats.Count -and $ClientCats.Count -and ($HostCats | Where-Object { $ClientCats -contains $_ })) {
    throw "net_control lists overlap: $($HostCats -join ',') vs $($ClientCats -join ','). " +
          'Two players driving one cat desyncs on the first turn it acts.'
}

Stop-Peers
Start-Sleep -Seconds 2

if ($Build) {
    Write-Host '==> configuring (cmake)' -ForegroundColor Cyan
    $buildPath = Join-Path $root $BuildDir
    & cmake -S $root -B $buildPath -A x64 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with exit code $LASTEXITCODE" }

    Write-Host '==> building (cmake --build)' -ForegroundColor Cyan
    & cmake --build $buildPath --config $Config | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed with exit code $LASTEXITCODE" }
}

$outDir = Get-CmakeOutDir

# Deploy. Each peer's config is derived from the build tree's mgmp.json so the
# game path stays in one place; only the per-peer keys are rewritten.
#
# This used to be fifteen regex substitutions against the raw ini text, each
# with a matching "...and append the key if it was not there", because a line
# that does not exist cannot be replaced. Against an object none of that is
# needed: a key that is absent is simply assigned.
$baseConfig = Get-Content (Join-Path $outDir 'mgmp.json') -Raw
foreach ($peer in 'host', 'client') {
    $dir = Join-Path $root $peers[$peer]
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item (Join-Path $outDir 'mgmp.dll'), (Join-Path $outDir 'mgmp_loader.exe') $dir -Force

    $list = if ($peer -eq 'host') { $HostCats } else { $ClientCats }
    $cats = if ($list.Count -eq 0) { 'auto' } else { $list -join ',' }

    # Parsed fresh per peer: ConvertFrom-Json returns a mutable object graph and
    # the two peers must not share one.
    $cfg = $baseConfig | ConvertFrom-Json

    $cfg.log = "mgmp_$peer.log"

    $cfg.net.role = $peer
    $cfg.net.addr = $Addr
    $cfg.net.port = $Port
    # "auto" is a string and an explicit split is an array of numbers -- the two
    # legal shapes of net.control, which the parser tells apart by type.
    #
    # Assigned from a statement rather than from `= if (...) {...} else {...}`:
    # PowerShell ENUMERATES the output of an if-expression, so a single-element
    # list came out as the bare number 0 and serialized as `"control": 0`. The
    # DLL then refused it as neither "auto" nor a list and fell back to auto --
    # which is to say `-HostCats 0` would have silently not been applied.
    if ($list.Count -eq 0) { $cfg.net.control = 'auto' }
    else                   { $cfg.net.control = [int[]]$list }

    # The delay is client-only: the host never follows, so setting it there would
    # be a no-op that quietly makes the log confusing.
    $delay = if ($peer -eq 'client') { $LateClient } else { 0 }
    $cfg.debug.follow          = -not $NoFollow
    $cfg.debug.follow_delay_ms = $delay
    $cfg.debug.join_barrier    = -not $NoBarrier
    # Both peers, always the same value: one peer that halts takes the run --
    # and the evidence -- with it, which is the whole thing -NoHalt is trying to
    # keep alive.
    $cfg.debug.desync_halt     = -not $NoHalt
    $cfg.debug.record          = [bool]$Record
    $cfg.debug.record_note     = "lockstep $peer, cats [$cats]"
    # Lockstep owns Brain::GetChoice. Leaving a replay file configured would put
    # two injectors on one buffer, and lockstep wins -- so the replay would
    # silently never advance.
    $cfg.debug.replay          = ''

    # Save sharing is not optional and has no knob any more -- it is how the
    # client acquires the run. It used to be gated by a switch that defaulted
    # OFF, which meant a plain -Build run could never reach a shared battle and
    # nothing said why.
    #
    # Both instances also share ONE save directory: the game resolves it through
    # SDL_GetPrefPath -> SHGetFolderPathW(CSIDL_APPDATA), which no environment
    # variable can redirect.
    $cfg | ConvertTo-Json -Depth 6 |
        Set-Content (Join-Path $dir 'mgmp.json') -Encoding UTF8
    $note = "cats [$cats]"
    if ($delay)    { $note += ", follow delayed ${delay}ms" }
    if ($NoBarrier){ $note += ", JOIN BARRIER OFF" }
    if ($NoHalt)   { $note += ", DESYNC HALT OFF" }
    Write-Host "  $peer -> $note" -ForegroundColor DarkGray
}

if ($LateClient -and -not $NoBarrier) {
    Write-Host "==> late-join test: client lags each node by ${LateClient}ms, barrier ON." -ForegroundColor Yellow
    Write-Host "    EXPECT: host logs 'join barrier: holding every decision', then" -ForegroundColor DarkGray
    Write-Host "    'join barrier OPEN ... after N poll(s)', then matching turn hashes." -ForegroundColor DarkGray
} elseif ($LateClient -and $NoBarrier) {
    Write-Host "==> late-join CONTROL run: barrier OFF. This is expected to DESYNC." -ForegroundColor Yellow
    Write-Host "    EXPECT: host plays turns alone; client halts on a hash mismatch with" -ForegroundColor DarkGray
    Write-Host "    its rng_hash equal to its own previous turn (the stream never ran)." -ForegroundColor DarkGray
}

Write-Host "==> launching host (listening on $Port)" -ForegroundColor Cyan
$hd = Join-Path $root $peers['host']
Start-Process -FilePath (Join-Path $hd 'mgmp_loader.exe') -WorkingDirectory $hd
Start-Sleep -Seconds $HostLead

Write-Host "==> launching client (dialling ${Addr}:${Port})" -ForegroundColor Cyan
$cd = Join-Path $root $peers['client']
Start-Process -FilePath (Join-Path $cd 'mgmp_loader.exe') -WorkingDirectory $cd
Start-Sleep -Seconds 10

Write-Host ''
Get-Process Mewgenics -ErrorAction SilentlyContinue |
    Format-Table Id, StartTime, Responding -AutoSize

foreach ($peer in 'host', 'client') {
    $log = Get-PeerLog $peer
    Write-Host "=== $peer handshake ===" -ForegroundColor Cyan
    if ($log) {
        Get-Content $log.FullName | Select-String -Pattern 'SESSION|NET ' |
            ForEach-Object { Write-Host "  $($_.Line.Trim())" }
    } else { Write-Host '  (no log yet)' -ForegroundColor DarkGray }
}

Write-Host @"

Both instances are up. Now:
  1. $(if ($NoFollow) { 'Drive BOTH windows to the same battle by hand (net_follow = 0).' } else { 'Drive the HOST only. The client follows it into every map node it enters,' })
     $(if ($NoFollow) { '' } else { 'and its own map clicks are swallowed -- the host owns the run.' })
  2. Cat split: host [$(if ($HostCats.Count) { $HostCats -join ',' } else { 'auto: first half' })];
     client [$(if ($ClientCats.Count) { $ClientCats -join ',' } else { 'auto: second half' })].
     Each peer prints its roster and the agreed split at the first turn boundary.
  3. In the battle, act with BOTH peers so each direction is exercised.
  4. Then:  .\tools\net_test.ps1 -Summary
"@ -ForegroundColor Green
