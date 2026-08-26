# save_snapshot.ps1 -- snapshot and restore the campaign save around a capture.
#
# Run D needs the SAME battle captured more than once: a calm baseline and a
# frame-starved one. The replayer already removes the hands from the loop, but
# it cannot rewind the campaign -- a battle you have won is gone, and with it
# any chance of replaying it. So the save has to be snapshotted BEFORE the
# battle is entered, and restored between runs.
#
#   .\tools\save_snapshot.ps1 -Snapshot runE      # before entering the battle
#   .\tools\save_snapshot.ps1 -List
#   .\tools\save_snapshot.ps1 -Restore runE       # between every replay
#
# Restoring always snapshots what it is about to overwrite (as `_overwritten`)
# first. This file moves real campaign progress around; it must never be the
# reason some of it is gone.
[CmdletBinding(DefaultParameterSetName = 'List')]
param(
    [Parameter(ParameterSetName = 'Snap', Mandatory)][string]$Snapshot,
    [Parameter(ParameterSetName = 'Rest', Mandatory)][string]$Restore,
    [Parameter(ParameterSetName = 'List')][switch]$List,
    [string]$Save = "$env:APPDATA\Glaiel Games\Mewgenics\76561197960271872\saves\steamcampaign01.sav"
)

$ErrorActionPreference = 'Stop'
$store = Join-Path $PSScriptRoot '..\build\saves'
New-Item -ItemType Directory -Force -Path $store | Out-Null

# The game rewrites the save on exit, so a restore performed while it is running
# is undone the moment you quit -- silently, and the capture that follows would
# be of the wrong battle. Cheaper to refuse than to debug later.
function Assert-GameClosed {
    if (Get-Process -Name 'Mewgenics' -ErrorAction SilentlyContinue) {
        throw "Mewgenics is running. Close it first -- it rewrites the save on exit, which would undo this."
    }
}

switch ($PSCmdlet.ParameterSetName) {
    'Snap' {
        if (-not (Test-Path $Save)) { throw "No save at $Save" }
        $dst = Join-Path $store "$Snapshot.sav"
        Copy-Item $Save $dst -Force
        "snapshot -> $dst  ($((Get-Item $dst).Length) bytes)"
    }
    'Rest' {
        Assert-GameClosed
        $src = Join-Path $store "$Restore.sav"
        if (-not (Test-Path $src)) { throw "No snapshot named '$Restore' in $store" }
        if (Test-Path $Save) {
            Copy-Item $Save (Join-Path $store '_overwritten.sav') -Force
        }
        Copy-Item $src $Save -Force
        "restored '$Restore' -> $Save  (previous saved as _overwritten)"
    }
    default {
        $snaps = Get-ChildItem "$store\*.sav" -ErrorAction SilentlyContinue
        if (-not $snaps) { "no snapshots yet in $store"; break }
        $snaps | Sort-Object LastWriteTime -Descending |
            Format-Table @{n='name';e={$_.BaseName}}, Length, LastWriteTime -AutoSize
    }
}
