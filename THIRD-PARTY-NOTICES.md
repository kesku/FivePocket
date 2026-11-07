# Third-party notices

FivePocket's own code is under the GNU General Public License, version 3 or later ([COPYING](COPYING)). The parts
below were written by others and keep their own terms. Full licence texts are in [LICENSES/](LICENSES/); each
download carries the texts for what is inside it in its own `LICENSES/` folder.

Nothing listed here is the game. The game's code and data come only from the player's own discs.

## 1. Android app (FivePocket, FivePocket Thor Edition APK)

| Part | Where | Terms | Text |
|---|---|---|---|
| Nunito Sans (The Nunito Sans Project Authors) | `assets/fonts/NunitoSans-VF.ttf` | SIL Open Font License 1.1 | `LICENSES/OFL-1.1-Nunito-Sans.txt` |
| AMD FidelityFX Super Resolution 1 | shaders in the game library (section 2) | MIT | `LICENSES/MIT-AMD-FidelityFX-FSR1.txt` |
| Android NDK C++ runtime (libc++, libunwind) | `lib/arm64-v8a/*.so` | Apache License 2.0 with LLVM Exceptions | `LICENSES/Apache-2.0-WITH-LLVM-exception.txt` |
| Gradle wrapper (source tree only, not in the APK) | `android/gradle/wrapper/` | Apache License 2.0 | `LICENSES/Apache-2.0.txt` |

