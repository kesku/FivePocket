#!/usr/bin/env python3
"""Check public names: no product, app, id, download or player-visible name may use the game's or its makers' names.

    scan_names.py [ROOT]           (default: the repository this file is in)

A name fails when it contains one of the words in FORBIDDEN ("gta" also covers gta5 and gtav; "grandtheft" covers
"grand theft"), ignoring case and separators ("G-T-A" and "Gta_5" fail too). The rule (maintainers, 2026-11-05):
must be clean
- ROOT's own name (a download's or hand-out's folder) and the names of its immediate children, files and folders:
  what a player sees on opening it;
- the name of every handed-out file type anywhere below it (PACKAGE_SUFFIXES: .apk, .ipa, .bundle, .zip, .dmg, .tar
  and other archives, apps and installers);
- app and product names: Android labels (res/values*/strings.xml), iOS CFBundleName / CFBundleDisplayName, the first
  heading of README.md, of each packaging/README-*.txt and of a download's README.txt;
- application and bundle ids (applicationId, the manifest package, CFBundleIdentifier, PRODUCT_BUNDLE_IDENTIFIER);
- folder names the app shows the player, written as text in any file ("Pictures/..." and the like);
allowed
- internal source file and folder names deeper inside (runtime/host/gta5_*.h, runtime headers);
- descriptive text such as "an Xbox 360 disc of the game" inside docs and messages: it is not a name.
Also fails on an unfilled release placeholder (a word ending in -TO-BE-SET).
Exit 0: PASS. 1: FAIL (each failing name is printed). 2: ROOT missing.
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

FORBIDDEN = ('gta', 'grandtheft', 'rockstar', 'taketwo', 'lossantos')
PACKAGE_SUFFIXES = {'.apk', '.aab', '.ipa', '.bundle', '.zip', '.dmg', '.tar', '.xz', '.gz', '.tgz', '.7z', '.app',
                    '.exe', '.msi', '.pkg'}
IDS = re.compile(r'(?:applicationId(?:Suffix)?\s*=?\s*"([^"]+)"|\bpackage="([^"]+)"'
                 r'|<key>CFBundleIdentifier</key>\s*<string>([^<]*)</string>|PRODUCT_BUNDLE_IDENTIFIER\s*=\s*"?([^;"\s]+))')
VISIBLE_FOLDER = re.compile(r'\b(?:Pictures|Movies|DCIM|Documents|Download|Music)/([^"\'\n/]{1,60})')
PLIST_NAME = re.compile(r'<key>CFBundle(?:Display)?Name</key>\s*<string>([^<]*)</string>')
PLACEHOLDER = re.compile(r'\b[A-Z][A-Z-]*-TO-BE-SET\b')
SELF = {'tools/release/scan_names.py', 'tools/release/test_release_checks.py'}  # they name what they look for


def squash(text: str) -> str:
    return re.sub(r'[^a-z0-9]', '', text.lower())


def hits(text: str):
    flat = squash(text)
    return [w for w in FORBIDDEN if w in flat]


def files(root: Path):
    if (root / '.git').exists():
        out = subprocess.run(['git', '-C', str(root), 'ls-files', '-z'], capture_output=True, check=True)
        names = [n for n in out.stdout.decode().split('\0') if n]
        if names:
            return names
    return sorted(str(p.relative_to(root)) for p in root.rglob('*') if p.is_file() and '.git' not in p.parts)


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    root = Path(argv[0] if argv else Path(__file__).resolve().parents[2])
    if not root.is_dir():
        print(f'scan_names: no such folder: {root}', file=sys.stderr)
        return 2
    failures = []
    if hits(root.resolve().name):
        failures.append(f'NAME {root.resolve().name}: the download or folder name contains {",".join(hits(root.resolve().name))}')
    for rel in files(root):
        parts = Path(rel).parts
        # Each path part is judged on its own so a word split across two folder names is not invented.
        for i, part in enumerate(parts):
            found = hits(part)
            visible = i == 0 or (i == len(parts) - 1 and Path(part).suffix.lower() in PACKAGE_SUFFIXES)
            if found and visible:
                failures.append(f'NAME {rel}: "{part}" contains {",".join(found)}')
                break
        path = root / rel
        if Path(rel).as_posix() in SELF or not path.is_file() or path.stat().st_size > 8 << 20:
            continue
        data = path.read_bytes()
        if b'\0' in data[:8192]:
            continue
        text = data.decode('utf-8', errors='replace')
        for m in VISIBLE_FOLDER.finditer(text):
            if hits(m.group(1)):
                failures.append(f'FOLDER {rel}:{text.count(chr(10), 0, m.start()) + 1}: player-visible folder "{m.group(0)}"')
        for m in PLIST_NAME.finditer(text):
            if hits(m.group(1)):
                failures.append(f'LABEL {rel}: iOS name "{m.group(1)}"')
        for m in IDS.finditer(text):
            ident = next(g for g in m.groups() if g is not None)
            if hits(ident):
                failures.append(f'ID {rel}:{text.count(chr(10), 0, m.start()) + 1}: "{ident}"')
        for m in PLACEHOLDER.finditer(text):
            failures.append(f'PLACEHOLDER {rel}:{text.count(chr(10), 0, m.start()) + 1}: {m.group(0)} is not filled in')
    for strings in root.glob('android/app/src/*/res/values*/strings.xml'):
        for m in re.finditer(r'<string name="([^"]+)">([^<]*)</string>', strings.read_text(errors='replace')):
            found = hits(m.group(2))
            if found:
                failures.append(f'LABEL {strings.relative_to(root)} {m.group(1)}="{m.group(2)}" contains {",".join(found)}')
    readme = root / 'README.md'
    if readme.exists():
        heading = next((l for l in readme.read_text(errors='replace').splitlines() if l.startswith('#')), '')
        if hits(heading):
            failures.append(f'TITLE README.md "{heading}"')
    for template in sorted(root.glob('packaging/README-*.txt')) + [root / 'README.txt']:
        if template.is_file():
            first = (template.read_text(errors='replace').splitlines() or [''])[0]
            if hits(first):
                failures.append(f'TITLE {template.relative_to(root)} "{first}"')
    for f in failures:
        print(f)
    print(f'NAMES {"PASS" if not failures else "FAIL"} failures={len(failures)}')
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
