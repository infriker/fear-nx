# Building NVK for Nintendo Switch

Build the static Mesa/NVK libraries for Horizon/AArch64. Game files are not
needed. The resulting archives stay in the ignored `sdk/nvk/` directory.

## Pinned versions

- `danfromtico/mesa-switch` commit `d4a00ea0ab3f59afb967cc5d779e4263d237bd77` (Mesa 26.2.3).
- Patches `0001`–`0004` come from Project8 unchanged. Patches `0005` and `0006`
  bind GPU engines before the first submission for Eden compatibility.
- Patch `0007` backports the GPFIFO-capacity fix from upstream commit
  `f68a41350dbc0b380dcb1462898d8bed48dfdb5e`, preserving the engine bindings.
- Patch `0008` fixes memory binding for images with a stencil-copy plane.
  It is included in v0.1.1; affected images may lose compression, with no
  global compression override. See the patch directory README.
- devkitPro base image: `devkitpro/devkita64:20260219`.
- Docker Desktop with its Linux engine and PowerShell 7.

## Prepare the sources

Run these commands from the repository root to create a shallow checkout:

```powershell
$mesa = Join-Path (Get-Location) 'third_party/mesa-switch'
New-Item -ItemType Directory -Force -Path $mesa | Out-Null
git -C $mesa init
git -C $mesa remote add origin https://github.com/danfromtico/mesa-switch
git -C $mesa fetch --depth=1 --no-tags origin d4a00ea0ab3f59afb967cc5d779e4263d237bd77
git -C $mesa checkout --detach FETCH_HEAD

Get-ChildItem '.\patches\mesa-switch' -Filter '000*.patch' | Sort-Object Name | ForEach-Object {
    git -C $mesa apply --check -- $_.FullName
    if ($LASTEXITCODE -ne 0) { throw "Patch check failed: $($_.Name)" }
    git -C $mesa apply -- $_.FullName
    if ($LASTEXITCODE -ne 0) { throw "Could not apply patch: $($_.Name)" }
}
```

## Prepare the Docker image

Limit parallel compilation to three jobs and install the Rust target explicitly.
Apply these build settings after the eight patches:

```powershell
$dockerfile = Join-Path $mesa 'Docker.rust'
$d = Get-Content -Raw $dockerfile
if ($d -notmatch 'FROM devkitpro/devkita64:20260219') {
    $d = $d.Replace('FROM devkitpro/devkita64:latest', 'FROM devkitpro/devkita64:20260219')
}
$d = $d.Replace('make -j$(nproc)', 'make -j3')
$d = $d.Replace('rustup component add rust-src', 'rustup component add rust-src && rustup target add aarch64-unknown-linux-gnu')
Set-Content -LiteralPath $dockerfile -Value $d -Encoding utf8 -NoNewline

$buildScript = Join-Path $mesa 'build-switch-p8.sh'
$s = Get-Content -Raw $buildScript
$s = $s.Replace('ninja -C /project/builddir-native', 'ninja -j3 -C /project/builddir-native')
$s = $s.Replace('ninja -C /project/builddir-switch', 'ninja -j3 -C /project/builddir-switch')
$chmod = 'chmod +x /usr/local/libexec/bindgen /usr/local/libexec/rustc'
$lfFix = $chmod + "`r`nsed -i 's/\r$//' /usr/local/libexec/bindgen /usr/local/libexec/rustc"
$s = $s.Replace($chmod, $lfFix)
Set-Content -LiteralPath $buildScript -Value $s -Encoding utf8 -NoNewline

docker build -f (Join-Path $mesa 'Docker.rust') -t devkitpro-mesa-rust-aarch64 $mesa
```

The patch pattern applies all eight patches in filename order. Normalizing the
`rustc` and `bindgen` wrappers removes Windows line endings from their shebangs.

## Build the libraries

Start a container with the Mesa checkout mounted:

```powershell
$mesaMount = $mesa.Replace('\', '/')
docker run --detach --name mesa-switch-build `
  --mount "type=bind,source=$mesaMount,target=/project" `
  --workdir /project devkitpro-mesa-rust-aarch64 sleep infinity
```

Prepare the wrappers, headers and compatibility archives used by Horizon:

