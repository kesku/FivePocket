#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the Duo host tools and Apple audio decoder from pinned source."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

import build_xma_decoder
import ndk
from host_support import file_sha256

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def run(command, **kwargs):
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def load_lock(path=HERE / 'sources.lock.json'):
    lock = json.loads(path.read_text(encoding='utf-8'))
    if lock.get('schema') != 'duo-source-lock/1':
        raise ValueError('unsupported source lock')
    for source in lock['sources'].values():
        revision = source['revision']
        if len(revision) != 40 or any(c not in '0123456789abcdef' for c in revision):
            raise ValueError('source revisions must be exact Git commits')
    return lock


def checkout(source, workspace):
    path = workspace / source['directory']
    if not path.exists():
        run(['git', 'clone', '--filter=blob:none', '--no-checkout', source['url'], path])
        run(['git', '-C', path, 'fetch', 'origin', source['revision']])
        run(['git', '-C', path, 'checkout', '--detach', source['revision']])
    revision = run(['git', '-C', path, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    if revision != source['revision']:
        raise ValueError(f'{path.name} is at another revision; use a fresh workspace')
    dirty = run(['git', '-C', path, 'status', '--porcelain', '--untracked-files=no'], capture_output=True, text=True).stdout
    if dirty:
        raise ValueError(f'{path.name} has local changes; use a fresh workspace')
    if source['submodules']:
        run(['git', '-C', path, 'submodule', 'update', '--init', '--recursive', '--', *source['submodules']])
    return path


def verify_vendored():
    path = ROOT / 'third_party/xenonrecomp'
    record = json.loads((path / 'SOURCE.json').read_text(encoding='utf-8'))
    for name, digest in record['files'].items():
        if file_sha256(path / name) != digest:
            raise ValueError('vendored generator source differs: ' + name)
    return path


def copy_git_source(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    archive = destination.with_suffix('.tar')
    with archive.open('wb') as output:
        run(['git', '-C', source, 'archive', '--format=tar', 'HEAD'], stdout=output)
    destination.mkdir(parents=True)
    with tarfile.open(archive) as data:
        data.extractall(destination, filter='data')
    archive.unlink()


def materialize(workspace, sources, lock):
    vendor = verify_vendored()
    patches = HERE / 'source-patches'
    identity = hashlib.sha256((vendor / 'SOURCE.json').read_bytes())
    identity.update(json.dumps(lock,sort_keys=True).encode())
    for path in sorted(patches.glob('*.patch')):
        identity.update(path.name.encode())
        identity.update(path.read_bytes())
    stage = workspace / 'materialized' / identity.hexdigest()[:16]
    if (stage / 'complete').is_file():
        return stage
    if stage.exists():
        raise ValueError('incomplete source preparation; remove it or use a fresh workspace: ' + str(stage))
    xenon = stage / 'xenon'
    shutil.copytree(vendor, xenon)
    for name in ('fmt', 'xxHash', 'tomlplusplus', 'libmspack', 'tiny-AES-c', 'simde'):
        (xenon / 'thirdparty' / name).symlink_to(sources['xenon-dependencies'] / 'thirdparty' / name, target_is_directory=True)
    for mode in ['v41', *lock['xenos_variants']]:
        folder = stage / 'xenos' / mode
        copy_git_source(sources['xenos'], folder)
        run(['git', 'apply', patches / 'xenos-base.patch'], cwd=folder)
        if mode != 'v41':
            run(['git', 'apply', patches / f'xenos-{mode}.patch'], cwd=folder)
        for name in ('fmt', 'xxHash', 'zstd', 'smol-v'):
            existing = folder / 'thirdparty' / name
            if existing.is_dir():
                existing.rmdir()
            existing.symlink_to(sources['xenos'] / 'thirdparty' / name, target_is_directory=True)
    (stage / 'complete').write_text('source preparation complete\n', encoding='utf-8', newline='\n')
    return stage


def build_dxc(workspace, source, jobs):
    build = workspace / 'build-dxc'
    run(['cmake', '-S', source, '-B', build, '-G', 'Ninja', '-C', source / 'cmake/caches/PredefinedParams.cmake',
         '-DCMAKE_BUILD_TYPE=Release', '-DLLVM_ENABLE_ASSERTIONS=OFF', '-DLLVM_INCLUDE_TESTS=OFF',
         '-DCLANG_INCLUDE_TESTS=OFF', '-DHLSL_INCLUDE_TESTS=OFF', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'])
    run(['cmake', '--build', build, '--target', 'dxc', 'dxcompiler', '--parallel', jobs])
    return build


def build_host_tools(workspace, sources, lock, jobs):
    stage = materialize(workspace, sources, lock)
    dxc = build_dxc(workspace, sources['dxc'], jobs)
    output = workspace / 'host-tools'
    (output / 'dxc/bin').mkdir(parents=True, exist_ok=True)
    (output / 'dxc/lib').mkdir(exist_ok=True)
    shutil.copy2(dxc / 'bin/dxc', output / 'dxc/bin/dxc-3.7')
    shutil.copy2(dxc / 'lib/libdxcompiler.dylib', output / 'dxc/lib/libdxcompiler.dylib')
    run(['cmake', '-S', HERE / 'host_tools', '-B', workspace / 'build-host-tools', '-G', 'Ninja',
         '-DCMAKE_BUILD_TYPE=Release', f'-DXENON_SOURCE={stage / "xenon"}',
         f'-DXENOS_SOURCE_ROOT={stage / "xenos"}', f'-DDXC_SOURCE={sources["dxc"]}',
         f'-DDXC_LIBRARY={output / "dxc/lib/libdxcompiler.dylib"}', f'-DHOST_OUTPUT={output}'])
    run(['cmake', '--build', workspace / 'build-host-tools', '--parallel', jobs])
    shutil.copy2(stage / 'xenon/XenonUtils/ppc_context.h', output / 'ppc_context.h')
    return output


def ffmpeg_inputs(plan,archive,xcode):
    compiler=xcode/'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
    return {'source_archive':file_sha256(archive),'plan':plan,
            'configured':{name:file_sha256(HERE/'xma_decoder/configured-apple'/name) for name in plan['configured']},
            'compiler':file_sha256(compiler)}


def build_ffmpeg(workspace, source, xcode, jobs):
    archive = workspace / 'ffmpeg-source.tar'
    candidate=archive.with_suffix('.tar.part')
    with candidate.open('wb') as output:
        run(['git', '-C', source, 'archive', '--format=tar', 'HEAD'], stdout=output)
    if archive.exists():
        if file_sha256(candidate)!=file_sha256(archive):
            candidate.unlink()
            raise ValueError('cached FFmpeg source archive differs from its pinned Git tree')
        candidate.unlink()
    else:
        candidate.replace(archive)
    plan = json.loads((HERE / 'xma_decoder/plan-apple.json').read_text(encoding='utf-8'))
    plan['source_archive_sha256'] = file_sha256(archive)
    inputs=ffmpeg_inputs(plan,archive,xcode)
    output = workspace / 'ffmpeg-apple'
    if output.exists():
        receipt=output/'source-inputs.json'
        if not receipt.is_file() or json.loads(receipt.read_text(encoding='utf-8'))!=inputs:
            raise ValueError('audio source/compiler inputs changed or lack a receipt; use a fresh workspace')
        report = json.loads((output / 'report.json').read_text(encoding='utf-8'))
        for name, item in report['archives'].items():
            if file_sha256(output / name) != item['sha256']:
                raise ValueError('changed audio library: ' + name)
        return output
    build_xma_decoder.build(plan, archive, HERE / 'xma_decoder/configured-apple', None, output,
                            jobs=jobs, xcode=xcode)
    (output/'source-inputs.json').write_text(json.dumps(inputs,indent=2)+'\n',encoding='utf-8',newline='\n')
    return output


def ensure_ndk(cache, accepted=False):
    release={'ndk':load_lock()['ndk']}
    target=cache/release['ndk']['sha256'][:16]/'installed.json'
    if not target.exists():
        ndk.fetch(release,cache,accepted)
    return ndk.existing_ndk(cache,release['ndk'])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['fetch', 'host-tools', 'ffmpeg', 'ndk', 'all'])
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--xcode', type=Path, default=os.environ.get('DEVELOPER_DIR'))
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--ndk-cache',type=Path)
    parser.add_argument('--accept-ndk-license',action='store_true')
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    workspace = args.work.resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    lock = load_lock()
    needed = [] if args.command=='ndk' else ['ffmpeg'] if args.command == 'ffmpeg' else list(lock['sources'])
    sources = {key: checkout(lock['sources'][key], workspace) for key in needed}
    result = {'sources': {key: lock['sources'][key]['revision'] for key in sources}}
    if args.command in ('host-tools', 'all'):
        result['host_tools'] = str(build_host_tools(workspace, sources, lock, args.jobs))
    if args.command in ('ffmpeg', 'all'):
        if args.xcode is None:
            parser.error('--xcode or DEVELOPER_DIR is required for the Apple audio decoder')
        result['ffmpeg'] = str(build_ffmpeg(workspace, sources['ffmpeg'], args.xcode.resolve(), args.jobs))
    if args.command in ('ndk','all'):
        result['ndk']=str(ensure_ndk((args.ndk_cache or workspace/'ndk-cache').resolve(),args.accept_ndk_license))
    (workspace / 'bootstrap.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print('BOOTSTRAP_FAILED:', error, file=sys.stderr)
        raise SystemExit(1)
