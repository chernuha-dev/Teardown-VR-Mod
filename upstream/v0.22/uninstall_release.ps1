# Teardown VR Mod - uninstaller for the release zip (Uninstall.bat runs this; tools\release\ in the source repository).
# Removes dxgi.dll from the Teardown folder ONLY when it is Teardown VR's (restoring a dxgi.dll the installer backed up)
# and the Lua mod folder Documents\Teardown\mods\TeardownVR. The settings and log files are kept unless -All.
# Usage: double-click Uninstall.bat, or
#        powershell -NoProfile -ExecutionPolicy Bypass -File uninstall_release.ps1 [-GameDir "<path>"] [-All] [-NoPrompt]
param([string]$GameDir = "", [switch]$All, [switch]$NoPrompt)

$ErrorActionPreference = "Stop"

# The marker string every Teardown VR dxgi.dll carries (src\vr_main.cpp g_tdvrMarker: "TDVR_PROXY_MARKER Teardown VR
# <version>"). Any other dxgi.dll (ReShade, another mod) is never removed.
function Test-IsOurDll([string]$path) {
    if (-not $path -or -not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    try { $bytes = [IO.File]::ReadAllBytes($path) } catch { return $false }
    $text = [Text.Encoding]::ASCII.GetString($bytes)
    return $text.Contains("TDVR_PROXY_MARKER Teardown VR")
}

# True when $dir is a Teardown game folder (has teardown.exe).
function Test-TeardownDir([string]$dir) {
    if (-not $dir) { return $false }
    return Test-Path -LiteralPath (Join-Path $dir "teardown.exe") -PathType Leaf
}

# The Steam library folders listed in <steam>\steamapps\libraryfolders.vdf (as install_release.ps1).
function Get-SteamLibraries([string]$steamRoot) {
    $libs = @()
    if (-not $steamRoot) { return , $libs }
    $libs += $steamRoot
    $vdf = Join-Path $steamRoot "steamapps\libraryfolders.vdf"
    if (Test-Path -LiteralPath $vdf -PathType Leaf) {
        foreach ($line in (Get-Content -LiteralPath $vdf -ErrorAction SilentlyContinue)) {
            if ($line -match '^\s*"(?:path|\d+)"\s+"(.+)"\s*$') {
                $p = $Matches[1] -replace '\\\\', '\'
                if ($p -match '^[A-Za-z]:[\\/]' -or $p -match '^\\\\') { $libs += $p }
            }
        }
    }
    return , $libs
}

function Get-SteamRoots {
    $roots = @()
    foreach ($k in @(@("HKCU:\Software\Valve\Steam", "SteamPath"), @("HKLM:\SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
                     @("HKLM:\SOFTWARE\Valve\Steam", "InstallPath"))) {
        $v = (Get-ItemProperty -Path $k[0] -ErrorAction SilentlyContinue).($k[1])
        if ($v) { $roots += ($v -replace '/', '\') }
    }
    if (${env:ProgramFiles(x86)}) { $roots += (Join-Path ${env:ProgramFiles(x86)} "Steam") }
    return , $roots
}

function Find-Teardown([string[]]$steamRoots = $null) {
    if ($null -eq $steamRoots) { $steamRoots = Get-SteamRoots }
    $seen = @{}
    foreach ($root in $steamRoots) {
        foreach ($lib in (Get-SteamLibraries $root)) {
            $key = $lib.TrimEnd('\', '/').ToLowerInvariant()
            if ($seen.ContainsKey($key)) { continue }
            $seen[$key] = $true
            $p = Join-Path $lib "steamapps\common\Teardown"
            if (Test-TeardownDir $p) { return $p }
        }
    }
    return $null
}

# Removes $dir\dxgi.dll when it is ours and puts back dxgi.dll.before_teardownvr (the installer's backup of a dxgi.dll
# that was there before). Returns "removed", "foreign" (somebody else's dxgi.dll: untouched) or "none".
function Remove-OurDll([string]$dir) {
    $target = Join-Path $dir "dxgi.dll"
    $result = "none"
    if (Test-Path -LiteralPath $target) {
        if (Test-IsOurDll $target) {
            # (a read-only / hidden attribute is cleared first: Remove-Item -Force would turn an access-denied error
            # into an ArgumentException that does not say so)
            $item = Get-Item -LiteralPath $target -Force
            $sticky = [IO.FileAttributes]::ReadOnly -bor [IO.FileAttributes]::Hidden -bor [IO.FileAttributes]::System
            if ($item.Attributes -band $sticky) { $item.Attributes = [IO.FileAttributes]::Normal }
            Remove-Item -LiteralPath $target -ErrorAction Stop
            $result = "removed"
        } else {
            return "foreign"
        }
    }
    $bak = Join-Path $dir "dxgi.dll.before_teardownvr"
    if ((Test-Path -LiteralPath $bak) -and -not (Test-Path -LiteralPath $target)) {
        Move-Item -LiteralPath $bak -Destination $target -ErrorAction Stop
    }
    return $result
}

# True when an error (an ErrorRecord or an exception) is Windows refusing access: a Teardown folder under a protected
# folder (for example C:\Program Files\SteamLibrary) needs the uninstaller run as administrator.
function Test-AccessDenied($err) {
    if ($null -eq $err) { return $false }
    if ($err -is [System.Management.Automation.ErrorRecord]) {
        if ($err.CategoryInfo.Category -eq [System.Management.Automation.ErrorCategory]::PermissionDenied) { return $true }
        $err = $err.Exception
    }
    for ($e = $err; $null -ne $e; $e = $e.InnerException) {
        if ($e -is [UnauthorizedAccessException]) { return $true }
    }
    return $false
}

function Stop-Uninstall([string]$message) {
    Write-Host $message -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------------------------------------------------
Write-Host "Teardown VR Mod - uninstaller"
Write-Host ""

if ($GameDir -eq "") { $GameDir = Find-Teardown }
if (-not (Test-TeardownDir $GameDir)) {
    Write-Host "Could not find Teardown automatically." -ForegroundColor Yellow
    if (-not $NoPrompt -and [Environment]::UserInteractive) {
        $GameDir = (Read-Host "Paste the Teardown folder (the one with teardown.exe), then press Enter").Trim().Trim('"')
    }
    if (-not (Test-TeardownDir $GameDir)) {
        Stop-Uninstall "Teardown not found. Run again with: powershell -ExecutionPolicy Bypass -File uninstall_release.ps1 -GameDir `"C:\path\to\Teardown`""
    }
}
Write-Host "Teardown folder: $GameDir"
if (Get-Process -Name teardown -ErrorAction SilentlyContinue) { Stop-Uninstall "Teardown is running - close it first." }

$hadBackup = Test-Path -LiteralPath (Join-Path $GameDir "dxgi.dll.before_teardownvr")
try {
    $dllResult = Remove-OurDll $GameDir
} catch {
    if (Test-AccessDenied $_) {
        Stop-Uninstall "Windows did not allow changing $GameDir. Close this window, right-click Uninstall.bat and choose 'Run as administrator'."
    }
    Stop-Uninstall "Could not remove dxgi.dll from $GameDir ($($_.Exception.Message)). Is Teardown still running?"
}
switch ($dllResult) {
    "removed" { Write-Host "Removed Teardown VR's dxgi.dll from $GameDir" }
    "foreign" { Write-Host "$GameDir\dxgi.dll is not Teardown VR's - left untouched." -ForegroundColor Yellow }
    default   { Write-Host "No dxgi.dll in $GameDir" }
}
if ($hadBackup -and -not (Test-Path -LiteralPath (Join-Path $GameDir "dxgi.dll.before_teardownvr"))) {
    Write-Host "Restored the dxgi.dll that was there before Teardown VR"
}

$modDir = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Teardown\mods\TeardownVR"
if (Test-Path -LiteralPath $modDir) { Remove-Item -LiteralPath $modDir -Recurse -Force; Write-Host "Removed $modDir" }

if ($All) {
    foreach ($f in @("teardown_vr.ini", "teardown_vr.log", "teardown_vr_crash.log")) {
        $p = Join-Path $GameDir $f
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; Write-Host "Removed $p" }
    }
} else {
    Write-Host "Kept the settings and log files (teardown_vr.ini / teardown_vr.log in the game folder; -All removes them)."
}
Write-Host ""
Write-Host "Teardown VR Mod uninstalled." -ForegroundColor Green
Write-Host "If you subscribed to it on the Steam Workshop, also unsubscribe there."
# While VR runs the mod sets the game's head bob to 0 and tool sway off, and puts them back when VR stops. If the game
# was closed while VR was running, that put-back may not have happened.
Write-Host "Note: if Options > Input shows Head bob at 0 or Tool sway off, set them back to your own values." -ForegroundColor Yellow
exit 0
