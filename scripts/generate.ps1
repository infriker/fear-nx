[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GameXex,
    [Parameter(Mandatory = $true)]
    [string]$ReXGlueExe,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$ExpectedHash = 'e7fccdf6dd57bb58d10efbdd0b2ca567b8cece5b6a3fc696c7fca879aaea2792'
$CanonicalManifest = Join-Path $RepoRoot 'fear_xbox360_manifest.toml'
$LocalManifest = Join-Path $RepoRoot 'fear_xbox360_manifest.local.toml'
$GameDestination = Join-Path $RepoRoot 'assets/default.xex'
$GeneratedSources = Join-Path $RepoRoot 'generated/default/sources.cmake'

if (-not (Test-Path -LiteralPath $GameXex -PathType Leaf)) {
    throw "XEX file not found: $GameXex"
}
if (-not (Test-Path -LiteralPath $CanonicalManifest -PathType Leaf)) {
    throw "Manifest not found: $CanonicalManifest"
}
if (-not (Test-Path -LiteralPath $ReXGlueExe -PathType Leaf)) {
    throw "ReXGlue executable not found: $ReXGlueExe"
}

$GameXex = [IO.Path]::GetFullPath($GameXex)
$ReXGlueExe = [IO.Path]::GetFullPath($ReXGlueExe)
$ActualHash = (Get-FileHash -LiteralPath $GameXex -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ActualHash -ne $ExpectedHash) {
    throw "Unsupported default.xex SHA-256: $ActualHash. This project is pinned to $ExpectedHash; no game file was copied."
}

$Version = (& $ReXGlueExe --version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $Version -ne '0.10.0') {
    throw "Expected the stock ReXGlue Windows CLI v0.10.0; found '$Version'. No code generation was started."
}

$ManifestText = Get-Content -LiteralPath $CanonicalManifest -Raw
if (-not $ManifestText.Contains('name = "fear_xbox360"')) {
    throw 'Manifest project name must remain fear_xbox360.'
}
if (-not $ManifestText.Contains('file_path = "assets/default.xex"')) {
    throw 'Manifest must point to assets/default.xex.'
}

$AssetsDirectory = Split-Path -Parent $GameDestination
New-Item -ItemType Directory -Force -Path $AssetsDirectory | Out-Null
if (-not (Test-Path -LiteralPath $GameDestination -PathType Leaf) -or
    (Get-FileHash -LiteralPath $GameDestination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ExpectedHash) {
    Copy-Item -LiteralPath $GameXex -Destination $GameDestination -Force
}
$CopiedHash = (Get-FileHash -LiteralPath $GameDestination -Algorithm SHA256).Hash.ToLowerInvariant()
if ($CopiedHash -ne $ExpectedHash) {
    throw 'The staged XEX did not pass the SHA-256 check after copying.'
}

# ReXGlue stamps sdk_version into the manifest it receives. Keep the tracked
# manifest exact by generating from a local copy in the same directory.
Copy-Item -LiteralPath $CanonicalManifest -Destination $LocalManifest -Force
$Arguments = @()
if ($Force) {
    $Arguments += '-f'
}
$Arguments += @('codegen', $LocalManifest)
Push-Location $RepoRoot
try {
    & $ReXGlueExe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "ReXGlue codegen failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath $GeneratedSources -PathType Leaf)) {
    throw "Codegen returned success but did not produce $GeneratedSources"
}
Write-Host "Generated source name: fear_xbox360"
Write-Host "Verified XEX SHA-256: $ExpectedHash"
Write-Host "ReXGlue CLI: v$Version"
Write-Host "Generated source is local under generated/default and must not be committed or redistributed."

