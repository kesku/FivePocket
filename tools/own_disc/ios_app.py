#!/usr/bin/env python3
"""The iOS app's data seed and the app folder, from the build's executable, the kit's app files and the run's outputs.

    ios_app.py payload --payload-map FILE --stage NAME=DIR [...] --out NEW_DIR
    ios_app.py app --executable FILE --app-files DIR --payload DIR --bundle-id ID --xcode DIR --out NEW_DIR/NAME.app
                   [--version TEXT] [--display-name TEXT] [--sdk-label SDK[:BUILD]] [--icon FILE.png]

payload: made before the compile, because the shell seeds Library/r only when the SHA-256 of payload/manifest.txt
equals the RECOMP_PAYLOAD_MANIFEST_SHA256 compiled into it (the CMake source target). The payload map (ios-payload/1)
names, for every file of the seed, the stage and path it comes from and its SHA-256 (the decoded image, the boot modules,
the catalogs, the shader pack), checked when it is placed, and the runtime's switch files (one byte "1" each: the app's
own settings, not game data). It writes NEW_DIR/r/<path> for each, NEW_DIR/manifest.txt ("<sha256> <bytes> <path>",
sorted by path: the same files always give the same manifest) and NEW_DIR/manifest.sha256 (the value to compile in).
Paths are below the app's data root, parts of [A-Za-z0-9._-] never starting with a dot.

app: the executable, Info.plist from the template with the player's bundle identifier, the version and the SDK fields of
the Xcode that built the executable (DTSDKName, DTPlatformVersion, DTXcode...: the SDK label decides the iOS compatibility
window, see the brief 2.4), the app files as they are (the menus' font and its licence) and the payload as
payload/r/... + payload/manifest.txt. The app files never carry the game's art or the owner's own files: a file named in
REFUSED is refused, and so is any audio or image file. --sdk-label SDK[:BUILD] (the Duo edition: iOS 27.1 so the app does
not run in the compatibility window) relabels the executable's LC_BUILD_VERSION sdk with vtool (its minos kept) and sets
DTSDKName and DTPlatformVersion to that SDK instead of the building Xcode's (as the Duo session's proven duo_relabel.py
does), and DTSDKBuild and DTPlatformBuild when BUILD is given. --icon FILE.png (the project's own app icon, 1024x1024, opaque;
given apart from the app files, which stay free of images): a single-size asset catalog compiled by the Xcode's actool
(Assets.car and the icon files it writes) and its CFBundleIcons keys merged into Info.plist. An executable that still
names itself a private or development build (PRIVATE_MARKERS, any case: a public app must not show them) is refused. Nothing is signed here (ios_sign.py); ipa_writer.py packs the
folder. Exit 0: written; 2: refused.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

REFUSED = ('alert-restart.png', 'trainer-icon.png', 'trainer-open.m4a')
MEDIA = ('.png', '.jpg', '.jpeg', '.gif', '.heic', '.m4a', '.mp3', '.wav', '.caf', '.aiff')
BUNDLE_ID = re.compile(r'^[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$')
SWITCH = b'1'


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def safe_rel(rel: str) -> bool:
    return all(p and not p.startswith('.') and re.fullmatch(r'[A-Za-z0-9._-]+', p) for p in rel.split('/'))


def build_payload(payload_map: dict, stages: dict[str, Path], out: Path) -> str:
    """writes out/r/..., out/manifest.txt, out/manifest.sha256; returns the manifest's SHA-256"""
    if out.exists():
        raise SystemExit(f'refused: {out} exists')
    if payload_map.get('schema') != 'ios-payload/1':
        raise SystemExit('refused: the payload map is not ios-payload/1')
    files, switches = payload_map.get('files', {}), payload_map.get('switches', [])
    names = list(files) + switches
    if len(set(names)) != len(names) or not all(safe_rel(n) for n in names):
        raise SystemExit('refused: a payload path is repeated or not plain (parts of [A-Za-z0-9._-], none starting with a dot)')
    problems = []
    for rel, src in files.items():
        path = stages.get(src['stage'], Path('/nonexistent')) / src['path']
        if not path.is_file() or sha256(path) != src['sha256']:
            problems.append(f'stage {src["stage"]}: {src["path"]} is missing or not the expected file')
    if problems:
        raise SystemExit('refused:\n' + '\n'.join(problems[:20]))
    out.mkdir(parents=True)
    lines = []
    for rel in sorted(names):
        dst = out / 'r' / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if rel in files:
            subprocess.run(['/bin/cp', '-c', str(stages[files[rel]['stage']] / files[rel]['path']), str(dst)], check=True)
        else:
            dst.write_bytes(SWITCH)
        lines.append(f'{sha256(dst)} {dst.stat().st_size} {rel}\n')
    (out / 'manifest.txt').write_text(''.join(lines), encoding='utf-8', newline='\n')
    digest = sha256(out / 'manifest.txt')
    (out / 'manifest.sha256').write_text(digest + '\n', encoding='utf-8', newline='\n')
    return digest


