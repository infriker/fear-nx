[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [Parameter(Mandatory = $true)][string]$EdenRoot,
    [Parameter(Mandatory = $true)][string]$GameSource,
    [switch]$CopyGameData,
    [switch]$UpdateNro,
    [switch]$ReplaceConfig
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$expectedXexSha256 = 'e7fccdf6dd57bb58d10efbdd0b2ca567b8cece5b6a3fc696c7fca879aaea2792'
$repoRoot = [System.IO.Path]::GetFullPath($RepoRoot)
$edenRoot = [System.IO.Path]::GetFullPath($EdenRoot)
$distRoot = Join-Path $repoRoot 'dist\switch\fear-nx'
$nroSource = Join-Path $repoRoot 'out\switch\fear_nx.nro'
$configSource = Join-Path $repoRoot 'release\fear-nx.toml'
$edenSwitchRoot = Join-Path $edenRoot 'user\sdmc\switch'
$edenTitlePath = Join-Path $edenSwitchRoot 'fear-nx'
$gameRoot = Join-Path $distRoot 'game'

foreach ($required in @($nroSource, $configSource)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required build/profile file is missing: $required"
    }
}
if (-not (Test-Path -LiteralPath $edenRoot -PathType Container)) {
    throw "Eden portable directory not found: $edenRoot. This script does not download or install Eden."
}
if (-not (Test-Path -LiteralPath $edenSwitchRoot -PathType Container)) {
    if ($PSCmdlet.ShouldProcess($edenSwitchRoot, 'Create Eden SD-card switch directory')) {
        New-Item -ItemType Directory -Path $edenSwitchRoot -Force | Out-Null
    }
}

if ($CopyGameData) {
    $gameSource = [System.IO.Path]::GetFullPath($GameSource)
    $sourceXex = Join-Path $gameSource 'default.xex'
    if (-not (Test-Path -LiteralPath $sourceXex -PathType Leaf)) {
        throw "The extracted Xbox 360 game root must contain default.xex: $gameSource"
    }
    $sourceHash = (Get-FileHash -LiteralPath $sourceXex -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($sourceHash -ne $expectedXexSha256) {
        throw "Refusing game-data copy: default.xex hash is $sourceHash, expected $expectedXexSha256"
    }
    if (Test-Path -LiteralPath $gameRoot) {
        throw "Refusing to overwrite an existing game directory: $gameRoot. Preserve it and choose an empty dist folder."
    }
}

if (-not (Test-Path -LiteralPath $distRoot -PathType Container)) {
    if ($PSCmdlet.ShouldProcess($distRoot, 'Create Switch package directory')) {
        New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
    }
}
$stagedNro = Join-Path $distRoot 'fear_nx.nro'
if (Test-Path -LiteralPath $stagedNro -PathType Leaf) {
    $sourceHash = (Get-FileHash -LiteralPath $nroSource -Algorithm SHA256).Hash
    $stagedHash = (Get-FileHash -LiteralPath $stagedNro -Algorithm SHA256).Hash
    if ($sourceHash -ne $stagedHash -and -not $UpdateNro) {
        throw "A different staged NRO already exists at $stagedNro. Pass -UpdateNro to replace this fear-nx build."
    }
    if ($sourceHash -ne $stagedHash -and $PSCmdlet.ShouldProcess($stagedNro, 'Replace staged fear-nx NRO')) {
        Copy-Item -LiteralPath $nroSource -Destination $stagedNro -Force
    }
} elseif ($PSCmdlet.ShouldProcess($stagedNro, 'Stage fear-nx NRO')) {
    Copy-Item -LiteralPath $nroSource -Destination $stagedNro
}

$stagedConfig = Join-Path $distRoot 'fear-nx.toml'
if (-not (Test-Path -LiteralPath $stagedConfig)) {
    if ($PSCmdlet.ShouldProcess($stagedConfig, 'Install first-run fear-nx config')) {
        Copy-Item -LiteralPath $configSource -Destination $stagedConfig
    }
} else {
    $sourceConfigHash = (Get-FileHash -LiteralPath $configSource -Algorithm SHA256).Hash
    $existingConfigHash = (Get-FileHash -LiteralPath $stagedConfig -Algorithm SHA256).Hash
    if ($sourceConfigHash -ne $existingConfigHash) {
        if ($ReplaceConfig) {
            if ($PSCmdlet.ShouldProcess($stagedConfig, 'Replace local fear-nx config with release defaults')) {
                Copy-Item -LiteralPath $configSource -Destination $stagedConfig -Force
            }
        } else {
            Write-Warning "Keeping the existing local config at $stagedConfig. Pass -ReplaceConfig only if you want release defaults restored."
        }
    }
}

if ($CopyGameData -and $PSCmdlet.ShouldProcess($gameRoot, "Copy extracted game data from $GameSource (can take several minutes)")) {
    $null = New-Item -ItemType Directory -Path $gameRoot
    & robocopy.exe $gameSource $gameRoot /E /COPY:DAT /DCOPY:DAT /R:2 /W:2 /XJ
    $copyExitCode = $LASTEXITCODE
    if ($copyExitCode -ge 8) {
        throw "Robocopy failed with exit code $copyExitCode. The partial destination is retained at $gameRoot; inspect it before retrying."
    }
    $copiedHash = (Get-FileHash -LiteralPath (Join-Path $gameRoot 'default.xex') -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($copiedHash -ne $expectedXexSha256) {
        throw "Copied default.xex failed validation: $copiedHash"
    }
}

if (Test-Path -LiteralPath $edenTitlePath) {
    $item = Get-Item -LiteralPath $edenTitlePath -Force
    if ($item.LinkType -ne 'Junction' -or [System.IO.Path]::GetFullPath($item.Target) -ne [System.IO.Path]::GetFullPath($distRoot)) {
        throw "Eden path already exists and is not the expected junction to $distRoot. It was left unchanged: $edenTitlePath"
    }
} elseif ($PSCmdlet.ShouldProcess($edenTitlePath, "Create junction to $distRoot")) {
    New-Item -ItemType Junction -Path $edenTitlePath -Target $distRoot | Out-Null
}

Write-Host "Eden title path: $edenTitlePath"
Write-Host "Build package:    $distRoot"
Write-Host 'No emulator was launched.'
