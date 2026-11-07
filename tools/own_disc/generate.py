#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the private Duo game source and resources from the owner's two discs."""
from __future__ import annotations

import argparse
import contextlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import bootstrap
import disc_builder
import generate_bridge
from host_support import file_sha256
from source_statements import resolve_records

ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent
CONFIG=HERE/'config'


@contextlib.contextmanager
def environment(values):
    before={key:os.environ.get(key) for key in values}
    os.environ.update(values)
    try:
        yield
    finally:
        for key,value in before.items():
            if value is None:os.environ.pop(key,None)
            else:os.environ[key]=value


def write_json(path,value):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(value,indent=1)+'\n',encoding='utf-8',newline='\n')


def write_if_changed(path,text):
    if not path.is_file() or path.read_text(encoding='utf-8')!=text:
        path.write_text(text,encoding='utf-8',newline='\n')


def prepared_recipe(state, statements=None):
    sys.path.insert(0,str(ROOT/'tools/fp_codegen'))
    import recipe_build
    record=json.loads((CONFIG/'recipe-v455-portable-3.json').read_text(encoding='utf-8'))
    for step in record['passes']+record['base_passes']:
        step['tool']=recipe_build.tool_record(step['tool']['path'])
        for parameter in step.get('params',{}).values():
            path=ROOT/parameter['path']
            if path.name=='error-sites-published-1.json' and statements is not None:
                path=statements
                parameter['path']=str(path)
            parameter['sha256']=file_sha256(path)
    path=state/'prepared-recipe.json'
    write_json(path,record)
    return path


def generated_statements(stage,state,host_tools):
    records=json.loads((CONFIG/'error-sites-published-1.json').read_text(encoding='utf-8'))
    addresses=sorted({address for chunk in records['chunks'].values() for address,body in chunk['bodies'].items()
                      if any(isinstance(item,dict) and 'generated_line' in item for item in body.get('lines',[]))})
    fragments=state/'tail-fragments.cpp'
    config=[]
    for line in (CONFIG/'recomp.toml').read_text(encoding='utf-8').splitlines():
        if line.startswith('file_path = '):
            line='file_path = '+json.dumps(os.path.relpath(stage/'input/default.xex',state))
        elif line.startswith('switch_table_file_path = '):
            line='switch_table_file_path = '+json.dumps(os.path.relpath(CONFIG/'switch_tables.toml',state))
        elif line.startswith('out_directory_path = '):
            line='out_directory_path = "unused"'
        config.append(line)
    fragment_config=state/'fragment.toml'
    fragment_config.write_text('\n'.join(config)+'\n',encoding='utf-8',newline='\n')
    with (state/'tail-fragments.log').open('w',encoding='utf-8',newline='\n') as log:
        subprocess.run([str(host_tools/'xenon-fragment'),str(fragment_config),
                        str(fragments),*addresses],check=True,stdout=log,stderr=subprocess.STDOUT)
    count=resolve_records(records,fragments.read_text(encoding='utf-8'))
    path=state/'error-sites.json'
    write_json(path,records)
    print(f'Regenerated {count} tail-repair statements from the owned executable.',flush=True)
    return path


def cmake_path(path):
    value=str(path).replace('\\','/')
    if '\n' in value or '\r' in value:
        raise ValueError('a CMake path may not contain a newline')
    return '"'+value.replace('$','\\$').replace(';','\\;').replace('"','\\"')+'"'


