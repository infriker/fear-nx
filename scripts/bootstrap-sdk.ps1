[CmdletBinding()]
param(
    [string]$SdkPath,
    [switch]$VerifyOnly,
    [switch]$AdoptVerified
)

$ErrorActionPreference = 'Stop'
if ($VerifyOnly -and $AdoptVerified) {
    throw 'Choose either -VerifyOnly or -AdoptVerified, not both.'
}
$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($SdkPath)) {
    $SdkPath = Join-Path $RepoRoot 'third_party/rexglue-sdk'
}
$SdkPath = [IO.Path]::GetFullPath($SdkPath)
$SdkCommit = 'c94f5ebdcb3c9d1a460ca48e04f9758448f8d518'
$Project8Commit = '3393f2615deb7e8ac11ce51d2cd0e9defded5669'
$PatchDirectory = Join-Path $RepoRoot 'patches/rexglue-sdk'
$SeriesPath = Join-Path $PatchDirectory 'series'
$GitPath = (Get-Command git -ErrorAction Stop).Source

if (-not (Test-Path -LiteralPath $SeriesPath -PathType Leaf)) {
    throw "Patch series is missing: $SeriesPath"
}
$PatchNames = @(
    Get-Content -LiteralPath $SeriesPath |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and -not $_.StartsWith('#') }
)
if ($PatchNames.Count -ne 22 -or $PatchNames[-1] -ne '0022-compact-shadow-arena.patch') {
    throw 'Expected the 16 pinned Project8 patches followed by local patches 0017 through 0022.'
}
if (($PatchNames | Select-Object -Unique).Count -ne $PatchNames.Count) {
    throw 'The patch series contains duplicate entries.'
}
foreach ($PatchName in $PatchNames) {
    if (-not (Test-Path -LiteralPath (Join-Path $PatchDirectory $PatchName) -PathType Leaf)) {
        throw "Patch listed in the series is missing: $PatchName"
    }
}

$DigestLines = @("SDK=$SdkCommit", "Project8=$Project8Commit") + @(
    foreach ($PatchName in $PatchNames) {
        $PatchHash = (Get-FileHash -LiteralPath (Join-Path $PatchDirectory $PatchName) -Algorithm SHA256).Hash.ToLowerInvariant()
        "$PatchName=$PatchHash"
    }
)
$DigestBytes = [Text.Encoding]::UTF8.GetBytes(($DigestLines -join [Environment]::NewLine))
$Sha = [Security.Cryptography.SHA256]::Create()
try {
    $PatchSetDigest = [BitConverter]::ToString($Sha.ComputeHash($DigestBytes)).Replace('-', '').ToLowerInvariant()
}
finally {
    $Sha.Dispose()
}

function Invoke-SdkGit {
    param([Parameter(Mandatory)][string[]]$GitArgs)
    $Output = & $GitPath -C $SdkPath @GitArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "git $($GitArgs -join ' ') failed in '$SdkPath': $($Output -join ' ')"
    }
    return $Output
}

