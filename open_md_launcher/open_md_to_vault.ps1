# open_md_to_vault.ps1 — .md double-click handler.
#
# Decides what to do based on where the source file already lives:
#
#   • If the source is ALREADY inside the main vault
#     (C:\Users\azt12\OneDrive\Documents\Obsidian Vault\…):
#       Just fire obsidian://open?vault=...&file=<vault-relative>
#       so Obsidian opens it in place. NO copy. NO TEMP. NO watcher.
#
#   • If the source is OUTSIDE the vault:
#       1. Copy → %TEMP% staging file.
#       2. Move staging file into the vault's TEMP subfolder.
#       3. Fire obsidian:// URL pointing at TEMP/<name>.
#       4. Wait for Obsidian's main process to exit (kernel event via
#          Process.WaitForExit — not polling). When it exits, MOVE the
#          possibly-edited temp file back over the original source.
#       5. Register HKCU\RunOnce as a belt-and-suspenders move-back.
#
# Earlier versions watched workspace.json for "tab no longer contains
# our file" — but Obsidian replaces the leaf's file when you click a
# different file in the sidebar, which fired move-back prematurely.
# Process-exit is the right trigger.
#
# Wrapped with ps2exe -noConsole -noOutput -noError so the .exe is
# fully silent on double-click (no console window, no popups).

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $Path
)

$ErrorActionPreference = 'Stop'

$VaultRoot      = 'C:\Users\azt12\OneDrive\Documents\Obsidian Vault'
$VaultName      = 'Obsidian Vault'
$VaultTempDir   = Join-Path -Path $VaultRoot -ChildPath 'TEMP'
$LogPath        = 'C:\Users\azt12\open_md_to_vault.log'

function Log($msg) {
    $ts = (Get-Date -Format 'HH:mm:ss.fff')
    Add-Content -LiteralPath $LogPath -Value ('[' + $ts + '] ' + $msg)
}

# Normalize a path to its canonical full form for prefix comparison.
# Resolves relative segments, but does NOT require the path to exist.
function Get-CanonicalPath([string]$p) {
    return [System.IO.Path]::GetFullPath($p).TrimEnd([char]92)
}

# True if $childPath is inside $parentDir (case-insensitive,
# backslash-boundary-aware so "Foo Backup" doesn't match "Foo").
function Test-PathIsInside([string]$childPath, [string]$parentDir) {
    $c = (Get-CanonicalPath $childPath).ToLowerInvariant()
    $p = (Get-CanonicalPath $parentDir).ToLowerInvariant() + [char]92
    return $c.StartsWith($p)
}

# Fire obsidian://open?vault=...&file=<rel> via ShellExecute.
function Open-InObsidian([string]$vaultRelPath) {
    $vaultEnc = [uri]::EscapeDataString($VaultName)
    $fileEnc  = [uri]::EscapeDataString($vaultRelPath)
    $url      = 'obsidian://open?vault=' + $vaultEnc + '&file=' + $fileEnc
    Log ('url=' + $url)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName        = $url
    $psi.UseShellExecute = $true
    [void][System.Diagnostics.Process]::Start($psi)
    Log ('ShellExecute URL fired')
}

