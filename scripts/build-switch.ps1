[CmdletBinding()]
param(
    [string]$ProjectRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$Image = 'fear-nx-switch:devkitpro-20260219-clang19',
    [ValidateRange(1,32)][int]$Jobs = 4,
    [switch]$CompileOnly,
    [switch]$ConfigureOnly
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
$mode = if ($CompileOnly) { 'ON' } else { 'OFF' }
$configure = @('run','--rm','--mount',"type=bind,source=$ProjectRoot,target=/project",
    '-w','/project',$Image,'cmake','-S','.','-B','out/switch','-G','Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_TOOLCHAIN_FILE=/project/tools/switch/toolchain.cmake',
    '-DREXSDK_DIR=/project/third_party/rexglue-sdk',
    '-DREXGLUE_SWITCH_NVK_SDK=/project/sdk/nvk',
    "-DREXGLUE_SWITCH_COMPILE_ONLY=$mode")
& docker @configure
if ($LASTEXITCODE -ne 0) { throw 'Switch CMake configuration failed.' }
if (-not $ConfigureOnly) {
    [string[]]$targets = if ($CompileOnly) {
        @('fear_nx_recomp', 'rexruntime', 'rexgpu-xenos', 'rexglue_switch_startup', 'fear_nx')
    } else { @('fear_nx') }
    & docker run --rm --mount "type=bind,source=$ProjectRoot,target=/project" `
        -w /project $Image cmake --build out/switch --parallel $Jobs --target @targets
    if ($LASTEXITCODE -ne 0) { throw 'Switch compilation failed.' }
}