def xcode_fields(xcode: Path) -> dict:
    env = dict(os.environ, DEVELOPER_DIR=str(xcode))
    run = lambda *a: subprocess.run(list(a), capture_output=True, text=True, env=env, check=True).stdout.strip()
    version, build = run('/usr/bin/xcodebuild', '-version').splitlines()[:2]
    number = version.split()[-1].split('.')
    sdk = run('/usr/bin/xcrun', '--sdk', 'iphoneos', '--show-sdk-version')
    sdk_build = run('/usr/bin/xcrun', '--sdk', 'iphoneos', '--show-sdk-build-version')
    return {'DTXcode': f'{int(number[0]):02d}{int(number[1]) if len(number) > 1 else 0}{int(number[2]) if len(number) > 2 else 0}',
            'DTXcodeBuild': build.split()[-1], 'DTSDKName': f'iphoneos{sdk}', 'DTPlatformVersion': sdk,
            'DTSDKBuild': sdk_build, 'DTPlatformBuild': sdk_build}


def compile_icon(icon: Path, xcode: Path, app: Path, minimum: str) -> dict:
    """the app icon as Xcode builds it from a single-size 1024 catalog entry: Assets.car + icon files into the app; returns the
    Info.plist keys actool writes for the iPhone app"""
    import tempfile
    head = icon.read_bytes()[:33]
    if head[:8] != b'\x89PNG\r\n\x1a\n' or int.from_bytes(head[16:20], 'big') != 1024 or int.from_bytes(head[20:24], 'big') != 1024:
        raise SystemExit(f'refused: {icon} is not a 1024x1024 PNG')
    if head[25] in (4, 6):                                # grey+alpha or RGBA: an app icon must be opaque
        raise SystemExit(f'refused: {icon} has an alpha channel (an app icon is opaque)')
    with tempfile.TemporaryDirectory() as tmp:
        cat = Path(tmp) / 'Icon.xcassets'; (cat / 'AppIcon.appiconset').mkdir(parents=True)
        shutil.copyfile(icon, cat / 'AppIcon.appiconset' / 'icon.png')
        (cat / 'Contents.json').write_text('{"info":{"author":"xcode","version":1}}\n', encoding='utf-8')
        (cat / 'AppIcon.appiconset' / 'Contents.json').write_text(json.dumps({'images': [
            {'filename': 'icon.png', 'idiom': 'universal', 'platform': 'ios', 'size': '1024x1024'}],
            'info': {'author': 'xcode', 'version': 1}}) + '\n', encoding='utf-8')
        partial = Path(tmp) / 'partial.plist'
        r = subprocess.run(['/usr/bin/xcrun', 'actool', str(cat), '--compile', str(app), '--platform', 'iphoneos',
                            '--minimum-deployment-target', minimum, '--app-icon', 'AppIcon', '--output-partial-info-plist',
                            str(partial), '--target-device', 'iphone', '--output-format',
                            'human-readable-text'], capture_output=True, text=True, env=dict(os.environ, DEVELOPER_DIR=str(xcode)))
        if r.returncode or not partial.is_file() or not (app / 'Assets.car').is_file():
            raise SystemExit(f'refused: actool did not build the icon: {(r.stdout + r.stderr).strip()[-400:]}')
        with partial.open('rb') as f:
            return plistlib.load(f)


PRIVATE_MARKERS = (b'private build', b'apple port', b'v507 apple')     # main 2026-11-05: never in a public app's text


def private_markers(data: bytes) -> list[str]:
    low = data.lower()                  # (lowers ASCII letters anywhere, so UTF-16LE text too)
    # an Objective-C @"..." literal with a non-ASCII character is stored as UTF-16LE (the Duo session's find, 2026-11-05)
    return [m.decode() for m in PRIVATE_MARKERS if m in low or m.decode().encode('utf-16le') in low]


