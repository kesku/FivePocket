#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the source app icon and merge actool's metadata into the built bundle."""
import argparse
import os
from pathlib import Path
import plistlib
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/own_disc'))
from ios_app import compile_icon, xcode_fields


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--icon', type=Path, required=True)
    parser.add_argument('--minimum', required=True)
    parser.add_argument('--developer', default='')
    args = parser.parse_args()
    developer = args.developer or os.environ.get('DEVELOPER_DIR') or subprocess.check_output(
        ['/usr/bin/xcode-select', '-p'], text=True).strip()
    metadata = compile_icon(args.icon, Path(developer), args.app, args.minimum)
    info = args.app / 'Info.plist'
    with info.open('rb') as stream:
        values = plistlib.load(stream)
    values.update(metadata)
    values.update(xcode_fields(Path(developer)))
    with info.open('wb') as stream:
        plistlib.dump(values, stream)


if __name__ == '__main__':
    main()
