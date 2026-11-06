# Contributing

Thank you for helping. Three rules come before everything else:

1. **No game content, ever.** Do not commit, attach, paste or link anything from the never-share table in
   [LEGAL.md](LEGAL.md): disc files, the decoded executable, generated code, shader packs or caches, bundles, saves,
   memory dumps. Not in code, tests, issues, pull requests or screenshots of hex views.
2. **No keys or personal details.** No console keys, signing keys, tokens, home folder paths, device serials or
   private network addresses.
3. **No game names in product names.** Apps, app labels, downloads, folders and anything the player sees use
   FivePocket names. Internal source files may keep a descriptive name (`tools/release/scan_names.py` says exactly
   what is checked).

Before a pull request, run:

```
python3 tools/release/run_checks.py
```

It runs the name, secret and personal-detail checks and the release-check tests. If you have your own discs and a
builder work folder, also run the taint scan on your change:

```
python3 tools/release/taint_scan.py --published <changed files> --retail <your disc images and builder outputs>
```

## Code

- C++20 for native code, Java 17 for the Android app, Python 3.12 for tools.
- Keep the style of the file you are editing. New files carry `SPDX-License-Identifier: GPL-3.0-or-later` unless they
  adapt code under another licence, which then must be named in the file and in THIRD-PARTY-NOTICES.md.
- Add a host test that fails without your change when you fix a bug.
- By sending a contribution you agree that it is published under the GPL, version 3 or later.

## Reporting a bug

Use this fork's issue tracker for iPhone Duo problems. Include the app version, iOS version and steps to reproduce. Check logs for personal details and memory dumps before sharing them.

Never attach disc images, generated code, builds, imported assets, caches or saves. See [LEGAL.md](LEGAL.md) and [build notes](docs/BUILDING.md#checks-and-logs).
