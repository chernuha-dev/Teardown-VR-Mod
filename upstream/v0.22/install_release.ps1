# Teardown VR Mod - installer for the release zip (Install.bat runs this; tools\release\ in the source repository).
# Copies dxgi.dll (the native part, next to this script) into the Teardown game folder and the TeardownVR folder (the
# Lua part) to Documents\Teardown\mods\TeardownVR.
# Usage: double-click Install.bat, or
#        powershell -NoProfile -ExecutionPolicy Bypass -File install_release.ps1 [-GameDir "<path to Teardown>"] [-NoPrompt]
param([string]$GameDir = "", [switch]$NoPrompt)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# This release as numbers (major, minor) - the same as install.ps1's $ModVersion (test\test_install_release.ps1 checks).
# v0.21 (docs/V021_PLAN.md section 15): 0.21 - a 0.20 settings file is backed up; the TeardownVR folder gains
# hands_blocky.vox. v0.22 (docs/V022_PLAN.md section 9): 0.22 - a 0.21 settings file is backed up (new options
# hand_punch, hand_steady, scope_zoom); the TeardownVR folder's file list is unchanged.
$ModVersion = @(0, 22)

# v0.21: the files the TeardownVR folder of this release holds (the same list as install.ps1's Get-ModFileNames;
# test\test_install_release.ps1 checks that the two agree and that tools\make_release.ps1 packs each of them).
function Get-ModFileNames {
    return @("main.lua", "info.txt", "hands_glove.vox", "hands_skin.vox", "hands_blocky.vox")
}

# The names of Get-ModFileNames missing in $srcDir (an empty array when the folder is complete).
function Get-MissingModFiles([string]$srcDir) {
    $missing = @()
    foreach ($f in (Get-ModFileNames)) {
        if (-not (Test-Path -LiteralPath (Join-Path $srcDir $f) -PathType Leaf)) { $missing += $f }
    }
    return $missing   # unrolled: callers wrap it in @() or read .Count ($null / one name / names)
}