def build_app(executable: Path, app_files: Path, payload: Path, bundle_id: str, xcode: Path, out: Path,
              version: str | None = None, display_name: str | None = None, sdk_label: str | None = None,
              icon: Path | None = None) -> dict:
    if out.exists() or out.suffix != '.app':
        raise SystemExit(f'refused: {out} exists or is not a .app folder name')
    if not BUNDLE_ID.match(bundle_id):
        raise SystemExit(f'refused: {bundle_id!r} is not a bundle identifier (reverse-DNS, letters, digits, hyphens)')
    if not (payload / 'manifest.txt').is_file() or not (payload / 'r').is_dir():
        raise SystemExit(f'refused: {payload} is no payload (ios_app.py payload)')
    found = private_markers(Path(executable).read_bytes())
    if found:
        raise SystemExit(f'refused: the executable still shows private-build text ({", ".join(found)}): build it from the public sources')
    with (app_files / 'Info.plist').open('rb') as f:
        info = plistlib.load(f)
    extra = [p for p in sorted(app_files.rglob('*')) if p.is_file() and p.name != 'Info.plist']
    for p in extra:
        if p.name in REFUSED or p.suffix.lower() in MEDIA:
            raise SystemExit(f'refused: {p.name} is art or sound, which the app files must not carry')
    name = info['CFBundleExecutable']
    if sdk_label:
        sdk, _, sdk_build = sdk_label.partition(':')
        if not re.fullmatch(r'\d+\.\d+(\.\d+)?', sdk) or (sdk_build and not re.fullmatch(r'[0-9A-Za-z]+', sdk_build)):
            raise SystemExit(f'refused: --sdk-label {sdk_label!r} is not SDK[:BUILD] (for example 27.1)')
        env = dict(os.environ, DEVELOPER_DIR=str(xcode))
        shown = subprocess.run(['/usr/bin/xcrun', 'vtool', '-show-build', str(executable)], capture_output=True, text=True,
                               env=env, check=True).stdout
        minos = re.findall(r'^\s*minos (\S+)$', shown, re.M)
        if len(minos) != 1:
            raise SystemExit('refused: the executable has no single LC_BUILD_VERSION to relabel')
        out.mkdir(parents=True)
        subprocess.run(['/usr/bin/xcrun', 'vtool', '-set-build-version', 'ios', minos[0], sdk, '-replace', '-output',
                        str(out / name), str(executable)], capture_output=True, text=True, env=env, check=True)
    else:
        out.mkdir(parents=True)
        shutil.copyfile(executable, out / name)
    (out / name).chmod(0o755)
    for p in extra:
        dst = out / p.relative_to(app_files)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(p, dst)
    (out / 'payload').mkdir()
    subprocess.run(['/bin/cp', '-c', '-R', str(payload / 'r'), str(out / 'payload')], check=True)
    shutil.copyfile(payload / 'manifest.txt', out / 'payload' / 'manifest.txt')
    info.update(xcode_fields(xcode))
    if sdk_label:
        info.update(DTSDKName=f'iphoneos{sdk}', DTPlatformVersion=sdk)
        if sdk_build:
            info.update(DTSDKBuild=sdk_build, DTPlatformBuild=sdk_build)
    info['CFBundleIdentifier'] = bundle_id
    if version:
        info['CFBundleVersion'] = info['CFBundleShortVersionString'] = version
    if display_name:
        info['CFBundleDisplayName'] = info['CFBundleName'] = display_name
    if icon is not None:
        info.update(compile_icon(icon, xcode, out, info.get('MinimumOSVersion', '17.4')))
    info['UIDeviceFamily'] = [1]
    for key in list(info):
        if key.endswith('~ipad'):
            del info[key]
    info.pop('BuildMachineOSBuild', None)                 # the building Mac's own system: not a property of the app
    with (out / 'Info.plist').open('wb') as f:
        plistlib.dump(info, f, fmt=plistlib.FMT_BINARY, sort_keys=True)
    (out / 'PkgInfo').write_bytes(b'APPL????')
    lines = (out / 'payload' / 'manifest.txt').read_text(encoding='utf-8').count('\n')
    return {'app': str(out), 'payload_files': lines, 'bundle_id': bundle_id, 'sdk': info['DTSDKName']}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='command', required=True)
    pl = sub.add_parser('payload')
    pl.add_argument('--payload-map', type=Path, required=True)
    pl.add_argument('--stage', action='append', default=[])
    pl.add_argument('--out', type=Path, required=True)
    app = sub.add_parser('app')
    app.add_argument('--executable', type=Path, required=True)
    app.add_argument('--app-files', type=Path, required=True)
    app.add_argument('--payload', type=Path, required=True)
    app.add_argument('--bundle-id', required=True)
    app.add_argument('--xcode', type=Path, required=True)
    app.add_argument('--out', type=Path, required=True)
    app.add_argument('--version')
    app.add_argument('--display-name')
    app.add_argument('--sdk-label')
    app.add_argument('--icon', type=Path)
    a = ap.parse_args(argv)
    if a.command == 'payload':
        stages = {k: Path(v) for k, _, v in (item.partition('=') for item in a.stage)}
        digest = build_payload(json.loads(a.payload_map.read_text(encoding='utf-8')), stages, a.out)
        count = (a.out / 'manifest.txt').read_text(encoding='utf-8').count('\n')
        print(f'PAYLOAD {count} files, manifest sha256 {digest}', flush=True)
        return 0
    r = build_app(a.executable, a.app_files, a.payload, a.bundle_id, a.xcode, a.out, a.version, a.display_name, a.sdk_label, a.icon)
    print(f'APP {Path(r["app"]).name}: {r["payload_files"]} payload files, bundle id {r["bundle_id"]}, {r["sdk"]}', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