```powershell
docker exec mesa-switch-build bash -lc 'set -e
mkdir -p /usr/local/libexec
cp /project/bindgen-switch-wrapper.sh /usr/local/libexec/bindgen
cp /project/rustc-switch-wrapper.sh /usr/local/libexec/rustc
cp /project/bindgen-atomic-shim.h /usr/local/libexec/bindgen-atomic-shim.h
chmod +x /usr/local/libexec/bindgen /usr/local/libexec/rustc
sed -i "s/\r$//" /usr/local/libexec/bindgen /usr/local/libexec/rustc
MESON_BIN=$(command -v meson)
mkdir -p /usr/local/bin
if [ "$MESON_BIN" != /usr/local/bin/meson ]; then ln -sf "$MESON_BIN" /usr/local/bin/meson; fi
cp /project/src/gallium/winsys/nouveau/drm/nouveau.h /opt/devkitpro/portlibs/switch/include/
cp /project/src/nouveau/headers/nv_device_info.h /opt/devkitpro/portlibs/switch/include/
if [ ! -d /usr/lib/llvm-15/lib/clang/15/include ]; then ln -sf /usr/lib/llvm-15/lib/clang/15.0.6/include /usr/lib/llvm-15/lib/clang/15/include; fi
cd /tmp
printf "void __mesa_switch_stub_lib(void) {}\n" > stub_posix.c
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc -c stub_posix.c -o stub_posix.o
for L in dl rt util; do /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar rcs /opt/devkitpro/portlibs/switch/lib/lib${L}.a stub_posix.o; done'
```

Build the host tools used for shader generation:

```powershell
docker exec -e CARGO_BUILD_JOBS=3 mesa-switch-build bash -lc 'cd /project && meson setup builddir-native --wipe -Dvulkan-drivers= -Dgallium-drivers= -Dshader-cache=true -Dplatforms= -Dglx=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dtools=[] -Dllvm=enabled -Dmesa-clc=enabled -Dprecomp-compiler=enabled -Dinstall-mesa-clc=true'
docker exec -e CARGO_BUILD_JOBS=3 mesa-switch-build ninja -j3 -C /project/builddir-native src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2
```

Configure and compile Switch NVK:

```powershell
docker exec -e MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu -e CARGO_BUILD_JOBS=3 mesa-switch-build bash -lc 'export PATH=/usr/local/libexec:/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:$PATH && cd /project && meson setup builddir-switch --wipe --cross-file switch_cross_file.txt --native-file p8-native.txt --buildtype=release -Doptimization=1 -Db_lto=false -Db_ndebug=true -Dvulkan-drivers=nouveau -Dgallium-drivers=nouveau -Dshader-cache=true -Dgallium-rusticl=false -Dplatforms=switch -Dglx=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dshared-glapi=disabled -Dshared-llvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system -Dcpp_rtti=false'

docker exec -e MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu -e CARGO_BUILD_JOBS=3 mesa-switch-build bash -lc 'export PATH=/usr/local/libexec:/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:$PATH && ninja -j3 -C /project/builddir-switch src/nouveau/vulkan/libnvk.a src/nouveau/vulkan/libvulkan.a src/vulkan/util/libvulkan_util.a src/compiler/nir/libnir.a src/compiler/libcompiler.a src/compiler/spirv/libvtn.a src/util/libxmlconfig.a src/nouveau/compiler/libnak.a src/nouveau/compiler/libnak_rs.a src/nouveau/nil/libnil.a src/nouveau/nil/liblibnil_format_table.a src/nouveau/mme/libnouveau_mme.a src/nouveau/winsys/libnouveau_ws.a src/nouveau/headers/libnvidia_headers_c.a src/compiler/rust/libcompiler_c_helpers.a src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a'
```

## Validate and stage the SDK

GNU BFD may report LLVM bitcode version warnings while indexing the Rust archive
(for example, `Opaque pointers are only supported in -opaque-pointers mode` or
`Invalid record`). Its plugin cannot read the newer Rust LTO bitcode. The staging
helper separately validates ordinary objects as ELF64 little-endian AArch64 and
runs `aarch64-none-elf-ranlib`. Linking the NRO checks symbol resolution.

Mount the project and Mesa checkout in a staging container:

```powershell
$rootMount = (Get-Location).Path.Replace('\', '/')
docker run --detach --name mesa-stage `
  --mount "type=bind,source=$rootMount,target=/fear-nx" `
  --mount "type=bind,source=$mesaMount,target=/project" `
  --workdir /fear-nx devkitpro-mesa-rust-aarch64 sleep infinity

docker exec mesa-stage python3 /fear-nx/tools/switch/stage-mesa-archive.py `
  /project/builddir-switch/src/nouveau/vulkan/libvulkan.a `
  /fear-nx/sdk/nvk/lib/libvulkan.a `
  --ranlib /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ranlib
docker exec mesa-stage sh -c 'cp /project/builddir-switch/src/nouveau/compiler/libnak_rs.a /fear-nx/sdk/nvk/lib/libnak_rs.a'
docker exec mesa-stage sh -c 'ls -lh /fear-nx/sdk/nvk/lib/libvulkan.a /fear-nx/sdk/nvk/lib/libnak_rs.a'
docker stop mesa-stage
```

Point `REXGLUE_SWITCH_NVK_SDK` to `sdk/nvk`, the directory containing `lib/`.
Keep SDK archives out of Git. Rebuild the NRO after changing the driver and
test it on a Switch.
