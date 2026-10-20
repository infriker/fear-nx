[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$EdenRoot,
    [string]$EdenExecutableName = 'eden-cli.exe',
    [string]$TitleDirectory,
    [string]$NroName = 'fear_nx.nro'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$edenRoot = [System.IO.Path]::GetFullPath($EdenRoot)
if ([string]::IsNullOrWhiteSpace($TitleDirectory)) {
    $TitleDirectory = Join-Path $edenRoot 'user\sdmc\switch\fear-nx'
}
$edenExecutable = [System.IO.Path]::GetFullPath((Join-Path $edenRoot $EdenExecutableName))
$titleDirectory = [System.IO.Path]::GetFullPath($TitleDirectory)
$nroPath = Join-Path $titleDirectory $NroName
$configPath = Join-Path $titleDirectory 'fear-nx.toml'
$gameXex = Join-Path $titleDirectory 'game\default.xex'
$expectedXexSha256 = 'e7fccdf6dd57bb58d10efbdd0b2ca567b8cece5b6a3fc696c7fca879aaea2792'

if (-not (Test-Path -LiteralPath $edenExecutable -PathType Leaf)) {
    throw "Eden executable not found: $edenExecutable. This script does not download or install Eden."
}
if (-not (Test-Path -LiteralPath $nroPath -PathType Leaf)) {
    throw "Staged NRO not found: $nroPath. Run scripts/stage-eden.ps1 after building fear_nx.nro."
}
if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
    throw "The per-title config is missing beside the NRO: $configPath"
}
if (-not (Test-Path -LiteralPath $gameXex -PathType Leaf)) {
    throw "Game data is not staged: $gameXex. Run scripts/stage-eden.ps1 -CopyGameData first."
}
$actualXexSha256 = (Get-FileHash -LiteralPath $gameXex -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualXexSha256 -ne $expectedXexSha256) {
    throw "Refusing to launch unsupported game data. default.xex SHA-256 is $actualXexSha256; expected $expectedXexSha256."
}

$workingDirectory = Split-Path -Parent $edenExecutable
Write-Host "Launching the staged F.E.A.R. NX NRO in Eden: $nroPath"
Write-Host "Per-title config: $configPath"
$process = Start-Process -FilePath $edenExecutable -ArgumentList ('"{0}"' -f $nroPath) -WorkingDirectory $workingDirectory -PassThru
Write-Host "Eden started (PID $($process.Id))."
