FivePocket Duo Builder {version} for macOS (Apple silicon)
========================================================

Build FivePocket Duo from your own two Xbox 360 disc images. The builder needs
Apple silicon, Xcode, at least 16 GiB of free space plus the Android NDK tools,
and your Apple identity to sign the resulting app.

In Terminal, inside the builder folder:

  ./builder fetch --accept-ndk-license
  ./builder check
  ./builder build --play-disc PLAY-DISC.iso --install-disc INSTALL-DISC.iso
  ./builder sign --ipa UNSIGNED.ipa --identity "Apple Development: you@example.com" --profile-file YOUR.mobileprovision --out SIGNED.ipa

The Android NDK supplies shader and decoder tools used by the Duo build; no Android
app is built. Read its terms before accepting them. A stopped build resumes from
its verified completed stages. Build output is under local-analysis/builder/work/out.

Keep the printed bundle identifier when signing and reinstalling. Changing it
creates a different app with empty storage. --bundle-id chooses one for a build.
The exact release toolchain is checked; explicit development builds record
code-identity differences instead of claiming an identical release.

See README-duo.txt for installation and controls. The complete source of the
builder's programs is in builder/sources/. Never share disc images, generated
code, the IPA, build folders, game files, shader caches or saves. See LEGAL.md,
COPYING and THIRD-PARTY-NOTICES.md for provenance and licence terms.
