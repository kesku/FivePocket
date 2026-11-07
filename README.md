# FivePocket Duo

This fork focuses exclusively on **iPhone Duo (iPhone19,4)**. It runs your own Xbox 360 copy through static recompilation, with the game above and the map, phone and controls below when the display is split.

Android, Thor, generic iPad/iOS editions and Windows application builders are not maintained in this fork. Shared emulation, graphics and shader code remains where the Duo runtime needs it.

No game is included. Supply your own two disc images. Generated game code, imported assets, builds and saves stay on your own devices; do not publish them. See [LEGAL.md](LEGAL.md).

## Source layout

- `app/duo/`: the UIKit launcher and application lifecycle.
- `platform/apple/`: Metal presentation, audio, input and Apple platform support.
- `runtime/`: the emulation runtime and authored shaders.
- `runtime/host`, `runtime/bridge`, `runtime/gpu`: native dependencies moved out of the former Android/probe projects.
- `resources/duo/`: the Duo app template, font and icon.
- `tools/own_disc/`: the Mac build, verification and signing stages.
- `tools/fp_codegen`, `tools/rpf7_inventory`: code and shader translation support.

The source build fetches pinned compiler/SDK dependencies. NDK shader tools are retained; they do not produce an Android application.

## Build and test

Build from this checkout on an Apple-silicon Mac with Xcode, CMake, Ninja, Python and your own discs. No recovered source kit or prebuilt game runtime is needed. [Source build instructions](docs/BUILDING.md).

```sh
cmake -S . -B build/tests -DFIVEPOCKET_BUILD_DUO=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
python3 tools/release/run_checks.py
```

When updating the installed app, keep its bundle identifier and install over it. Uninstalling removes its data and saves.

## Licence

GPL-3.0-or-later; see [COPYING](COPYING), [CREDITS.md](CREDITS.md), [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and [LICENSES/](LICENSES/). This fork is based on [marcm0de/FivePocket](https://github.com/marcm0de/FivePocket) and is not affiliated with the game publisher or Apple.