function Get-SdkDiffHash {
    $DiffPath = Join-Path ([IO.Path]::GetTempPath()) ("fear-nx-sdk-" + [guid]::NewGuid().ToString('N') + '.patch')
    try {
        & $GitPath -C $SdkPath diff --binary HEAD "--output=$DiffPath"
        if ($LASTEXITCODE -ne 0) {
            throw 'Could not inspect the SDK diff.'
        }
        return (Get-FileHash -LiteralPath $DiffPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    finally {
        Remove-Item -LiteralPath $DiffPath -Force -ErrorAction SilentlyContinue
    }
}

function Get-ExpectedPatchDiffHash {
    $TempIndex = Join-Path $env:TEMP ('fear-nx-expected-' + [guid]::NewGuid().ToString('N') + '.index')
    $ExpectedDiffPath = Join-Path $env:TEMP ('fear-nx-expected-' + [guid]::NewGuid().ToString('N') + '.patch')
    $HadPreviousIndex = Test-Path Env:GIT_INDEX_FILE
    $PreviousIndex = $env:GIT_INDEX_FILE
    try {
        $env:GIT_INDEX_FILE = $TempIndex
        $null = Invoke-SdkGit -GitArgs @('read-tree', $SdkCommit)
        foreach ($PatchName in $PatchNames) {
            $null = Invoke-SdkGit -GitArgs @('apply', '--cached', (Join-Path $PatchDirectory $PatchName))
        }
        $null = Invoke-SdkGit -GitArgs @('diff', '--cached', '--binary', 'HEAD', "--output=$ExpectedDiffPath")
        return (Get-FileHash -LiteralPath $ExpectedDiffPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    finally {
        if ($HadPreviousIndex) {
            $env:GIT_INDEX_FILE = $PreviousIndex
        }
        else {
            Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue
        }
        Remove-Item -LiteralPath $TempIndex, ($TempIndex + '.lock'), $ExpectedDiffPath -Force -ErrorAction SilentlyContinue
    }
}
function Get-SdkMarkerPath {
    $GitDirectory = (Invoke-SdkGit -GitArgs @('rev-parse', '--git-dir') | Select-Object -First 1).ToString().Trim()
    if (-not [IO.Path]::IsPathRooted($GitDirectory)) {
        $GitDirectory = Join-Path $SdkPath $GitDirectory
    }
    return Join-Path $GitDirectory 'fear-nx-bootstrap.json'
}

function Assert-NoUntrackedFiles {
    $Untracked = @(Invoke-SdkGit -GitArgs @('ls-files', '--others', '--exclude-standard'))
    if ($Untracked.Count -gt 0) {
        throw "The SDK contains untracked files. Nothing was changed. Move or preserve them yourself, then rerun: $($Untracked -join ', ')"
    }
}

function Update-RecursiveSubmodules {
    $null = Invoke-SdkGit -GitArgs @('submodule', 'update', '--init', '--recursive')
}

if (-not (Test-Path -LiteralPath $SdkPath)) {
    if ($VerifyOnly) {
        throw "SDK checkout does not exist: $SdkPath"
    }
    $ParentDirectory = Split-Path -Parent $SdkPath
    if (-not (Test-Path -LiteralPath $ParentDirectory -PathType Container)) {
        New-Item -ItemType Directory -Force -Path $ParentDirectory | Out-Null
    }
    Write-Host "Cloning ReXGlue SDK at the pinned base commit..."
    & $GitPath clone 'https://github.com/rexglue/rexglue-sdk.git' $SdkPath
    if ($LASTEXITCODE -ne 0) {
        throw 'SDK clone failed.'
    }
    $null = & $GitPath -C $SdkPath checkout --detach $SdkCommit
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not check out the pinned SDK commit in the new clone.'
    }
    $IsNewCheckout = $true
}
else {
    $InsideWorkTree = (& $GitPath -C $SdkPath rev-parse --is-inside-work-tree 2>$null)
    if ($LASTEXITCODE -ne 0 -or $InsideWorkTree -ne 'true') {
        throw "SDK path exists but is not a Git checkout. It was left untouched: $SdkPath"
    }
    $IsNewCheckout = $false
}

$Head = (Invoke-SdkGit -GitArgs @('rev-parse', 'HEAD') | Select-Object -First 1).ToString().Trim().ToLowerInvariant()
if ($Head -ne $SdkCommit) {
    throw "SDK HEAD is $Head; expected $SdkCommit. The checkout was not reset or changed."
}

$MarkerPath = Get-SdkMarkerPath
$Status = @(Invoke-SdkGit -GitArgs @('status', '--porcelain', '--untracked-files=all'))
$HasChanges = $Status.Count -gt 0

if ($AdoptVerified -and -not $HasChanges) {
    throw '-AdoptVerified requires an already-patched SDK tree; it never applies patches.'
}

if ($HasChanges) {
    Assert-NoUntrackedFiles
    if (-not (Test-Path -LiteralPath $MarkerPath -PathType Leaf)) {
        if (-not $AdoptVerified) {
            throw "SDK is dirty but has no F.E.A.R. NX bootstrap marker. To protect local work, bootstrap refused to modify it. Use a new SDK path or review it, then run -AdoptVerified only if it is the exact pinned patch set: $SdkPath"
        }

        $ActualDiffHash = Get-SdkDiffHash
        $ExpectedDiffHash = Get-ExpectedPatchDiffHash
        if ($ActualDiffHash -ne $ExpectedDiffHash) {
            throw "The existing SDK diff does not exactly match all $($PatchNames.Count) pinned patches. Nothing was adopted or changed: $SdkPath"
        }
        $SubmoduleStatus = @(Invoke-SdkGit -GitArgs @('submodule', 'status', '--recursive'))
        $WrongSubmodules = @($SubmoduleStatus | Where-Object { $_ -match '^[+\-U]' })
        if ($WrongSubmodules.Count -gt 0) {
            throw "Cannot adopt while recursive submodules are missing, changed, or conflicted. They were left untouched: $($WrongSubmodules -join '; ')"
        }

        $Marker = [ordered]@{
            SdkCommit = $SdkCommit
            Project8Commit = $Project8Commit
            PatchSetDigest = $PatchSetDigest
            DiffHash = $ActualDiffHash
            PatchCount = $PatchNames.Count
        }
        $Marker | ConvertTo-Json | Set-Content -LiteralPath $MarkerPath -Encoding ascii
        Write-Host "Adopted verified SDK patch state. The SDK worktree and real Git index were not changed; only the private bootstrap marker was written."
        exit 0
    }
    $Marker = Get-Content -LiteralPath $MarkerPath -Raw | ConvertFrom-Json
    $CurrentDiffHash = Get-SdkDiffHash
    $MarkerMatchesCurrent = $Marker.SdkCommit -eq $SdkCommit -and
        $Marker.PatchSetDigest -eq $PatchSetDigest -and
        $Marker.DiffHash -eq $CurrentDiffHash

    if (-not $MarkerMatchesCurrent -and $AdoptVerified) {
        $ExpectedDiffHash = Get-ExpectedPatchDiffHash
        if ($CurrentDiffHash -ne $ExpectedDiffHash) {
            throw "The existing SDK diff does not exactly match all $($PatchNames.Count) pinned patches. Nothing was adopted or changed: $SdkPath"
        }
        $SubmoduleStatus = @(Invoke-SdkGit -GitArgs @('submodule', 'status', '--recursive'))
        $WrongSubmodules = @($SubmoduleStatus | Where-Object { $_ -match '^[+\-U]' })
        if ($WrongSubmodules.Count -gt 0) {
            throw "Cannot refresh the marker while recursive submodules are missing, changed, or conflicted. They were left untouched: $($WrongSubmodules -join '; ')"
        }

        $UpdatedMarker = [ordered]@{
            SdkCommit = $SdkCommit
            Project8Commit = $Project8Commit
            PatchSetDigest = $PatchSetDigest
            DiffHash = $CurrentDiffHash
            PatchCount = $PatchNames.Count
        }
        $UpdatedMarker | ConvertTo-Json | Set-Content -LiteralPath $MarkerPath -Encoding ascii
        Write-Host "Refreshed the private marker after verifying the complete $($PatchNames.Count)-patch SDK diff. SDK source and Git index were not changed."
        exit 0
    }

    if ($Marker.SdkCommit -ne $SdkCommit -or $Marker.PatchSetDigest -ne $PatchSetDigest) {
        throw "Bootstrap marker does not match the pinned SDK and patch series. The SDK was left untouched: $SdkPath"
    }
    if ($CurrentDiffHash -ne $Marker.DiffHash) {
        throw "SDK changes differ from the recorded patch set. Nothing was reset or overwritten: $SdkPath"
    }

    $SubmoduleStatus = @(Invoke-SdkGit -GitArgs @('submodule', 'status', '--recursive'))
    $WrongSubmodules = @($SubmoduleStatus | Where-Object { $_ -match '^[+U]' })
    if ($WrongSubmodules.Count -gt 0) {
        throw "One or more initialized submodules are at a different or conflicted revision. They were left untouched: $($WrongSubmodules -join '; ')"
    }
    $MissingSubmodules = @($SubmoduleStatus | Where-Object { $_ -match '^-' })
    if ($MissingSubmodules.Count -gt 0) {
        if ($VerifyOnly) {
            throw 'One or more recursive SDK submodules are not initialized.'
        }
        Update-RecursiveSubmodules
        $SubmoduleStatus = @(Invoke-SdkGit -GitArgs @('submodule', 'status', '--recursive'))
        if (@($SubmoduleStatus | Where-Object { $_ -match '^[+-U]' }).Count -gt 0) {
            throw 'Recursive SDK submodule initialization did not reach the pinned commits.'
        }
    }
    Write-Host "ReXGlue SDK is already bootstrapped and matches the pinned patch set."
    exit 0
}

if ($VerifyOnly) {
    throw "SDK is at the clean base commit but the F.E.A.R. NX patches are not applied. Run scripts/bootstrap-sdk.ps1 first."
}


Update-RecursiveSubmodules
foreach ($PatchName in $PatchNames) {
    Write-Host "Applying $PatchName"
    & $GitPath -C $SdkPath apply --index (Join-Path $PatchDirectory $PatchName)
    if ($LASTEXITCODE -ne 0) {
        throw "Patch application failed at $PatchName. The checkout was left as-is; no reset was run."
    }
}
Update-RecursiveSubmodules

$Unstaged = & $GitPath -C $SdkPath diff --quiet
if ($LASTEXITCODE -ne 0) {
    throw 'Patch application left unstaged SDK edits; inspect the checkout before continuing.'
}
Assert-NoUntrackedFiles
$DiffHash = Get-SdkDiffHash
$Marker = [ordered]@{
    SdkCommit = $SdkCommit
    Project8Commit = $Project8Commit
    PatchSetDigest = $PatchSetDigest
    DiffHash = $DiffHash
    PatchCount = $PatchNames.Count
}
$Marker | ConvertTo-Json | Set-Content -LiteralPath $MarkerPath -Encoding ascii
Write-Host "Pinned SDK ready: $SdkCommit"
Write-Host "Project8 patch provenance: $Project8Commit (16 patches), plus three local F.E.A.R. NX compatibility patches."
Write-Host "Recursive submodules are initialized. No game files were copied."

