# Deko shader smoke test

This host-only tool reads an existing ReXGlue `XESH` microcode cache and checks every record through the same path used by the Deko renderer:

`Xenos microcode -> ReXGlue SPIR-V translator -> SPIRV-Cross GLSL -> UAM DKSH`

It does not create a graphics device or modify the input cache. Reports and GLSL for failed or warned shaders go into the output directory.

From the repository root, build inside the existing Switch toolchain container:

```sh
cmake -S tools/deko -B out/deko-validation/build -G Ninja -DCMAKE_CXX_COMPILER=clang++-19
cmake --build out/deko-validation/build --target deko_shader_smoke -j4
out/deko-validation/build/bin/deko_shader_smoke /project/cache/shaders/shareable/565507D9.xsh out/deko-validation/565507D9 135
```

Mount the repository at `/project`. Put only your locally captured cache in the ignored `cache/` directory. A zero exit code means every record translated, converted, compiled to DKSH, and produced no converter warnings. `results.csv` contains per-shader stage, resource counts, warnings, and errors; `summary.txt` contains aggregate counts.

## PC precompilation and startup warmup

`deko_shader_precompile` produces the exact prepared files consumed by the
Switch renderer. It uses the pinned translator, converter, UAM and compiler
identity, and never creates a GPU device. Build it with the same CMake project.
The Windows/PC wrapper uses the existing Docker toolchain:

```powershell
python tools/deko/precompile_cache.py --warmup-dir "PATH/TO/cache/fear-deko-warmup-v1" --output "out/precompiled-cache"
```

The new NRO records each successfully translated guest program and its exact
modification bits in `cache/fear-deko-warmup-v1/<Xbox-title-id>/*.fwarm`.
The wrapper reads those records, compiles DKSH on the PC and writes
`fear-deko-prepared-v1/*.fshader` plus four fixed internal GLSL programs in
`fear-deko-dksh-v1/*.fcache`. Copy these two directories into the game's
`cache` directory; retain or copy the captured `fear-deko-warmup-v1` directory
as well for startup preparation. Existing unrelated cache entries are retained.
The SDK/compiler version and converter ABI are part of the key; rebuild the
cache when those change. Match `--implicit-lod false` if that setting is
explicitly disabled in the game's TOML (the application default is true).

For an initial pack, `--xsh PATH/TO/565507D9.xsh` accepts the existing PC
microcode cache. XESH has no exact draw-state modifications, so this compiles
only a default variant per program. It does not claim complete game coverage.
`--skip-build` reuses the already built host tool. With no input, the wrapper
prepares only the four fixed internal shaders.

On startup, `[nx] native_shader_warmup = true` (default) translates previously
observed variants and loads their prepared DKSH before the guest thread runs.
Known prepared entries bypass SPIRV-Cross and UAM. Xenos analysis/translation
still happens once per process to restore the SDK's binding tables. Unknown
variants compile synchronously on first use and are saved for later runs;
draws are never skipped while a shader compiles. Warmup is bounded to 4,096
records, with soft 32 MiB input/code budgets within the 64 MiB shader-code pool.
Dynamic geometry uses the prepared cache but is still instantiated on demand.

Check `[deko-cache] warmup complete` and the ten-second summaries for prepared
hits, legacy DKSH hits, UAM compile count and timings. Test the same scene on
two separate launches. `UAM compiles = 0` during a slow interval means the
guest compiler is not the cause of that interval; An emulator has separate host GPU compilation not measured by these counters.

CPU regression checks:

```sh
cmake --build out/deko-validation/build --target deko_shader_cache_test deko_shader_warmup_test -j4
ctest --test-dir out/deko-validation/build --output-on-failure -R 'deko_shader_(cache|warmup)_test'
```


For historical measurements and validation policy changes, see
[the test history](../../docs/deko3d-history.md). Reports and generated caches stay local.
