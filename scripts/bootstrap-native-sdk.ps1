[CmdletBinding()]
param(
    [string]$SdkPath,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path $PSScriptRoot -Parent
if ([string]::IsNullOrWhiteSpace($SdkPath)) {
    $SdkPath = Join-Path $RepoRoot 'third_party/rexglue-sdk'
}
$SdkPath = [IO.Path]::GetFullPath($SdkPath)
$Snapshot = Join-Path $RepoRoot 'patches/native-vulkan'
$Manifest = Get-Content -LiteralPath (Join-Path $Snapshot 'manifest.json') -Raw | ConvertFrom-Json
$Patch = Join-Path $Snapshot $Manifest.patch
if ((Get-FileHash -LiteralPath $Patch -Algorithm SHA256).Hash -ne $Manifest.sha256) {
    throw 'The SDK snapshot patch checksum does not match its manifest.'
}
function Invoke-SdkGit {
    param([string[]]$GitArgs)
    $Result = & git -C $SdkPath @GitArgs
    if ($LASTEXITCODE -ne 0) { throw "SDK git operation failed: $($GitArgs -join ' ')" }
    return $Result
}
function Get-SourceTree {
    $TempIndex = Join-Path ([IO.Path]::GetTempPath()) ('fear-native-' + [guid]::NewGuid().ToString('N') + '.index')
    $PreviousIndex = $env:GIT_INDEX_FILE
    try {
        $env:GIT_INDEX_FILE = $TempIndex
        $null = Invoke-SdkGit @('read-tree', 'HEAD')
        $null = Invoke-SdkGit @('add', '-A')
        return (Invoke-SdkGit @('write-tree')).Trim()
    } finally {
        if ($null -eq $PreviousIndex) { Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue }
        else { $env:GIT_INDEX_FILE = $PreviousIndex }
        Remove-Item -LiteralPath $TempIndex, ($TempIndex + '.lock') -Force -ErrorAction SilentlyContinue
    }
}
if (-not (Test-Path -LiteralPath $SdkPath)) {
    if ($VerifyOnly) { throw "SDK does not exist: $SdkPath" }
    New-Item -ItemType Directory -Path (Split-Path $SdkPath -Parent) -Force | Out-Null
    & git clone --no-checkout $Manifest.origin $SdkPath
    if ($LASTEXITCODE -ne 0) { throw 'SDK clone failed.' }
    $null = Invoke-SdkGit @('checkout', '--detach', $Manifest.base_commit)
}
if ((Invoke-SdkGit @('rev-parse', 'HEAD')).Trim() -ne $Manifest.base_commit) {
    throw 'Existing SDK has a different base commit. It was not reset; use a separate path.'
}
$Status = @(Invoke-SdkGit @('status', '--porcelain', '--untracked-files=all'))
if ($Status.Count -eq 0) {
    if ($VerifyOnly) { throw 'SDK is at the clean base; the native snapshot is not applied.' }
    $null = Invoke-SdkGit @('apply', '--check', $Patch)
    $null = Invoke-SdkGit @('apply', '--index', $Patch)
    $null = Invoke-SdkGit @('submodule', 'update', '--init', '--recursive')
} elseif (@(Invoke-SdkGit @('ls-files', '--others', '--exclude-standard')).Count -gt 0) {
    throw 'Existing SDK has untracked files. It was left untouched; review or use a separate path.'
}
$Submodules = @(Invoke-SdkGit @('submodule', 'status', '--recursive'))
$ActualRevisions = @($Submodules | ForEach-Object { ($_.Trim() -split '\s+')[0..1] -join ' ' })
$ExpectedRevisions = @($Manifest.recursive_submodules | ForEach-Object { ($_.Trim() -split '\s+')[0..1] -join ' ' })
if (Compare-Object $ExpectedRevisions $ActualRevisions) {
    throw 'SDK submodule revisions differ from the snapshot or are missing. Existing submodules were not modified.'
}
if ((Get-SourceTree) -ne $Manifest.patched_tree) {
    throw 'SDK sources differ from the complete native snapshot. Existing source and real index were not changed.'
}
Write-Output "Verified native SDK: $SdkPath"
Write-Output "Base: $($Manifest.base_commit); source tree: $($Manifest.patched_tree)"