The app shows these notices, the GPL notice and the address of the source code (https://github.com/marcm0de/FivePocket) on its About page.

## 2. Game library (made by the builder on the player's computer; never distributed)

The builder compiles these parts together with the code it translates from the player's discs. They are listed
because the player's own build contains them, and their sources are in this repository or fetched from their
publishers.

| Part | Terms | Text |
|---|---|---|
| AMD FidelityFX Super Resolution 1 (`third_party/fsr1`) | MIT | `LICENSES/MIT-AMD-FidelityFX-FSR1.txt` |
| Qualcomm Snapdragon Game Super Resolution 1 | BSD 3-Clause | `LICENSES/BSD-3-Clause-Qualcomm-GSR.txt` |
| Xenia (Ben Vanik and contributors): adapted texture, microcode and GPU logic, marked in each file | BSD 3-Clause | `LICENSES/BSD-3-Clause-Xenia.txt` |
| ReXGlue (Tom Clay): adapted texture addressing, marked in each file | BSD 3-Clause | `LICENSES/BSD-3-Clause-ReXGlue.txt` |
| FFmpeg libavcodec/libavutil (xenia-project fork, 15ece0882e8d), the XMA decoder, built from source by the builder | LGPL 2.1 or later | `LICENSES/LGPL-2.1-or-later.txt` |
| XenonRecomp support header `ppc_context.h` (hedge-dev and contributors) | MIT | `LICENSES/MIT-XenonRecomp.txt` |
| SIMDe (Evan Nemerson and contributors) | MIT | `LICENSES/MIT-SIMDe.txt` |
| Android NDK runtime | Apache 2.0 with LLVM Exceptions | `LICENSES/Apache-2.0-WITH-LLVM-exception.txt` |

Because the player builds the library from source on their own machine, the LGPL's relinking terms are met by the
sources and build scripts in this repository and in the builder download.

## 3. Builder for macOS and Windows

| Part | In | Terms | Text |
|---|---|---|---|
| XenonRecomp (hedge-dev and contributors), with `patches/xenonrecomp` | `host-tools/XenonRecomp`, `xex_image` | MIT | `LICENSES/MIT-XenonRecomp.txt` |
| GNU binutils PowerPC disassembler (inside XenonRecomp) | `XenonRecomp` | GPL 2.0 or later, used under GPL 3.0 | `LICENSES/GPL-2.0-or-later-binutils-ppc-dis-notice.txt`, `LICENSES/GPL-3.0-or-later.txt` |
| libmspack LZX decoder | `XenonRecomp`, `xmem-lzx` | LGPL 2.1 | `LICENSES/LGPL-2.1-or-later.txt` |
| tiny-AES-c | `XenonRecomp`, `xex_image` | The Unlicense | `LICENSES/Unlicense-tiny-AES-c.txt` |
| TinySHA1 | `XenonRecomp`, `xex_image` | notice in the file | `LICENSES/TinySHA1-notice.txt` |
| {fmt} | translators | MIT | `LICENSES/MIT-fmt.txt` |
| toml++ | `XenonRecomp` | MIT | `LICENSES/MIT-tomlplusplus.txt` |
| xxHash | translators | BSD 2-Clause | `LICENSES/BSD-2-Clause-xxHash.txt` |
| XenosRecomp (hedge-dev and contributors), with `patches/xenosrecomp` | `host-tools/xenos/*` | MIT | `LICENSES/MIT-XenosRecomp.txt` |
| Zstandard | `XenosRecomp` | BSD 3-Clause | `LICENSES/BSD-3-Clause-zstd.txt` |
| SMOL-V | `XenosRecomp` | public domain or MIT | `LICENSES/MIT-SMOL-V.txt` |
| Windows only: LLVM runtime (libc++, libunwind, compiler-rt) | `*.exe` | Apache 2.0 with LLVM Exceptions | `LICENSES/Apache-2.0-WITH-LLVM-exception.txt` |
| Windows only: MinGW-w64 runtime and winpthreads | `*.exe` | MinGW-w64 runtime terms | `LICENSES/MinGW-w64-runtime.txt`, `LICENSES/MinGW-w64-winpthreads.txt` |
| Windows only: CPython 3.12 embeddable | `python/` | PSF License 2.0 and its listed parts | `python/LICENSE.txt` (travels with it) |
| macOS only: DirectX Shader Compiler v1.9.2607 (Microsoft), built from its source | `host-tools/dxc` | University of Illinois/NCSA, with DXC's third-party notices | `LICENSES/NCSA-DirectXShaderCompiler.txt`, the downloaded SDK notices |
| Fetched, not distributed: Android NDK r27c (Google); DirectX Shader Compiler v1.9.2607 (Microsoft; Windows builders only) | builder cache | the publishers' terms, accepted by the player at `builder fetch` | shown by the publisher |

Each builder download contains the complete corresponding source of its host programs (the pinned upstream source
plus our patches and build scripts) in `builder/sources/`, and the notices of every part it carries in the downloaded SDK notices.

## 4. iOS and Duo apps (made by the Mac builder on the player's Mac; never distributed)

The Mac builder download carries the iOS kit these apps are built from; the app the builder makes contains:

| Part | Terms | Text |
|---|---|---|
| MoltenVK 1.4.2 (The Brenwill Workshop and contributors), official iOS SDK fetched by CMake | Apache License 2.0 | `LICENSES/Apache-2.0.txt`, the downloaded SDK notices |
| SPIRV-Cross and SPIRV-Tools (Khronos and contributors), built into MoltenVK | Apache License 2.0 | `LICENSES/Apache-2.0.txt`, the downloaded SDK notices |
| cereal (Randolph Voorhies, Shane Grant), built into MoltenVK | BSD 3-Clause | `LICENSES/BSD-3-Clause-cereal.txt`, the downloaded SDK notices |
| Vulkan headers (Khronos Group), as carried by MoltenVK | Apache License 2.0 or MIT | the downloaded SDK notices |
| FFmpeg (as in section 2), statically linked by the player's own build | LGPL 2.1 or later | `LICENSES/LGPL-2.1-or-later.txt` |
| LLVM compiler-rt | Apache 2.0 with LLVM Exceptions | `LICENSES/Apache-2.0-WITH-LLVM-exception.txt` |
| Nunito Sans, FSR 1, GSR 1, Xenia, ReXGlue, SIMDe | as above | as above |
