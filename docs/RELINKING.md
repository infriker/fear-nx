# Rebuilding with modified libraries

The NRO links the ReXGlue runtime and its dependencies, including FFmpeg and
libmspack, plus Mesa/NVK. Component licenses are preserved in `NOTICE` and
`LICENSES/`. The installer package includes their notices in `LICENSES.txt`.

To use modified library sources, prepare the pinned SDK with
`scripts/bootstrap-native-sdk.ps1`, then change the relevant source under
`third_party/rexglue-sdk/thirdparty/`. FFmpeg is at `thirdparty/FFmpeg` and
libmspack at `thirdparty/libmspack`. Record any local changes separately before
updating dependencies; the bootstrap verifier intentionally rejects a modified
tree rather than replacing it.

Follow [building.md](building.md) with your own supported game executable and
run `scripts/build-switch.ps1` to rebuild and relink the application. The NRO
packaging step runs after linking. Driver changes require rebuilding the Mesa
archives using [driver-build.md](driver-build.md), followed by the application
build. The CMake target tracks both staged driver archives as link inputs.

No signing key from this project is needed to load a locally rebuilt NRO in a
homebrew environment. Original game inputs and generated game source are
local build material, not downloadable source dependencies in this repository.
