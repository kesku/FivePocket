# Building for iPhone Duo

The only application target in this fork is `duo`. The host is an Apple-silicon Mac with Xcode and Python 3.12 or later. A complete build also needs the matching Duo source kit from the upstream Mac builder release and your own Install and Play disc images.

The repository contains source and build tooling, not a ready-to-run frozen kit. Keep the kit, generated code, shader packs, signing material and disc images outside Git.

From a prepared Duo builder payload:

```sh
./builder fetch --accept-ndk-license
./builder check
./builder build --duo --bundle-id com.example.fivepocket.duo --play-disc PLAY.iso --install-disc INSTALL.iso
```

Use the existing installed bundle identifier when rebuilding an app you already use. Sign with your own Apple development identity and install over the app to preserve its storage.

The pipeline retains NDK host shader tools and its intermediate XMA decoder build. These are dependencies of the Duo build, not Android application targets. PowerPC and shader translation, FFmpeg, FSR and GSR code remain for the same reason.

`ios_compile.py --source-root CHECKOUT --development-toolchain` uses the relocated dependencies in `runtime/host`, `runtime/bridge` and `runtime/gpu`. It verifies the original kit inputs, records the selected source hashes and compiles the assembled tree. Development mode reports toolchain/code-identity differences; it does not certify an exact upstream release replay.

## Checks and logs

Run the native tests shown in the root README and `python3 tools/release/run_checks.py`. Before publishing source changes, also run `tools/release/taint_scan.py` against your local discs and generated inputs.

Keep device logs local unless they have been checked for personal details and memory dumps. Never attach builds, imported game files, caches or saves to an issue.
