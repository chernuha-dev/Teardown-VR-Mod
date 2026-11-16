param([string]$BuildDirectory = 'build\native')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) {
    $cmakePath = $cmake.Source
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio 2022 C++ Build Tools and CMake.' }
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmakePath = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path -LiteralPath $cmakePath)) { throw 'Install the Visual Studio C++ CMake tools component.' }
}
$buildCandidate = if ([IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $repoRoot $BuildDirectory }
$buildDir = [IO.Path]::GetFullPath($buildCandidate)
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build')) + [IO.Path]::DirectorySeparatorChar
if (-not $buildDir.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDirectory must be a subdirectory of this repository build/.'
}
& $cmakePath -S $repoRoot -B $buildDir -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& $cmakePath --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
$ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
& $ctestPath --test-dir $buildDir -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Native checks failed.' }