try {
    Log ('=== start ===  Path=' + $Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        Log ('ERROR: source not found')
        exit 1
    }

    # ── Gate: is the source already inside the main vault? ──────────
    $srcFull   = Get-CanonicalPath $Path
    $vaultFull = Get-CanonicalPath $VaultRoot

    if (Test-PathIsInside -childPath $srcFull -parentDir $vaultFull) {
        # File lives in the vault already — just open it in place.
        # Compute vault-relative path with forward slashes for the URL
        # (obsidian:// expects /, not \).
        $rel = $srcFull.Substring($vaultFull.Length + 1).Replace([char]92, '/')
        Log ('source is INSIDE vault — relative=' + $rel + ' — opening in place, no copy')
        Open-InObsidian -vaultRelPath $rel
        Log ('=== end (in-place) ===')
        exit 0
    }

    Log ('source is OUTSIDE vault — proceeding with copy → TEMP → watch flow')

    # ── Outside-vault flow ──────────────────────────────────────────

    # Ensure the destination subfolder exists.
    if (-not (Test-Path -LiteralPath $VaultTempDir)) {
        New-Item -ItemType Directory -Path $VaultTempDir -Force | Out-Null
        Log ('created vault TEMP subfolder')
    }

    $leaf      = Split-Path -Leaf $Path
    $leafNoExt = [System.IO.Path]::GetFileNameWithoutExtension($leaf)
    $ext       = [System.IO.Path]::GetExtension($leaf)
    if ([string]::IsNullOrEmpty($ext)) { $ext = '.md' }

    # Pick the final filename, deduping on collision.
    $finalName = $leaf
    $finalPath = Join-Path -Path $VaultTempDir -ChildPath $finalName
    $n = 2
    while (Test-Path -LiteralPath $finalPath) {
        $finalName = ('{0} ({1}){2}' -f $leafNoExt, $n, $ext)
        $finalPath = Join-Path -Path $VaultTempDir -ChildPath $finalName
        $n++
    }
    Log ('final vault path=' + $finalPath)

    # Stage 1: copy to local %TEMP%.
    $tempStage = Join-Path -Path $env:TEMP -ChildPath ('obsmd_' + [guid]::NewGuid().Guid + $ext)
    Copy-Item -LiteralPath $Path -Destination $tempStage -Force
    Log ('copied to staging=' + $tempStage)

    # Stage 2: move staging file into the vault.
    Move-Item -LiteralPath $tempStage -Destination $finalPath -Force
    Log ('moved to vault')

    # Belt-and-suspenders RunOnce cleanup. If we get killed before the
    # watcher fires, this runs at next login and moves the (possibly
    # edited) temp file back over the original. `move /Y` overwrites.
    $runOnceKey  = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
    $runOnceName = 'ObsTempMoveBack_' + ([guid]::NewGuid().Guid.Substring(0,8))
    $runOnceCmd  = 'cmd /c move /Y "' + $finalPath + '" "' + $Path + '"'
    try {
        if (-not (Test-Path -LiteralPath $runOnceKey)) { New-Item -Path $runOnceKey -Force | Out-Null }
        New-ItemProperty -LiteralPath $runOnceKey -Name $runOnceName -Value $runOnceCmd -PropertyType String -Force | Out-Null
        Log ('RunOnce fallback registered as ' + $runOnceName)
    } catch {
        Log ('RunOnce registration failed: ' + $_.Exception.Message)
    }

    # Stage 3: fire the obsidian:// URL.
    $fileRel = 'TEMP/' + $finalName
    Open-InObsidian -vaultRelPath $fileRel

    # Stage 4: wait for Obsidian's main process to exit, then move
    # back. The discovery loop polls briefly (every 250 ms for up to
    # 15 s) so we can latch onto the right PID once Obsidian launches
    # in response to our URL — there's no event-driven way to learn
    # the PID of a process you didn't spawn. Once we have the handle,
    # Process.WaitForExit() blocks on a kernel event, no polling.
    $obs = $null
    $deadline = (Get-Date).AddSeconds(15)
    while (((Get-Date) -lt $deadline) -and -not $obs) {
        Start-Sleep -Milliseconds 250
        # Pick the main Obsidian process (one with a visible window).
        # Electron spawns multiple Obsidian.exe helpers; only the main
        # one has MainWindowHandle != 0.
        $obs = Get-Process Obsidian -ErrorAction SilentlyContinue |
               Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } |
               Select-Object -First 1
    }

    if (-not $obs) {
        Log ('no main Obsidian process found within 15s — exiting without move-back (file stays in TEMP; RunOnce will move-back at next login)')
        exit 0
    }

    Log ('waiting for Obsidian PID ' + $obs.Id + ' to exit')
    $obs.WaitForExit()
    Log ('Obsidian exited — moving file back')

    # If the temp file was deleted out from under us (user manually
    # removed it from the vault before closing Obsidian), there's
    # nothing to move back.
    if (-not (Test-Path -LiteralPath $finalPath)) {
        Log ('temp file already gone — nothing to move back')
    } else {
        try {
            Move-Item -LiteralPath $finalPath -Destination $Path -Force -ErrorAction Stop
            Log ('moved back to ' + $Path)
        } catch {
            Log ('move-back failed: ' + $_.Exception.Message + ' — leaving temp in vault')
        }
    }

    # Remove our RunOnce entry on graceful exit so cleanup doesn't
    # also fire at next login.
    try {
        Remove-ItemProperty -LiteralPath $runOnceKey -Name $runOnceName -ErrorAction SilentlyContinue
        Log ('RunOnce fallback removed')
    } catch {}

    Log ('=== end ===')
    exit 0
}
catch {
    Log ('EXCEPTION: ' + $_.Exception.Message)
    exit 2
}
