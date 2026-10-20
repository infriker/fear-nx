[CmdletBinding()]
param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ConfigPath,
    [string[]]$RegistryRoots = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)
if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $ConfigPath = Join-Path $RepoRoot 'release\fear-nx.toml'
}
$ConfigPath = [System.IO.Path]::GetFullPath($ConfigPath)
if ($RegistryRoots.Count -eq 0) {
    $RegistryRoots = @(
        (Join-Path $RepoRoot 'src'),
        (Join-Path $RepoRoot 'third_party\rexglue-sdk\src')
    )
}

if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) {
    throw "Config file not found: $ConfigPath"
}
foreach ($root in $RegistryRoots) {
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        throw "Cvar source directory not found: $root"
    }
}

$registered = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
$definePattern = 'REXCVAR_DEFINE_[A-Z0-9_]+\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,'
$sourceFiles = foreach ($root in $RegistryRoots) {
    Get-ChildItem -LiteralPath $root -Recurse -File |
        Where-Object { $_.Extension -in @('.c', '.cc', '.cpp', '.h', '.hpp') }
}
foreach ($sourceFile in $sourceFiles) {
    $source = [System.IO.File]::ReadAllText($sourceFile.FullName)
    foreach ($match in [System.Text.RegularExpressions.Regex]::Matches($source, $definePattern)) {
        [void]$registered.Add($match.Groups[1].Value)
    }
}
if ($registered.Count -eq 0) {
    throw 'No REXCVAR_DEFINE_* registrations were found in the supplied source roots.'
}

$section = ''
$mapped = 0
$lineNumber = 0
foreach ($line in [System.IO.File]::ReadLines($ConfigPath)) {
    $lineNumber++
    $trimmed = $line.Trim()
    if (-not $trimmed -or $trimmed.StartsWith('#')) { continue }

    if ($trimmed -match '^\[(?<table>[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)*)\]$') {
        $section = $Matches.table.Replace('.', '_')
        continue
    }

    if ($trimmed -notmatch '^(?<key>[A-Za-z0-9_-]+)\s*=\s*\S') {
        throw "Unsupported profile syntax at $($ConfigPath):$($lineNumber): $trimmed"
    }

    $key = $Matches.key
    $fullName = if ($section) { "${section}_$key" } else { $key }
    if (-not $registered.Contains($fullName)) {
        throw "Config key '$fullName' at $($ConfigPath):$($lineNumber) is not registered by a REXCVAR_DEFINE_* in the supplied sources."
    }
    Write-Host "OK: $trimmed -> $fullName"
    $mapped++
}

if ($mapped -eq 0) {
    throw "No cvar assignments were found in $ConfigPath"
}
Write-Host "Validated $mapped config cvar key(s) against $($registered.Count) source registration(s)."
