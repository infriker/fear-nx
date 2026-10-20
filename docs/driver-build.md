# Сборка NVK для Nintendo Switch

Этот документ собирает статические библиотеки Mesa NVK для Horizon/AArch64. Он не скачивает и не включает игровые файлы. Архивы остаются локальными в `sdk/nvk/` и исключены из Git.

## Зафиксированные версии

- `danfromtico/mesa-switch`, commit `d4a00ea0ab3f59afb967cc5d779e4263d237bd77` (Mesa 26.2.3).
- Патчи `0001`–`0004` — сохранённые патчи Project8 без изменений. `0005` и `0006` —
  локальные патчи `fear-nx` для совместимости с Eden.
- `0005` один раз привязывает класс Maxwell 3D (`SET_OBJECT`, `0xB197`) к субканалу
  0 перед встроенными NVK-командами cache-acquire/fence.
- `0006` перед первой отправкой команд привязывает 3D и запрошенные NVK-движки
  в одной bootstrap-записи. В частности, copy engine класса `0xB0B5` привязывается
  к субканалу 4; subchannel 4/method `0x1C0` (метод `NVA0B5_SET_REMAP_CONST_A`)
  был зафиксирован диагностикой Test 9 как незапривязанный. Исправление в Eden ещё
  не проверено: успешные компиляция и staging не доказывают прохождение этого места
  или отображение меню.
- База devkitPro `devkitpro/devkita64:20260219`.
- Сборка ведётся в Docker Desktop с Linux engine; исходники и результаты хранятся на диске `D:`.

## Подготовка исходников

Откройте PowerShell в корне `fear-nx`. Эти команды создают shallow checkout без истории и без частичной загрузки blobs:

```powershell
$mesa = Join-Path (Get-Location) 'third_party/mesa-switch'
New-Item -ItemType Directory -Force -Path $mesa | Out-Null
git -C $mesa init
git -C $mesa remote add origin https://github.com/danfromtico/mesa-switch
git -C $mesa fetch --depth=1 --no-tags origin d4a00ea0ab3f59afb967cc5d779e4263d237bd77
git -C $mesa checkout --detach FETCH_HEAD

Get-ChildItem '.\patches\mesa-switch' -Filter '000*.patch' | Sort-Object Name | ForEach-Object {
    git -C $mesa apply --check -- $_.FullName
    if ($LASTEXITCODE -ne 0) { throw "Проверка патча не прошла: $($_.Name)" }
    git -C $mesa apply -- $_.FullName
    if ($LASTEXITCODE -ne 0) { throw "Не удалось применить патч: $($_.Name)" }
}
```

## Ограничение ресурсов и Docker-образ

Скрипт из патча `0004` запускает `ninja` без лимита, а его Dockerfile собирает SPIR-V Tools с `nproc`. На этом проекте команды ограничены тремя задачами, а Rust target нужно установить явно. После применения патчей внесите локальные настройки сборки:

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

Шаблон `000*.patch` применяет все шесть патчей в порядке имени: четыре неизменённых патча Project8, затем локальные `0005` и `0006`.

Нормализация `rustc` и `bindgen` нужна при checkout на Windows: CRLF в shebang не позволяет Linux-контейнеру запустить эти скрипты напрямую.

## Сборка библиотек

Запустите контейнер, подставив фактический путь checkout:

```powershell
$mesaMount = $mesa.Replace('\', '/')
docker run --detach --name mesa-switch-build `
  --mount "type=bind,source=$mesaMount,target=/project" `
  --workdir /project devkitpro-mesa-rust-aarch64 sleep infinity
```

Подготовьте обёртки, заголовки и совместимые stub-архивы, которые предусмотрены сборочной схемой Horizon:

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

Соберите host-инструменты генерации шейдеров:

```powershell
docker exec -e CARGO_BUILD_JOBS=3 mesa-switch-build bash -lc 'cd /project && meson setup builddir-native --wipe -Dvulkan-drivers= -Dgallium-drivers= -Dshader-cache=true -Dplatforms= -Dglx=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dtools=[] -Dllvm=enabled -Dmesa-clc=enabled -Dprecomp-compiler=enabled -Dinstall-mesa-clc=true'
docker exec -e CARGO_BUILD_JOBS=3 mesa-switch-build ninja -j3 -C /project/builddir-native src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2
```

Настройте и соберите Switch NVK:

```powershell
docker exec -e MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu -e CARGO_BUILD_JOBS=3 mesa-switch-build bash -lc 'export PATH=/usr/local/libexec:/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:$PATH && cd /project && meson setup builddir-switch --wipe --cross-file switch_cross_file.txt --native-file p8-native.txt --buildtype=release -Doptimization=1 -Db_lto=false -Db_ndebug=true -Dvulkan-drivers=nouveau -Dgallium-drivers=nouveau -Dshader-cache=true -Dgallium-rusticl=false -Dplatforms=switch -Dglx=disabled -Degl=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dshared-glapi=disabled -Dshared-llvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system -Dcpp_rtti=false'

docker exec -e MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu -e CARGO_BUILD_JOBS=3 mesa-switch-build bash -lc 'export PATH=/usr/local/libexec:/project/builddir-native/src/compiler/clc:/project/builddir-native/src/compiler/spirv:$PATH && ninja -j3 -C /project/builddir-switch src/nouveau/vulkan/libnvk.a src/nouveau/vulkan/libvulkan.a src/vulkan/util/libvulkan_util.a src/compiler/nir/libnir.a src/compiler/libcompiler.a src/compiler/spirv/libvtn.a src/util/libxmlconfig.a src/nouveau/compiler/libnak.a src/nouveau/compiler/libnak_rs.a src/nouveau/nil/libnil.a src/nouveau/nil/liblibnil_format_table.a src/nouveau/mme/libnouveau_mme.a src/nouveau/winsys/libnouveau_ws.a src/nouveau/headers/libnvidia_headers_c.a src/compiler/rust/libcompiler_c_helpers.a src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a'
```

## Проверка и размещение SDK

При индексации Rust-архива GNU BFD может вывести ожидаемые предупреждения о
несовместимости LLVM bitcode: производитель `LLVM23.1.4-rust-1.101.0-nightly`,
читатель `LLVM 14.0.6` (например, `Opaque pointers are only supported in
-opaque-pointers mode` или `Invalid record`). BFD-плагин не может прочитать этот
Rust LTO bitcode для LTO-индекса. Staging-скрипт отдельно проверяет каждый обычный
объект итогового архива как ELF64 little-endian для AArch64 и вызывает
`aarch64-none-elf-ranlib`. В текущей успешной staging-проверке было принято 1557
AArch64-объектов. Stage подтверждает структуру и архитектуру архива; разрешение
символов проверяется последующей линковкой NRO. Ни stage, ни линковка не подтверждают
рендер меню в Eden.

Helper проверяет каждый ELF-объект в объединённом архиве и индексирует его `aarch64-none-elf-ranlib`. Для запуска helper смонтируйте корень проекта и Mesa checkout во временный контейнер:

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

Укажите `REXGLUE_SWITCH_NVK_SDK` на корень `sdk/nvk` (папка, содержащая `lib/`). Не добавляйте SDK-архивы в Git; каталог `sdk/` уже исключён из репозитория. Сборка проверяет архитектуру архивов, но сам факт компиляции ещё не подтверждает запуск Vulkan или рендер в Eden.
