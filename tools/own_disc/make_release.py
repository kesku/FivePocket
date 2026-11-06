#!/usr/bin/env python3
"""Write a verified Mac Duo builder inventory (builder-release/1).

    make_release.py --package DIR --profile REL [--python REL]
                    --ndk-url URL --ndk-sha256 HEX --ndk-size N
                    --ndk-directory NAME --ndk-unpacked-bytes N
                    [--ndk-image-root PATH] --min-free-bytes N
                    [--build-time TEXT] [--output FILE]

Every ordinary package file is hashed. Symlinks and unsafe archive names are
refused. The profile is published as the Duo target; other product targets are
not created. Mac shader-compiler files travel inside the verified payload.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import builder  # noqa: E402
import disc_builder as db  # noqa: E402

SKIPPED_FOLDERS = ('__pycache__',)
LICENSE_URL = 'https://developer.android.com/ndk/downloads'


def is_program(path: Path, host: str) -> bool:
    return bool(path.stat().st_mode & 0o111)


def inventory(package: Path, host: str) -> list:
    files = []
    for path in sorted(package.rglob('*'), key=lambda p: p.relative_to(package).as_posix()):
        rel = path.relative_to(package)
        if any(part in SKIPPED_FOLDERS for part in rel.parts):
            continue
        if path.is_symlink():
            raise ValueError(f'a link is in the package: {rel.as_posix()}')
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError(f'not a plain file: {rel.as_posix()}')
        name = rel.as_posix()
        try:
            builder.safe_name(name)
        except builder.Refusal:
            raise ValueError(f'the front end would refuse this name: {name}') from None
        record = {'path': name, 'sha256': db.sha256_file(path)}
        if is_program(path, host):
            record['executable'] = True
        files.append(record)
    if not files:
        raise ValueError('the package is empty')
    return files




def make(package: Path, host: str, profile: str, python: str, ndk: dict, min_free_bytes: int, build_time: str | None, output: Path,
         targets: dict | None = None) -> dict:
    package = package.resolve()
    output = output.resolve()
    if not package.is_dir():
        raise ValueError('the package folder is not there')
    if output.exists():
        raise ValueError('the inventory exists already: nothing is written over')
    if output.parent != package:
        raise ValueError('the inventory lies at the package root (the files are named from its folder)')
    if type(min_free_bytes) is not int or min_free_bytes <= 0:
        raise ValueError('the free space a build needs is a positive number of bytes')
    release = {'schema': 'builder-release/1', 'host': host, 'profile': profile, **({'python': python} if python else {}),
               'ndk': dict(ndk, license_url=LICENSE_URL),
               'estimates': {'min_free_bytes': min_free_bytes, **({'build_time': build_time} if build_time else {})},
               'files': inventory(package, host)}
    listed = {record['path'] for record in release['files']}
    if targets and set(targets) != {'duo'}:
        raise ValueError('only the duo target is supported')
    release['profiles'] = dict(targets or {'duo': profile})
    for what, name in [('profile', profile)] + ([('Python', python)] if python else []) + [(f'{t} profile', r) for t, r in sorted((targets or {}).items())]:
        if name not in listed:
            raise ValueError(f'the {what} is not a file of the package: {name}')
    try:
        builder.ndk_spec(release)
    except builder.Refusal as error:
        raise ValueError(f'the compiler archive is not pinned as the front end wants it: {error}') from None
    with output.open('x', encoding='utf-8', newline='\n') as stream:
        stream.write(json.dumps(release, indent=1, sort_keys=True) + '\n')
    if host == builder.host_id():           # the consumer's own reader, where it can run
        try:
            builder.load_release(output)
        except builder.Refusal as error:
            output.unlink()
            raise ValueError(f'the front end refuses the inventory just written: {error}') from None
    return release


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--package', type=Path, required=True)
    ap.add_argument('--host', choices=['darwin-arm64'], default='darwin-arm64')
    ap.add_argument('--profile', required=True)
    ap.add_argument('--python')
    ap.add_argument('--ndk-url', required=True)
    ap.add_argument('--ndk-sha256', required=True)
    ap.add_argument('--ndk-size', type=int, required=True)
    ap.add_argument('--ndk-directory', required=True)
    ap.add_argument('--ndk-unpacked-bytes', type=int, required=True)
    ap.add_argument('--ndk-image-root')
    ap.add_argument('--min-free-bytes', type=int, required=True)
    ap.add_argument('--build-time')
    ap.add_argument('--output', type=Path)
    ap.add_argument('--target', action='append', default=[], metavar='duo=REL')
    a = ap.parse_args(argv)
    ndk = {'url': a.ndk_url, 'sha256': a.ndk_sha256, 'size': a.ndk_size, 'directory': a.ndk_directory, 'unpacked_bytes': a.ndk_unpacked_bytes}
    if a.ndk_image_root:
        ndk['image_root'] = a.ndk_image_root
    try:
        if any('=' not in item for item in a.target):
            raise ValueError('--target wants NAME=REL')
        targets = dict(item.split('=', 1) for item in a.target)
        release = make(a.package, a.host, a.profile, a.python, ndk, a.min_free_bytes, a.build_time, a.output or a.package / 'release.json', targets)
    except (ValueError, OSError) as error:
        print('RELEASE_REFUSED:', error, file=sys.stderr)
        return 2
    programs = sum(1 for record in release['files'] if record.get('executable'))
    print('RELEASE', len(release['files']), 'files,', programs, 'programs, host', release['host'], flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