# The marker string every Teardown VR dxgi.dll carries (src\vr_main.cpp g_tdvrMarker: "TDVR_PROXY_MARKER Teardown VR
# <version>"). Any other dxgi.dll (ReShade, another mod) is never treated as ours.
function Test-IsOurDll([string]$path) {
    if (-not $path -or -not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    try { $bytes = [IO.File]::ReadAllBytes($path) } catch { return $false }
    $text = [Text.Encoding]::ASCII.GetString($bytes)
    return $text.Contains("TDVR_PROXY_MARKER Teardown VR")
}

# (major, minor) from the first line of a settings file ("# Teardown VR 0.20.0 - settings"), $null without such a header.
function Get-IniVersion([string]$firstLine) {
    if ($firstLine -match '^\uFEFF?# Teardown VR (\d{1,4})\.(\d{1,4})') { return , @([int]$Matches[1], [int]$Matches[2]) }
    return $null
}

# -1 / 0 / 1 as version a is older than / equal to / newer than version b (integers, major first: 0.10 > 0.9).
function Compare-ModVersion([int[]]$a, [int[]]$b) {
    if ($a[0] -ne $b[0]) { if ($a[0] -lt $b[0]) { return -1 } else { return 1 } }
    if ($a[1] -ne $b[1]) { if ($a[1] -lt $b[1]) { return -1 } else { return 1 } }
    return 0
}

# A settings file without a version header or from an older version is backed up (the mod writes a fresh one).
function Test-IniNeedsBackup([string]$firstLine, [int[]]$current) {
    $v = Get-IniVersion $firstLine
    if ($null -eq $v) { return $true }
    return (Compare-ModVersion $v $current) -lt 0
}

# True when $dir is a Teardown game folder (has teardown.exe).
function Test-TeardownDir([string]$dir) {
    if (-not $dir) { return $false }
    return Test-Path -LiteralPath (Join-Path $dir "teardown.exe") -PathType Leaf
}

# The Steam library folders listed in <steam>\steamapps\libraryfolders.vdf: the "path" entries (current format) or the
# numbered entries of the old format ("1" "D:\\SteamLibrary"); only absolute paths, \\ unescaped.
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

# The Steam install folders this PC names: the current user's SteamPath, the machine-wide InstallPath, the default.
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

# The Teardown folder (<library>\steamapps\common\Teardown with teardown.exe) in the given Steam folders' libraries
# (default: Get-SteamRoots), $null when none has it.
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

# v0.20: earlier versions copied the game's own tool models into the mod folder as tool_<name>.vox; they are the game's
# files and no longer used (the mod spawns the game's files directly). Returns the number deleted.
function Remove-OldToolVox([string]$modDir) {
    $n = 0
    if (-not (Test-Path -LiteralPath $modDir)) { return 0 }
    foreach ($f in (Get-ChildItem -LiteralPath $modDir -Filter "tool_*.vox" -File -ErrorAction SilentlyContinue)) {
        try { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction Stop; $n++ } catch { }
    }
    return $n
}

# True when an error (an ErrorRecord or an exception) is Windows refusing access: a Teardown folder under a protected
# folder (for example C:\Program Files\SteamLibrary) needs the installer run as administrator.
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

# The native part into $gameDir: somebody else's dxgi.dll (ReShade etc.) is never overwritten without a backup.
# Returns the backup's path ($null when nothing was backed up); throws when Windows does not allow the writes.
function Install-NativePart([string]$gameDir, [string]$srcDll) {
    $target = Join-Path $gameDir "dxgi.dll"
    $bak = $null
    if ((Test-Path -LiteralPath $target) -and -not (Test-IsOurDll $target)) {
        $bak = Join-Path $gameDir "dxgi.dll.before_teardownvr"
        Copy-Item -LiteralPath $target -Destination $bak -Force -ErrorAction Stop
    }
    Copy-Item -LiteralPath $srcDll -Destination $target -Force -ErrorAction Stop
    try { Unblock-File -LiteralPath $target -ErrorAction Stop } catch { }
    return $bak
}

# A settings file from an older version is moved to teardown_vr.ini.<version>.bak (the mod writes a fresh fully
# commented one at the next start). Returns the backup's path ($null when kept); throws when Windows does not allow it.
function Move-OldSettings([string]$gameDir, [int[]]$current) {
    $ini = Join-Path $gameDir "teardown_vr.ini"
    if (-not (Test-Path -LiteralPath $ini)) { return $null }
    $first = [string](Get-Content -LiteralPath $ini -TotalCount 1)
    if (-not (Test-IniNeedsBackup $first $current)) { return $null }
    $ver = "old"
    $v = Get-IniVersion $first
    if ($null -ne $v) { $ver = "v{0}.{1}" -f $v[0], $v[1] }
    $bakIni = Join-Path $gameDir "teardown_vr.ini.$ver.bak"
    Move-Item -LiteralPath $ini -Destination $bakIni -Force -ErrorAction Stop
    return $bakIni
}

function Stop-Install([string]$message) {
    Write-Host $message -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------------------------------------------------
$version = "{0}.{1}" -f $ModVersion[0], $ModVersion[1]
Write-Host "Teardown VR Mod $version - installer"
Write-Host ""

$srcDll = Join-Path $here "dxgi.dll"
$srcMod = Join-Path $here "TeardownVR"
if (-not (Test-Path -LiteralPath $srcDll -PathType Leaf) -or (Get-MissingModFiles $srcMod).Count -gt 0) {
    Stop-Install "dxgi.dll or the TeardownVR folder is missing next to this script. Extract the whole zip first, then run Install.bat from the extracted folder."
}

if ($GameDir -eq "") { $GameDir = Find-Teardown }
if (-not (Test-TeardownDir $GameDir)) {
    Write-Host "Could not find Teardown automatically." -ForegroundColor Yellow
    if (-not $NoPrompt -and [Environment]::UserInteractive) {
        $GameDir = (Read-Host "Paste the Teardown folder (the one with teardown.exe), then press Enter").Trim().Trim('"')
    }
    if (-not (Test-TeardownDir $GameDir)) {
        Stop-Install "Teardown not found. Run again with: powershell -ExecutionPolicy Bypass -File install_release.ps1 -GameDir `"C:\path\to\Teardown`""
    }
}
Write-Host "Teardown folder: $GameDir"

if (Get-Process -Name teardown -ErrorAction SilentlyContinue) {
    Stop-Install "Teardown is running - close it first, then run the installer again."
}

# The native part (never overwriting somebody else's dxgi.dll without a backup) and an older settings file's backup.
# A Teardown folder under a protected folder (C:\Program Files\...) refuses the writes unless run as administrator.
$target = Join-Path $GameDir "dxgi.dll"
try {
    $bak = Install-NativePart $GameDir $srcDll
    if ($bak) { Write-Host "An existing dxgi.dll (not Teardown VR's) was backed up to $bak" -ForegroundColor Yellow }
    Write-Host "Copied dxgi.dll to $target"

    $bakIni = Move-OldSettings $GameDir $ModVersion
    if ($bakIni) {
        Write-Host "Old settings moved to $bakIni (a new file with all $version options is written at the next start;" -ForegroundColor Yellow
        Write-Host "copy over any value you had changed)" -ForegroundColor Yellow
    }
} catch {
    if (Test-AccessDenied $_) {
        Stop-Install "Windows did not allow writing to $GameDir. Close this window, right-click Install.bat and choose 'Run as administrator'."
    }
    Stop-Install "Could not write to $GameDir ($($_.Exception.Message)). Is Teardown still running?"
}

# The Lua part.
$modDir = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Teardown\mods\TeardownVR"
New-Item -ItemType Directory -Force -Path $modDir | Out-Null
Get-ChildItem -LiteralPath $srcMod | Copy-Item -Destination $modDir -Recurse -Force
Get-ChildItem -LiteralPath $modDir -File -Recurse | ForEach-Object { try { Unblock-File -LiteralPath $_.FullName -ErrorAction Stop } catch { } }
Write-Host "Copied the Lua mod to $modDir"
$removed = Remove-OldToolVox $modDir
if ($removed -gt 0) {
    Write-Host "Removed $removed old tool model copies (tool_*.vox) left by an earlier version - the mod now uses the game's own files"
}

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host "Next step: start Teardown, open Mods > Local files and enable 'Teardown VR Mod'." -ForegroundColor Green
Write-Host "Then see README.md for the one-time game settings and how to start VR."
Write-Host "Settings file (created on first start): $GameDir\teardown_vr.ini"
Write-Host "Log file: $GameDir\teardown_vr.log"
exit 0
