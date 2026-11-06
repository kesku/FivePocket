# Legal

This page is not legal advice. It states what the FivePocket project does and what it asks of everyone who uses or
forks it.

## Not affiliated

FivePocket is an independent, non-commercial fan project. It is not affiliated with, endorsed by, sponsored by or
connected to the publisher or the developer of the game it runs, or to Microsoft, Apple, Google, AYN, Samsung,
Qualcomm or AMD.

## Trademarks

All product names, logos and brands are the property of their owners. Names of games, consoles, devices,
operating systems and technologies appear only to say what FivePocket is compatible with or which disc it reads.
"Xbox" and "Xbox 360" are trademarks of Microsoft. "iOS", "iPad", "iPhone", "Mac" and "Xcode" are trademarks of
Apple. "Android" is a trademark of Google. "Thor" in "FivePocket Thor Edition" refers to the AYN Thor device. "AMD
FidelityFX" is a trademark of AMD; "Snapdragon" is a trademark of Qualcomm. FivePocket uses no game logos, artwork,
music or other trademarked material of the game, apart from screenshots of our own play from discs we own in the
README.

## Bring your own discs

FivePocket contains **no** code, data, artwork, audio, video, text or shaders from the game. To use it you need the
Xbox 360 discs of the game that you own. The builder runs on your own computer and reads only the disc images you
give it; the app reads only the files you import into it. Using FivePocket with a copy you do not own is not
permitted by this project.

The builder decrypts and decompresses the game's executable from your disc on your machine because the code cannot
be translated otherwise. The key this needs comes with upstream XenonRecomp, not from FivePocket's own files (see
[Keys](#keys)).

## Never share what you build

Everything the builder and the app make from your discs is derived from the game. Never share, upload, sell or
publish any of it:

| Never share | What it is |
|---|---|
| disc images (`.iso`) and files copied off the discs | the game |
| `*.bundle` (the builder's output) | the game's code translated and compiled for your device, with its shaders |
| the `.ipa` or `.app` the builder makes for iPad and iPhone | the same, inside the app |
| the builder's work folder | the decoded executable, generated source code, shader packs |
| the app's game-files folder and caches (shader, pipeline) | game data and translated shaders |
| saves | game data |
| logs that contain memory dumps | parts of the game's memory |

Ordinary logs are fine to share (see [build notes](docs/BUILDING.md#checks-and-logs)). Screenshots and
videos of your own play are your own captures.

## Keys

FivePocket's own files contain no decryption keys, signing keys or other key material. The builder uses upstream
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (MIT), pinned at commit `16ee5918` plus this project's
published patch, as upstream publishes it: upstream's own source declares the key needed to read the executable on
your disc, and the builder's `XenonRecomp` and `xex_image` programs are built from that source.

Release checks (`tools/release/scan_secrets.py`) fail if a known console key, a private key or a token appears in
anything we publish. The only exception is those exact upstream files, each listed by SHA-256 with its reason in
`tools/release/upstream-key-allow.txt`; a changed byte fails the check again. Only SHA-256 digests of keys are kept,
never the keys.

## Clean room

FivePocket's code is written by its contributors or taken from the open-source projects named in
[CREDITS.md](CREDITS.md) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). No leaked, stolen or otherwise
unlicensed source code of the game or of any console SDK was used. Behaviour of the game is understood from the
player's own copy of the game: running it, and reading the code the translator produces from it on our own machines.
Generated code, decoded executables and shader packs are never committed; `tools/release/taint_scan.py` checks
every published file against the game's bytes before a release.

## Forks and redistribution

You may fork and redistribute FivePocket under the GPL. To keep forks clean:

1. Never commit or attach anything from the [never-share table](#never-share-what-you-build), not even "for testing".
2. Run the release checks before you publish a fork or a download:
   `tools/release/scan_names.py`, `tools/release/scan_secrets.py`, `tools/release/scan_owner.py`, and
   `tools/release/taint_scan.py` against your own discs and builder output.
3. Do not name your fork or its downloads after the game, its publisher or its developer, and do not use their
   artwork or logos.
4. Do not sell it or put it behind a payment, and do not bundle game files with it.
5. Keep the licence and notices (COPYING, LICENSES/, THIRD-PARTY-NOTICES.md, CREDITS.md) and mark your changes.

## Reporting content

If you believe this repository or a FivePocket download contains material it must not contain (game content, a key,
someone's personal data), or you hold rights and want something removed:

- open an issue with the **Content report** form, naming the file and line or the download and the file inside it,
  **without attaching or quoting the material**; or
- write to the contact in [SECURITY.md](SECURITY.md) if the report should not be public.

We remove confirmed material promptly and rebuild affected downloads.

## Licence

FivePocket's own code: GNU General Public License, version 3 or (at your option) any later version. See
[COPYING](COPYING), [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and [LICENSES/](LICENSES/).
