# Rebuilding with modified libraries

The NRO links the ReXGlue runtime and its dependencies, including FFmpeg and
libmspack, plus deko3d, UAM and SPIRV-Cross on this branch. The optional Vulkan
build uses Mesa/NVK instead. Component licenses are preserved in `NOTICE` and
`LICENSES/`. The installer package includes their notices in `LICENSES.txt`.

Prepare the pinned SDK using the checkout and patch commands in
[building.md](building.md#1-prepare-the-sdk), then change the relevant source
under `third_party/rexglue-sdk/thirdparty/`. FFmpeg is at `thirdparty/FFmpeg` and
libmspack at `thirdparty/libmspack`. Record your local changes separately before
updating dependencies. The snapshot tree ID verifies the initial source state;
intentional library modifications will produce a different tree.

Follow [building.md](building.md) with your own supported game executable and
run its CMake configure/build commands to rebuild and relink the application.
The NRO packaging step runs after linking. Deko3d dependencies are prepared by
`tools/switch/prepare_deko3d.py` at the pinned revisions documented there.
Modify the prepared dependency sources and rebuild with
`-DFEAR_DEKO_SKIP_SETUP=ON` to preserve intentional changes. For the optional
Vulkan build, rebuild Mesa archives using [driver-build.md](driver-build.md)
and then relink the application.

No signing key from this project is needed to load a locally rebuilt NRO in a
homebrew environment. Original game inputs and generated game source are
local build material, not downloadable source dependencies in this repository.
