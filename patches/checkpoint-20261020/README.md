# Complete dependency snapshot — 2026-10-20

These are **complete alternative patches against each exact base commit** in
`manifest.json`. Do not apply them after the numbered patch series. They preserve
all tracked working-source changes and the listed new source files, including
the final Eden display/texture-cache diagnostics and depth-format mapping.
The existing dependency indexes and worktrees were not modified.

To restore a dependency, clone its recorded origin in a separate directory,
check out its `base_commit`, and apply its single snapshot patch:

```powershell
git -C <checkout> apply --index <absolute-path-to-snapshot.patch>
git -C <checkout> submodule update --init --recursive
```

Each patch was applied to a temporary Git index at its base; the resulting
tree hash exactly matched the captured working source tree. Submodule revisions
are recorded in the manifest. Ignored build outputs, game data and logs are not
part of these patches. Build instructions remain in `docs/building.md` and
`docs/driver-build.md`; use the numbered-patch workflow OR this complete snapshot,
not both. The numbered SDK series can be checked with `bootstrap-sdk.ps1`.

For the current ReXGlue runtime use `../native-vulkan/` and `scripts/bootstrap-native-sdk.ps1`. This older directory remains the complete driver/emulator snapshot reference.
