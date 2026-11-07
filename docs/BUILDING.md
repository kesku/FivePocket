# Building iPhone Duo from source

Prerequisites: an Apple-silicon Mac, Xcode with the iOS 27.1 SDK or later, CMake 3.24+, Ninja, Git and Python 3.12+. Supply your own Install and Play disc images. No recovered kit, generated game source or signing key is required in the checkout.

## Build

Select your Xcode installation and build the pinned host tools and audio decoder. The NDK flag accepts Google's terms for the shader tools; read those terms before using it.

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
python3 tools/own_disc/bootstrap.py all --work "$PWD/build/dependencies" --accept-ndk-license
python3 tools/own_disc/generate.py --tools "$PWD/build/dependencies" \
  --play-disc /path/to/Play.iso --install-disc /path/to/Install.iso \
  --out "$PWD/local-analysis/builder/duo-source"
cmake -S . -B build/duo -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/iOS.cmake -DCMAKE_BUILD_TYPE=Release \
  -DFIVEPOCKET_GAME_DIR="$PWD/local-analysis/builder/duo-source" \
  -DFIVEPOCKET_BUNDLE_IDENTIFIER=com.example.fivepocket.duo
cmake --build build/duo --parallel 6
cmake --build build/duo --target ipa
```

The output is `build/duo/FivePocketDuo.app` and `build/duo/FivePocketDuo.ipa`. Signing is disabled by default. Sign locally with your own Apple identity; always use the existing bundle identifier when updating an installed app. Uninstalling removes its data.

Host compilers and FFmpeg are built from pinned source. CMake fetches pinned SIMDe headers and the official MoltenVK iOS SDK. The small vendored Xenon compiler snapshot preserves the matching public source whose original Git revision is no longer fetchable; its provenance and hashes are in `third_party/xenonrecomp/SOURCE.json`.

Generated PowerPC code, shader data and payloads stay under the ignored `local-analysis/builder` directory. Build outputs and dependency caches stay under `build`. Never commit or share those outputs, game files, saves, or signing material.

## Checks

The host tests in the root README require no game files. `python3 tools/release/run_checks.py` checks source names, secrets and personal details. Before publishing source changes, also run `tools/release/taint_scan.py` against your local discs and generated inputs.