def emit_manifest(work,ffmpeg):
    stage=work/'stages'
    bridge=work/'bridge'
    declarations=(stage/'codegen/ppc/ppc_recomp_shared.h').read_text(encoding='utf-8')
    abi=json.loads((CONFIG/'import-abi.json').read_text(encoding='utf-8'))['writes_return']
    generate_bridge.generate(declarations,abi,bridge)
    include=work/'include';include.mkdir(exist_ok=True)
    sys.path.insert(0,str(ROOT/'tools/fp_codegen'))
    import gpr_fastpath
    write_if_changed(include/'gpr_helpers_calls.h',gpr_fastpath.header())
    pending=work/'state/vfetch-index.inc'
    subprocess.run([sys.executable,str(ROOT/'tools/vfi_derive.py'),
                    '--inventory',str(stage/'shader-extract'),
                    '--pack',str(stage/'shader-select/generic-shaders.bin'),
                    '--shader-fixes',str(stage/'shader-fixes'),
                    '--out',str(pending),
                    '--report',str(work/'state/vfetch-index.json')],check=True)
    write_if_changed(include/'vfetch_index_rules.inc',pending.read_text(encoding='utf-8'))
    sources=sorted((stage/'recipe/chunks').glob('ppc_recomp.*.cpp'))
    sources+=[stage/'codegen/ppc/ppc_func_mapping.cpp',bridge/'generated_guest_import_bridge.cpp',
              bridge/'remaining_xbox_import_guarded_stops.cpp']
    if not sources or any(not path.is_file() for path in sources):
        raise ValueError('generated source set is incomplete')
    directories={include,stage/'codegen/ppc'}
    for folder in ('native-packs','code-headers','shader-overrides','shader-fixes','nan-overrides'):
        for path in (stage/folder).rglob('*'):
            if path.suffix in ('.h','.hpp','.inc'):
                directories.add(path.parent)
    payload=stage/'ios-payload'
    resource_files=sorted(payload.rglob('*'))
    resource_files=[path for path in resource_files if path.is_file()]
    lines=['# Generated from owned disc inputs. Do not distribute this directory.']
    for name,values in [('FIVEPOCKET_GENERATED_SOURCES',sources),('FIVEPOCKET_GENERATED_INCLUDE_DIRS',sorted(directories)),
                        ('FIVEPOCKET_GENERATED_RESOURCES',resource_files)]:
        lines.append('set('+name)
        lines.extend('  '+cmake_path(path) for path in values)
        lines.append(')')
    lines.append('set(FIVEPOCKET_FFMPEG_ROOT '+cmake_path(ffmpeg)+')')
    lines.append('set(FIVEPOCKET_GENERATED_RESOURCE_ROOT '+cmake_path(payload)+')')
    lines.append('set(FIVEPOCKET_GENERATED_RESOURCE_PREFIX "payload")')
    write_if_changed(work/'build-inputs.cmake','\n'.join(lines)+'\n')
    return {'schema':'duo-generated-inputs/1','sources':len(sources),'include_directories':len(directories),
            'resources':len(resource_files),'source_hashes':{str(p.relative_to(work)):file_sha256(p) for p in sources}}


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--play-disc',type=Path,required=True)
    parser.add_argument('--install-disc',type=Path,required=True)
    parser.add_argument('--tools',type=Path,required=True,help='workspace built by bootstrap.py')
    parser.add_argument('--ndk',type=Path,help='verified Android NDK root; shader tools only')
    parser.add_argument('--accept-ndk-license',action='store_true')
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--stop-after')
    args=parser.parse_args(argv)
    work=args.out.resolve();tools=args.tools.resolve();stage=work/'stages';state=work/'state'
    disc_builder.check_private_path(work,'generated game directory')
    if not work.is_relative_to(ROOT / 'local-analysis' / 'builder'):
        raise ValueError('--out must be under this checkout local-analysis/builder; shader tools require ignored private inputs')
    state.mkdir(parents=True,exist_ok=True)
    if args.ndk:
        ndk=args.ndk.resolve()
    else:
        ndk=bootstrap.ensure_ndk(tools/'ndk-cache',args.accept_ndk_license)
    recipe=prepared_recipe(state)
    profile=json.loads((CONFIG/'pipeline.json').read_text(encoding='utf-8'))
    for item in profile['stages']:
        for field in ('argv','inputs'):
            def remap(value):
                if isinstance(value,str):return value.replace('${work}/prepared-recipe.json',str(recipe))
                if isinstance(value,list):return [remap(x) for x in value]
                if isinstance(value,dict):return {k:remap(v) for k,v in value.items()}
                return value
            if field in item:item[field]=remap(item[field])
    profile_path=state/'pipeline.json';write_json(profile_path,profile)
    command=['build','--profile',str(profile_path),'--play-disc',str(args.play_disc.resolve()),
             '--install-disc',str(args.install_disc.resolve()),'--work',str(stage),
             '--repo',str(ROOT),'--data-root',str(work),'--allow-heavy']
    with environment({'BUILDER_HOST_TOOLS':str(tools/'host-tools'),'BUILDER_NDK_ROOT':str(ndk),
                              'BUILDER_PACKAGE_ROOT':str(ROOT),'BUILDER_PYTHON':sys.executable}):
        result=disc_builder.main([*command,'--stop-after','codegen'])
        if result:return result
        if args.stop_after=='codegen':return 0
        statements=generated_statements(stage,state,tools/'host-tools')
        prepared_recipe(state,statements)
        result=disc_builder.main(command+(['--stop-after',args.stop_after] if args.stop_after else []))
        if result or args.stop_after:return result
    record=emit_manifest(work,tools/'ffmpeg-apple')
    write_json(work/'generation.json',record)
    print(json.dumps({key:value for key,value in record.items() if key!='source_hashes'},indent=2))
    return 0


if __name__=='__main__':
    try:
        raise SystemExit(main())
    except (OSError,ValueError,subprocess.CalledProcessError) as error:
        print('GENERATION_FAILED:',error,file=sys.stderr)
        raise SystemExit(1)
