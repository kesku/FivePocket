#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Code-generation and recipe adapters shared by the Duo build stages."""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

GENERATOR = 'local-analysis/xenonrecomp-build/build/XenonRecomp/XenonRecomp'
GENERATOR_CONTEXT = 'local-analysis/xenonrecomp-build/src/XenonUtils/ppc_context.h'
RECIPE = 'local-analysis/recipes/recipe-v455.json'

def sha(p):
    h = hashlib.sha256()
    with Path(p).open('rb') as f:
        for block in iter(lambda: f.read(1048576), b''):
            h.update(block)
    return h.hexdigest()


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    obj = importlib.util.module_from_spec(spec)
    sys.modules[name] = obj
    spec.loader.exec_module(obj)
    return obj


def write_json(p, data):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8', newline='\n')


def clone(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if sys.platform == 'darwin':
        subprocess.run(['/bin/cp', '-c', str(src), str(dst)], check=True)     # an APFS clone: no bytes are copied
    else:
        shutil.copyfile(src, dst)                                               # a host without clones: the same bytes, copied


def generator_config(text, original_dir, xex):
    """Change the actual producer input, preserving the recorded switch tables."""
    out = []
    seen = set()
    for line in text.splitlines():
        m = re.fullmatch(r'(file_path|switch_table_file_path) = "(.*)"', line)
        if m:
            name, old = m.groups()
            seen.add(name)
            target = xex if name == 'file_path' else (original_dir / old).resolve()
            line = f'{name} = {json.dumps(str(target))}'
        out.append(line)
    if seen != {'file_path', 'switch_table_file_path'}:
        raise ValueError('configuration lacks exact input path fields')
    return '\n'.join(out) + '\n'


def connect_recipe(recipe, root, work):
    out = copy.deepcopy(recipe)
    out['target'] = 'v507'
    out['corpus'] = str(work / 'recipe/corpus')
    out['inputs']['image']['path'] = str(work / 'decode/default.image.bin')
    out['generator_base']['dir'] = str(work / 'codegen/ppc')
    for c in out['chunks'].values():
        base = c['base']
        parent = work / ('codegen/ppc' if base['layer'] == 'generator+passes' else 'recipe/corpus')
        base['path'] = str(parent / Path(base['path']).name)
        if 'corpus' in base:
            base['corpus']['path'] = str(work / 'recipe/corpus' / Path(base['corpus']['path']).name)
    return out


def require_hash(path, wanted):
    if sha(path) != wanted:
        raise ValueError('input/output hash differs: ' + Path(path).name)


def run_codegen(root, work, generator=None, context=None):
    """generator, context: another host's build of the generator and the header it ships with (default: this Mac's)."""
    cfg = root / 'local-analysis/regen/flags-off/recomp.toml'
    dst = work / 'codegen-input/recomp.toml'
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(generator_config(cfg.read_text(), cfg.parent, work / 'input/default.xex'), encoding='utf-8', newline='\n')
    subprocess.run([sys.executable, '-B', str(root / 'tools/own_disc/regen_check.py'),
                    '--generator', str(generator or root / GENERATOR),
                    '--context', str(context or root / GENERATOR_CONTEXT),
                    '--config-from', str(dst), '--work', str(work / 'codegen'),
                    '--report', str(work / 'codegen-report.json'), '--keep'], check=True)


def recipe_file(root, named=None):
    """The recipe record: the one named (the same recipe with its tools' hashes recorded anew), else this Mac's."""
    return Path(named) if named else root / RECIPE


def recipe_build_arguments(cfg, work, root):
    """What the recipe tool is asked: all chunks written, each checked against the recipe's recorded hash and nothing
    else. expect_only: the earlier build's own chunk files (attempts/v507/src) are not read. Until 2026-10-08 20:39
    they were, as a second comparison; a run from a root that held only the declared inputs found it."""
    return argparse.Namespace(cmd='build', recipe=str(cfg), out=str(work / 'recipe/chunks'), data_root=str(root),
                              attempt='v507', chunks=None, write='all', stitch_cache=None, expect_only=True)


def run_recipe(root, work, recipe=None):
    rb = module('m09_recipe', root / 'tools/fp_codegen/recipe_build.py')
    recipe = json.loads(recipe_file(root, recipe).read_text(encoding='utf-8'))
    connected = connect_recipe(recipe, root, work)
    require_hash(work / 'decode/default.image.bin', recipe['inputs']['image']['sha256'])
    problems = rb.check_hashes(connected, root)
    if problems:
        raise ValueError('recipe tool/parameter/input check failed: ' + '; '.join(problems))
    corpus = work / 'recipe/corpus'
    corpus.mkdir(parents=True)
    tmp = work / 'recipe/base-tmp'
    tmp.mkdir()
    passes = {p['id']: rb.Pass(p, root, tmp) for p in connected['base_passes']}
    checked = []
    for n, c in sorted(connected['chunks'].items(), key=lambda v: int(v[0])):
        base = c['base']
        source = work / 'codegen/ppc' / Path(base['path']).name
        require_hash(source, base['sha256'])
        text = rb.read_chunk(source)
        for step in base.get('passes', []):
            text = passes[step['pass']].apply(int(n), text)
            if rb.sha_text(text) != step['sha256']:
                raise ValueError('base pass hash differs for chunk ' + n)
        expected = base.get('corpus', base)['sha256']
        if rb.sha_text(text) != expected:
            raise ValueError('reconstructed corpus hash differs for chunk ' + n)
        (corpus / source.name).write_text(text, encoding='utf-8', newline='\n')
        checked.append(int(n))
    # The stitcher consumes only mapping and chunks; do not substitute runtime headers.
    clone(work / 'codegen/ppc/ppc_func_mapping.cpp', corpus / 'ppc_func_mapping.cpp')
    cfg = work / 'recipe/connected-recipe.json'
    write_json(cfg, connected)
    write_json(work / 'recipe/base-check.json', {'chunks_checked': checked, 'source': 'this-run-codegen'})
    code = rb.build(recipe_build_arguments(cfg, work, root))
    if code:
        raise SystemExit(code)


def check_work(root, work):
    """Everything a stage writes is game-derived: it stays under the ignored local-analysis/builder."""
    # (2026-10-24) resolved on both sides: local-analysis/builder may itself be a link (.builder since 2026-10-10)
    if not Path(work).resolve().is_relative_to((Path(root).resolve() / 'local-analysis/builder').resolve()):
        raise SystemExit('the work folder must lie in local-analysis/builder of the repository')


def main():
    ap = argparse.ArgumentParser(description="Run the Duo code-generation or recipe stage.")
    ap.add_argument('stage', choices=['codegen', 'recipe'])
    ap.add_argument('--repo', type=Path, required=True)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--recipe', type=Path)
    ap.add_argument('--generator', type=Path)
    ap.add_argument('--context', type=Path)
    args = ap.parse_args()
    root, work = args.repo.resolve(), args.work.resolve()
    check_work(root, work)
    if args.stage == 'codegen':
        run_codegen(root, work, args.generator.resolve() if args.generator else None,
                    args.context.resolve() if args.context else None)
    else:
        run_recipe(root, work, args.recipe.resolve() if args.recipe else None)


if __name__ == "__main__":
    main()
