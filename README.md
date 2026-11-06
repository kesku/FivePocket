# FivePocket Duo

This fork focuses exclusively on **iPhone Duo (iPhone19,4)**. It runs your own Xbox 360 copy through static recompilation, with the game above and the map, phone and controls below when the display is split.

Android, Thor, generic iPad/iOS editions and Windows application builders are not maintained in this fork. Shared emulation, graphics and shader code remains where the Duo runtime needs it.

No game is included. Supply your own two disc images. Generated game code, imported assets, builds and saves stay on your own devices; do not publish them. See [LEGAL.md](LEGAL.md).

## Source layout

- `runtime/`: the shared runtime and authored shaders.
- `runtime/host`, `runtime/bridge`, `runtime/gpu`: native dependencies moved out of the former Android/probe projects.
- `resources/duo/`: the Duo app template, font and icon.
- `tools/own_disc/`: the Mac build, verification and signing stages.
- `tools/fp_codegen`, `tools/rpf7_inventory`: code and shader translation support.

The Android NDK is still a build dependency: the current pipeline uses its host shader tools and an intermediate decoder build. It does not produce an Android application.

## Build and test

The build needs an Apple-silicon Mac, Xcode, the matching upstream Duo source kit and your own discs. The kit and generated inputs are not committed here. [Build notes](docs/BUILDING.md).

```sh
cmake -S runtime/tests -B build/runtime -DCMAKE_BUILD_TYPE=Release
cmake --build build/runtime
ctest --test-dir build/runtime --output-on-failure
python3 tools/release/run_checks.py
```

When updating the installed app, keep its bundle identifier and install over it. Uninstalling removes its data and saves.

## Licence

GPL-3.0-or-later; see [COPYING](COPYING), [CREDITS.md](CREDITS.md), [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and [LICENSES/](LICENSES/). This fork is based on [marcm0de/FivePocket](https://github.com/marcm0de/FivePocket) and is not affiliated with the game publisher or Apple.
