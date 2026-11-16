$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$manifest = Get-Content -LiteralPath (Join-Path $repoRoot 'baseline.sha256.json') -Raw | ConvertFrom-Json
foreach ($entry in $manifest.files) {
    $path = Join-Path $repoRoot $entry.path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing baseline file: $($entry.path)" }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $entry.sha256) { throw "Baseline differs: $($entry.path). Expected after intentional development changes; review the diff." }
}
Write-Output "Baseline verified: $($manifest.files.Count) files."
