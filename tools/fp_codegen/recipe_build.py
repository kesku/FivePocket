#!/usr/bin/env python3
"""recipe_build.py: rebuild every generated PPC chunk of an attempt from a recorded recipe and verify it byte for byte.

  make SPEC.json RECIPE.json --attempt vN [--stitch-cache DIR] [--allow-mismatch]
        Runs the spec's ordered passes over the validated corpus, records per chunk the base layer, the passes that change
        the chunk (with the SHA-256 after each) and the expected output SHA-256 = the attempt's effective chunk, plus the
        SHA-256 of every tool file (pass tool + its sibling imports), parameter file and input. Refuses (exit 1) when a
        chunk does not reproduce, unless --allow-mismatch (the chunk is then recorded with "reproduced": false).
  build RECIPE.json OUT_DIR [--attempt vN] [--chunks N,M,...] [--write none|mismatch|all] [--stitch-cache DIR]
        Rebuilds every chunk (or the listed ones) from the recipe alone: checks every tool, parameter and input hash
        first (refuses on any drift), then per chunk base -> passes, checking each intermediate SHA-256, and compares
        the result with the recipe's expected SHA-256 and with the attempt's effective chunk (default: the recipe's
        target). Writes OUT_DIR/report.json (per chunk: match / differ + the first pass whose output deviates + the
        first differing line) and, per --write (default mismatch), the rebuilt chunks. Exit 1 on any mismatch.
  check RECIPE.json
        Only the tool / parameter / input hash checks.

SPEC (private, under local-analysis/): {"schema": "gta5-recipe-spec/1", "corpus": DIR, "generator": {...}, "inputs":
{name: PATH}, "passes": [{"id", "kind", "tool", "params": {name: PATH}, "args": {...}, "chunks": "all" | [N, ...] |
{"param": NAME, "key": K}, "lineage": [attempts], "note"}]} in application order. Kinds: stitch (corpus level:
stitch_split_functions.py --root R... once, roots from params.roots), fence, fence-dup, anchored-insert, jump-table,
gpr-v243, leaf, direct, gpr-fastpath, vmx, dcbt, ap, rg, case-label-flush (see ADAPTERS). Tool paths are relative to this repository;
parameter / input / corpus / attempt paths relative to the data root (the checkout that holds local-analysis/; --data-root
or RECIPE_DATA_ROOT override). Parameter files hold game-derived data (guest addresses, exclusion lists, inserted
diagnostics, stitch roots) and stay under the ignored local-analysis/, referenced by path + SHA-256; so does every recipe
instance (it lists SHA-256s of generated code). This tool contains no game code or game-derived data.

Generator base (design gap G4, optional): "generator_base": {"dir": DIR, "report": PATH, "note"} names a XenonRecomp
output tree (regen_check.py --keep: the 19-patch generator with the recorded config and switch tables) and "base_passes"
the ordered passes that turn its chunk into the validated corpus chunk (kinds case-blocks, switch-expand, error-sites,
vmx-unpack-call, vmx-helper-includes, eof-blank, recorded-hunks; tools in recipe/). For every chunk in their scope whose
corpus SHA-256 they reproduce, make records the base as {"layer": "generator+passes", path/sha256 of the generator file,
the base passes with the SHA-256 after each, "corpus": path/sha256}; build then starts from the generator file, replays
the base passes, requires the corpus SHA-256 (the stitch stage reads the corpus tree, so this equality is what makes the
stitched copy of such a chunk follow from the generator) and continues with the ordinary passes. A chunk the base
passes do not reproduce keeps the validated-ppc base and is listed as "G4 open". The recipe must be written under
local-analysis/ (make's work directory is created next to it).

Determinism: every pass is a pure text -> text function (in process, or a subprocess with PYTHONHASHSEED=0 and no *_MUTANT
variables); chunks are processed in numeric order; OUT_DIR must be new and under local-analysis/; attempt and corpus files
are only read. The stitch stage (~2 min) can be reused from --stitch-cache DIR when its stamp (stitcher + helpers, image,
corpus and roots SHA-256s) is identical; its outputs are still checked against the recipe's per-chunk hashes.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'own_disc'))
import host_support

TOOLS_ROOT = host_support.package_root(__file__)
SCHEMA = 'gta5-chunk-recipe/1'
SPEC_SCHEMA = 'gta5-recipe-spec/1'
CHUNK = re.compile(r'ppc_recomp\.(\d+)\.cpp')
IMPORT = re.compile(r'^\s*(?:import\s+([\w\s,]+?)|from\s+(\w+)\s+import\b.*)\s*(?:#.*)?$', re.M)
TRIM = 160


# ------------------------------------------------------------------------------------------------ paths and hashes

def find_data_root(arg: str | None) -> Path:
    for cand in (arg, os.environ.get('RECIPE_DATA_ROOT')):
        if cand:
            p = Path(cand).resolve()
            if not p.is_dir():
                raise SystemExit(f'{p}: data root missing; choose the build work directory')
            return p
    if (TOOLS_ROOT / 'local-analysis').is_dir():
        return TOOLS_ROOT
    git = TOOLS_ROOT / '.git'
    if git.is_file():  # a worktree: <main>/.git/worktrees/<name>
        text = git.read_text(encoding='utf-8').strip()
        if text.startswith('gitdir:'):
            p = Path(text.split(':', 1)[1].strip())
            if p.parent.name == 'worktrees' and p.parent.parent.name == '.git':
                main = p.parent.parent.parent
                if (main / 'local-analysis').is_dir():
                    return main
    raise SystemExit('cannot find the data root (the checkout with local-analysis/); pass --data-root')


def sha_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def sha_text(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


def read_chunk(path: Path) -> str:
    data = path.read_bytes()
    if b'\r' in data:
        raise SystemExit(f'{path}: carriage return in a generated chunk')
    return data.decode('utf-8', errors='strict')


def closure(tool: Path) -> list[Path]:
    """the tool file plus every sibling module it imports (recursively): the files whose bytes define the pass"""
    seen, todo = [], [tool]
    while todo:
        p = todo.pop(0)
        if p in seen:
            continue
        seen.append(p)
        for m in IMPORT.finditer(p.read_text(encoding='utf-8')):
            names = [m.group(2)] if m.group(2) else [x.split()[0] for x in m.group(1).split(',') if x.strip()]
            for name in names:
                q = p.parent / f'{name}.py'
                if name == 'host_support' and not q.exists():
                    q = TOOLS_ROOT / 'tools/own_disc/host_support.py'
                if q.exists() and q not in seen:
                    todo.append(q)
    return seen


def tool_name(path, root) -> str:
    """A tool file's name in a recipe: its path below the tools root, with forward slashes on every host (a recipe
    written on one host is checked on another)."""
    return path.relative_to(root).as_posix()


def tool_record(rel: str) -> dict:
    path = TOOLS_ROOT / rel
    files = closure(path)
    return {'path': rel, 'sha256': sha_file(path),
            'helpers': {tool_name(p, TOOLS_ROOT): sha_file(p) for p in files[1:]}}


def check_tool(rec: dict, problems: list[str]) -> None:
    path = TOOLS_ROOT / rec['path']
    if not path.exists():
        problems.append(f'tool missing: {rec["path"]}')
        return
    if sha_file(path) != rec['sha256']:
        problems.append(f'tool changed: {rec["path"]}')
    now = {tool_name(p, TOOLS_ROOT): sha_file(p) for p in closure(path)[1:]}
    if now != rec['helpers']:
        problems.append(f'tool helpers changed: {rec["path"]}: {sorted(set(now.items()) ^ set(rec["helpers"].items()))}')


# ------------------------------------------------------------------------------------------------ attempts

def effective(root: Path, attempt: str) -> dict[int, Path]:
    """iterate.py's rule: an attempt.json chunk entry with "source" is pinned to that file, else attempts/<v>/src/<name>;
    chunks the attempt does not list come from the validated corpus"""
    a = root / 'local-analysis/attempts' / attempt
    spec = json.loads((a / 'attempt.json').read_text(encoding='utf-8'))
    corpus = root / 'local-analysis/android-private-trace-window/api26/validated-ppc'
    out: dict[int, Path] = {}
    for f in spec['files']:
        if 'ppc_chunk' in f:
            out[int(f['ppc_chunk'])] = (root / f['source']) if f.get('source') else (a / 'src' / f['name'])
    for p in corpus.glob('ppc_recomp.*.cpp'):
        m = CHUNK.fullmatch(p.name)
        if m and int(m.group(1)) not in out:
            out[int(m.group(1))] = p
    return out


def corpus_chunks(corpus: Path) -> dict[int, Path]:
    out = {}
    for p in corpus.glob('ppc_recomp.*.cpp'):
        m = CHUNK.fullmatch(p.name)
        if m:  # skip the corpus's ppc_recomp.N.pre-*-before.cpp repair backups
            out[int(m.group(1))] = p
    return dict(sorted(out.items()))


# ------------------------------------------------------------------------------------------------ pass adapters

class Pass:
    def __init__(self, d: dict, root: Path, tmp: Path):
        self.id, self.kind, self.d, self.root, self.tmp = d['id'], d['kind'], d, root, tmp
        self.tool = TOOLS_ROOT / d['tool']['path']
        self.param_paths = {k: root / v['path'] for k, v in d.get('params', {}).items()}
        self.params = {}
        for k, p in self.param_paths.items():
            self.params[k] = p.read_text(encoding='utf-8') if p.suffix == '.txt' else json.loads(p.read_text(encoding='utf-8'))
        self.args = d.get('args', {})
        self._mod = None

    def module(self):
        if self._mod is None:
            here = str(self.tool.parent)
            if here not in sys.path:
                sys.path.insert(0, here)
            name = self.tool.stem
            mod = sys.modules.get(name)
            if mod is None:
                spec = importlib.util.spec_from_file_location(name, self.tool)
                mod = importlib.util.module_from_spec(spec)
                sys.modules[name] = mod
                spec.loader.exec_module(mod)
            if Path(mod.__file__).resolve() != self.tool.resolve():
                raise SystemExit(f'module name clash: {name} -> {mod.__file__}')
            for f in closure(self.tool)[1:]:  # sibling imports must resolve to the recorded files
                m = sys.modules.get(f.stem)
                if m is not None and Path(m.__file__).resolve() != f.resolve():
                    raise SystemExit(f'module name clash: {f.stem} -> {m.__file__}')
            self._mod = mod
        return self._mod

    def scope(self, chunks: list[int]) -> set[int]:
        s = self.d.get('chunks', 'all')
        if s == 'all':
            return set(chunks)
        if isinstance(s, list):
            return {int(x) for x in s}
        obj = self.params[s['param']]
        if s.get('key'):
            obj = obj[s['key']]
        return {int(x) for x in obj}

    def run(self, cmd: list[str], text: str) -> str:
        src, dst = self.tmp / f'{self.id}.in.cpp', self.tmp / f'{self.id}.out.cpp'
        for p in (src, dst):
            if p.exists():
                p.unlink()
        src.write_text(text, encoding='utf-8', newline='\n')
        env = {k: v for k, v in os.environ.items() if not k.endswith('_MUTANT')}
        env['PYTHONHASHSEED'] = '0'
        env['PYTHONDONTWRITEBYTECODE'] = '1'
        argv = [x.replace('{in}', str(src)).replace('{out}', str(dst)) for x in cmd]
        command, options = host_support.low_priority(host_support.python_command(self.tool, *argv))
        r = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', env=env, **options)
        if r.returncode != 0 or not dst.exists():
            raise PassError(f'{self.id}: rc {r.returncode}: {(r.stderr or r.stdout)[-400:]}')
        out = read_chunk(dst)
        src.unlink(); dst.unlink()
        return out

    def apply(self, n: int, text: str) -> str:
        return ADAPTERS[self.kind](self, n, str(n), text)


class PassError(RuntimeError):
    pass


def _fence(p, n, key, text):
    lines, _ = p.module().rewrite(text.splitlines(keepends=True))
    return ''.join(lines)


def _fence_dup(p, n, key, text):
    return p.module().apply(text, p.params['records']['chunks'][key])


def _anchored(p, n, key, text):
    return p.module().apply(text, p.params['records']['chunks'][key]['inserts'])


def _jump_table(p, n, key, text):
    return p.module().apply(text, p.params['records']['chunks'][key])[0]


def _gpr_v243(p, n, key, text):
    return p.module().apply(text)[0]


def _leaf(p, n, key, text):
    return p.run(['{in}', '{out}', p.params['targets']['chunks'][key]], text)


def _direct(p, n, key, text):
    return p.module().rewrite(text, set(p.params['hooked'].split()))[0]


def _gpr_fastpath(p, n, key, text):
    return p.run(['helpers', '{in}', '{out}'] + (['--twin', p.args['twin']] if p.args.get('twin') else []), text)


def _vmx(p, n, key, text):
    mod = p.module()
    man = p.params['exclusions']
    c_text, _ = mod.apply_c(text, set(man['exclude_c']))
    b_text, _, _ = mod.apply_b(c_text, set(man['exclude_b']))
    return b_text


def _dcbt(p, n, key, text):
    src, dst = p.tmp / 'dcbt.in.cpp', p.tmp / 'dcbt.out.cpp'
    for q in (src, dst):
        if q.exists():
            q.unlink()
    src.write_text(text, encoding='utf-8', newline='\n')
    p.module().rewrite(src, dst, p.args.get('variant', 'switch'), set(p.params['exclusions'][key]['excluded_functions']))
    out = read_chunk(dst)
    src.unlink(); dst.unlink()
    return out


def _sites(p, n, key, text):
    return p.run(['{in}', '{out}', '--sites-file', str(p.param_paths['sites'])], text)


def _records(p, n, key, text):  # G4 corpus-repair passes with per-chunk records
    return p.module().apply(text, p.params['records']['chunks'][key])[0]


def _plain(p, n, key, text):  # G4 corpus-repair passes without parameters
    return p.module().apply(text)[0]


ADAPTERS = {'fence': _fence, 'fence-dup': _fence_dup, 'anchored-insert': _anchored, 'jump-table': _jump_table,
            'gpr-v243': _gpr_v243, 'leaf': _leaf, 'direct': _direct, 'gpr-fastpath': _gpr_fastpath, 'vmx': _vmx,
            'dcbt': _dcbt, 'ap': _sites, 'rg': _sites,
            'case-blocks': _records, 'switch-expand': _records, 'error-sites': _records, 'recorded-hunks': _records,
            'vmx-unpack-call': _plain, 'vmx-helper-includes': _plain, 'eof-blank': _plain,
            'case-label-flush': _records}   # ftz-fix: the generator's CSR reset at text-added labels


# ------------------------------------------------------------------------------------------------ stitch stage

def stitch_stage(p: dict, root: Path, corpus: Path, inputs: dict, work: Path, cache: Path | None,
                 chunk_paths: dict[int, Path]) -> tuple[Path, str]:
    """run stitch_split_functions.py once with the recorded roots; returns (directory of stitched chunk copies, how)"""
    roots_path = root / p['params']['roots']['path']
    roots = json.loads(roots_path.read_text(encoding='utf-8'))['roots']
    mapping = corpus / 'ppc_func_mapping.cpp'
    corpus_digest = hashlib.sha256(''.join(f'{n}:{sha_file(q)}\n' for n, q in sorted(chunk_paths.items())).encode()
                                   + sha_file(mapping).encode()).hexdigest()
    stamp = {'tool': tool_record(p['tool']['path']), 'image': inputs['image']['sha256'], 'corpus': corpus_digest,
             'roots': sha_file(roots_path)}
    if cache is not None and (cache / 'stamp.json').exists():
        if json.loads((cache / 'stamp.json').read_text(encoding='utf-8')) == stamp:
            return cache, 'reused (identical stamp)'
    dst = cache if cache is not None and not cache.exists() else work / 'stitch'
    if dst.exists():
        raise SystemExit(f'{dst} exists (stale stitch cache? use a new directory)')
    cmd = host_support.python_command(TOOLS_ROOT / p['tool']['path'], '--image', root / inputs['image']['path'],
           '--sources', corpus, '--out', dst, '--report', work / 'stitch-report.json')
    for r in roots:
        cmd += ['--root', r]
    env = dict(os.environ, PYTHONHASHSEED='0', PYTHONDONTWRITEBYTECODE='1')
    t0 = time.monotonic()
    cmd, options = host_support.low_priority(cmd)
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', env=env, **options)
    if r.returncode != 0:
        raise SystemExit(f'stitch: rc {r.returncode}: {r.stderr[-800:]}')
    rep = json.loads((work / 'stitch-report.json').read_text(encoding='utf-8'))['summary']
    if rep['rejected'] or rep['rebuilt'] != len(roots):
        raise SystemExit(f'stitch: {rep["rebuilt"]} of {len(roots)} roots rebuilt, {rep["rejected"]} rejected')
    (dst / 'stamp.json').write_text(json.dumps(stamp, indent=1) + '\n', encoding='utf-8', newline='\n')
    return dst, f'ran ({time.monotonic() - t0:.0f} s)'


# ------------------------------------------------------------------------------------------------ make

def make(args) -> int:
    root = find_data_root(args.data_root)
    spec = json.loads(Path(args.spec).read_text(encoding='utf-8'))
    if spec.get('schema') != SPEC_SCHEMA:
        raise SystemExit('not a recipe spec')
    out = Path(args.recipe)
    if out.exists():
        raise SystemExit(f'{out} exists')
    corpus = root / spec['corpus']
    chunk_paths = corpus_chunks(corpus)
    target = effective(root, args.attempt)
    if set(target) != set(chunk_paths):
        raise SystemExit('attempt chunk set differs from the corpus chunk set')
    inputs = {k: {'path': v, 'sha256': sha_file(root / v)} for k, v in spec.get('inputs', {}).items()}
    def records(ds):
        out_ = []
        for d in ds:
            rec = dict(d)
            rec['tool'] = tool_record(d['tool'])
            rec['params'] = {k: {'path': v, 'sha256': sha_file(root / v)} for k, v in d.get('params', {}).items()}
            out_.append(rec)
        return out_
    passes = records(spec['passes'])
    base_passes = records(spec.get('base_passes', []))  # G4: generator output -> validated corpus chunk
    if any(p['kind'] == 'stitch' for p in base_passes):
        raise SystemExit('the stitch stage cannot be a base pass')
    gbase = spec.get('generator_base')
    if base_passes and not gbase:
        raise SystemExit('base_passes need a generator_base directory')
    work = out.absolute().parent / '.make-work'
    if not work.is_relative_to(root / 'local-analysis'):
        raise SystemExit('the recipe must be written under local-analysis/ (it lists hashes of generated code)')
    if work.exists():
        raise SystemExit(f'{work} exists (an interrupted make? remove it)')
    work.mkdir(parents=True)
    tmp = work / 'tmp'
    tmp.mkdir()
    gen = spec.get('generator', {})
    raw = {}
    if gen.get('output'):
        for n in chunk_paths:
            q = root / gen['output'] / f'ppc_recomp.{n}.cpp'
            if q.exists():
                raw[n] = sha_file(q)
    r19 = json.loads((root / gen['regen19_report']).read_text(encoding='utf-8'))['digests'] if gen.get('regen19_report') else {}
    try:
        stitched_dir, how = (None, '')
        st = [p for p in passes if p['kind'] == 'stitch']
        if st:
            stitched_dir, how = stitch_stage(st[0], root, corpus, inputs, work, Path(args.stitch_cache) if args.stitch_cache else None,
                                             chunk_paths)
        objs = [Pass(p, root, tmp) for p in passes if p['kind'] != 'stitch']
        scopes = {o.id: o.scope(list(chunk_paths)) for o in objs}
        base_objs = [Pass(p, root, tmp) for p in base_passes]
        base_scopes = {o.id: o.scope(list(chunk_paths)) for o in base_objs}
        base_scope = set().union(*base_scopes.values()) if base_objs else set()
        chunks, bad, g4_open = {}, [], {}
        for n, path in chunk_paths.items():
            base_sha = sha_file(path)
            base = {'layer': 'validated-ppc', 'path': str(path.relative_to(root)), 'sha256': base_sha}
            if raw.get(n) == base_sha:
                base['generator'] = 'equals the 14-patch generator output (regen/flags-off, full SHA-256)'
            elif r19.get(path.name) and base_sha.startswith(r19[path.name]):
                base['generator'] = 'equals the 19-patch generator output (12-digit digest prefix, regen19-check)'
            else:
                base['generator'] = 'post-generation repair (not reproduced by the generator; design gap G4)'
            if n in base_scope:
                gpath = root / gbase['dir'] / path.name
                gtext, bsteps = read_chunk(gpath), []
                try:
                    for o in base_objs:
                        if n in base_scopes[o.id]:
                            new = o.apply(n, gtext)
                            if new != gtext:
                                gtext = new
                                bsteps.append({'pass': o.id, 'sha256': sha_text(gtext)})
                    why = None if sha_text(gtext) == base_sha else 'output differs from the corpus chunk'
                except (PassError, SystemExit) as e:
                    why = f'{e}'[:200]
                if why is None:  # the corpus chunk = generator output + the G4 passes: that becomes the base
                    base = {'layer': 'generator+passes', 'path': str(gpath.relative_to(root)), 'sha256': sha_file(gpath),
                            'passes': bsteps, 'corpus': {'path': str(path.relative_to(root)), 'sha256': base_sha},
                            'generator': 'post-generation repair reproduced from the 19-patch generator output by the '
                                         'G4 base passes'}
                else:
                    g4_open[n] = why
                    base['g4'] = f'G4 base passes do not reproduce it: {why}'
            text = read_chunk(path)
            steps = []
            if stitched_dir is not None and (stitched_dir / path.name).exists():
                text = read_chunk(stitched_dir / path.name)
                steps.append({'pass': st[0]['id'], 'sha256': sha_text(text)})
            for o in objs:
                if n not in scopes[o.id]:
                    continue
                new = o.apply(n, text)
                if new != text:
                    text = new
                    steps.append({'pass': o.id, 'sha256': sha_text(text)})
            want = sha_file(target[n])
            ok = sha_text(text) == want
            if not ok:
                bad.append(n)
            chunks[str(n)] = {'base': base, 'passes': steps, 'expect': {'sha256': want, 'file': str(target[n].relative_to(root))},
                              'reproduced': ok}
    except BaseException:
        shutil.rmtree(work, ignore_errors=True)
        raise
    if bad and not args.allow_mismatch:
        shutil.rmtree(work, ignore_errors=True)
        print(f'not reproduced: {bad}')
        return 1
    recipe = {'schema': SCHEMA, 'target': args.attempt, 'made': time.strftime('%Y-%m-%d %H:%M:%S'),
              'note': 'private: per-chunk SHA-256s of generated game code and paths of game-derived parameter files',
              'spec': {'path': str(Path(args.spec).resolve()), 'sha256': sha_file(Path(args.spec))},
              'corpus': spec['corpus'], 'generator': {k: {'path': v, 'sha256': sha_file(root / v)} if k in ('manifest', 'regen19_report') else v
                                                      for k, v in gen.items()},
              'inputs': inputs, 'passes': passes, 'chunks': chunks}
    if base_passes:
        recipe['generator_base'] = {k: ({'path': v, 'sha256': sha_file(root / v)} if k in ('report', 'manifest') else v)
                                    for k, v in gbase.items()}
        recipe['base_passes'] = base_passes
    out.write_text(json.dumps(recipe, indent=1) + '\n', encoding='utf-8', newline='\n')
    shutil.rmtree(work, ignore_errors=True)
    used = {}
    for c in chunks.values():
        for s in c['base'].get('passes', []) + c['passes']:
            used[s['pass']] = used.get(s['pass'], 0) + 1
    layers = {}
    for c in chunks.values():
        k = c['base']['layer'] + (' (G4 open)' if 'g4' in c['base'] else '')
        layers[k] = layers.get(k, 0) + 1
    print(f'{args.attempt}: {len(chunks) - len(bad)} of {len(chunks)} chunks reproduced; not: {bad}; stitch {how}; '
          f'base layers {layers}; G4 open {sorted(g4_open)}; passes {used}')
    return 0


# ------------------------------------------------------------------------------------------------ build / check

def check_hashes(recipe: dict, root: Path) -> list[str]:
    problems = []
    for name, rec in recipe['inputs'].items():
        p = root / rec['path']
        if not p.exists() or sha_file(p) != rec['sha256']:
            problems.append(f'input {name} missing or changed: {rec["path"]}')
    for p in recipe['passes'] + recipe.get('base_passes', []):
        check_tool(p['tool'], problems)
        if p['kind'] not in ADAPTERS and p['kind'] != 'stitch':
            problems.append(f'unknown pass kind {p["kind"]}')
        for k, rec in p.get('params', {}).items():
            q = root / rec['path']
            if not q.exists() or sha_file(q) != rec['sha256']:
                problems.append(f'parameter file {p["id"]}.{k} missing or changed: {rec["path"]}')
    for k, rec in recipe.get('generator_base', {}).items():
        if isinstance(rec, dict) and 'sha256' in rec:
            q = root / rec['path']
            if not q.exists() or sha_file(q) != rec['sha256']:
                problems.append(f'generator base {k} missing or changed: {rec["path"]}')
    return problems


def first_difference(got: str, want: str) -> dict:
    a, b = got.splitlines(keepends=True), want.splitlines(keepends=True)
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            return {'line': i + 1, 'built': a[i][:TRIM], 'expected': b[i][:TRIM]}
    return {'line': min(len(a), len(b)) + 1, 'built': (a[len(b)] if len(a) > len(b) else '<end>')[:TRIM],
            'expected': (b[len(a)] if len(b) > len(a) else '<end>')[:TRIM]}


def build(args) -> int:
    root = find_data_root(args.data_root)
    recipe = json.loads(Path(args.recipe).read_text(encoding='utf-8'))
    if recipe.get('schema') != SCHEMA:
        raise SystemExit('not a chunk recipe')
    out = Path(args.out).absolute()
    if out.exists() or out.is_relative_to(root / 'local-analysis/attempts'):
        raise SystemExit('OUT_DIR must be new and outside attempts/')
    host_support.private_output(out)
    if any(k.endswith('_MUTANT') for k in os.environ):
        raise SystemExit('a *_MUTANT variable is set: pass tools would run their mutants')
    problems = check_hashes(recipe, root)
    if problems:
        for p in problems:
            print('REFUSED:', p)
        return 1
    if args.cmd == 'check':
        print('all tool, parameter and input hashes match')
        return 0
    attempt = args.attempt or recipe['target']
    # --expect-only: each chunk is checked against the hash the recipe records for it and nothing else. The attempt's
    # own chunk files (game code of the earlier build) are then never opened: another machine has none, and the
    # builder's run must not read one.
    target = None if getattr(args, 'expect_only', False) else effective(root, attempt)
    want_chunks = sorted(int(x) for x in recipe['chunks'])
    if args.chunks:
        want_chunks = [int(x) for x in args.chunks.split(',')]
    out.mkdir(parents=True)
    tmp = out / 'tmp'
    tmp.mkdir()
    corpus = root / recipe['corpus']
    chunk_paths = corpus_chunks(corpus)
    t0 = time.monotonic()
    by_id = {p['id']: p for p in recipe['passes']}
    objs = {p['id']: Pass(p, root, tmp) for p in recipe['passes'] if p['kind'] != 'stitch'}
    base_objs = {p['id']: Pass(p, root, tmp) for p in recipe.get('base_passes', [])}
    stitched_dir, how = None, 'not needed'
    needs_stitch = [p for p in recipe['passes'] if p['kind'] == 'stitch' and any(
        s['pass'] == p['id'] for n in want_chunks for s in recipe['chunks'][str(n)]['passes'])]
    if needs_stitch:
        stitched_dir, how = stitch_stage(needs_stitch[0], root, corpus, recipe['inputs'], out,
                                         Path(args.stitch_cache) if args.stitch_cache else None, chunk_paths)
    t_stitch = time.monotonic() - t0
    rows, counts = {}, {'match': 0, 'differ': 0, 'error': 0}
    for n in want_chunks:
        c = recipe['chunks'][str(n)]
        row = {'passes': [s['pass'] for s in c['passes']]}
        try:
            base_path = root / c['base']['path']
            text = read_chunk(base_path)
            if sha_text(text) != c['base']['sha256']:
                raise PassError('base layer changed')
            deviated = None
            if c['base']['layer'] == 'generator+passes':  # G4: generator output + base passes = the corpus chunk
                row['base'] = [s['pass'] for s in c['base']['passes']]
                for s in c['base']['passes']:
                    text = base_objs[s['pass']].apply(n, text)
                    if deviated is None and sha_text(text) != s['sha256']:
                        deviated = s['pass']
                row['base_reproduces_corpus'] = sha_text(text) == c['base']['corpus']['sha256']
                if not row['base_reproduces_corpus']:
                    deviated = deviated or 'base'
            for s in c['passes']:
                p = by_id[s['pass']]
                if p['kind'] == 'stitch':  # no stitched copy = the stitcher left this chunk unchanged
                    q = stitched_dir / base_path.name
                    text = read_chunk(q) if q.exists() else text
                else:
                    text = objs[p['id']].apply(n, text)
                if deviated is None and sha_text(text) != s['sha256']:
                    deviated = s['pass']
            got = sha_text(text)
            actual = sha_file(target[n]) if target is not None else None
            # a G4 chunk the stitcher replaces must still have reproduced the stitcher's corpus input
            ok = got == c['expect']['sha256'] and (target is None or got == actual) and row.get('base_reproduces_corpus', True)
            row.update(status='match' if ok else 'differ', sha256=got[:16])
            if not ok:
                row['first_deviating_pass'] = deviated
                if target is not None:
                    row['recipe_expect_equals_attempt'] = c['expect']['sha256'] == actual
                    row['first_difference'] = first_difference(text, read_chunk(target[n]))
            if args.write == 'all' or (args.write == 'mismatch' and not ok):
                (out / f'ppc_recomp.{n}.cpp').write_text(text, encoding='utf-8', newline='\n')
        except (PassError, SystemExit, KeyError, OSError) as e:
            row.update(status='error', error=str(e)[:400])
        counts[row['status']] += 1
        rows[str(n)] = row
    shutil.rmtree(tmp, ignore_errors=True)
    if stitched_dir is not None and stitched_dir == out / 'stitch' and args.write != 'all':
        shutil.rmtree(stitched_dir)  # intermediate copies of generated code: keep only what --write asks for
    report = {'recipe': str(Path(args.recipe).resolve()), 'recipe_sha256': sha_file(Path(args.recipe)), 'attempt': attempt,
              'compared_with': 'the recipe\'s recorded hashes only' if target is None else 'the recipe\'s recorded hashes and the attempt\'s chunk files',
              'runner_sha256': sha_file(Path(__file__)),
              'python': sys.version.split()[0], 'stitch': how, 'seconds': {'stitch': round(t_stitch, 1),
                                                                         'total': round(time.monotonic() - t0, 1)},
              'counts': counts, 'not_matching': [n for n, r in rows.items() if r['status'] != 'match'], 'chunks': rows}
    (out / 'report.json').write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(f'{attempt}: {counts["match"]} match, {counts["differ"]} differ, {counts["error"]} error of {len(rows)} chunks '
          f'(stitch {how}; {report["seconds"]["total"]} s); not matching: {report["not_matching"]}')
    for n in report['not_matching'][:10]:
        r = rows[n]
        print(f'  chunk {n}: {r["status"]} {r.get("error", "")} first deviating pass {r.get("first_deviating_pass")} '
              f'first differing line {r.get("first_difference", {}).get("line")}')
    return 0 if counts['match'] == len(rows) else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('make')
    m.add_argument('spec'); m.add_argument('recipe'); m.add_argument('--attempt', required=True)
    m.add_argument('--stitch-cache'); m.add_argument('--allow-mismatch', action='store_true'); m.add_argument('--data-root')
    b = sub.add_parser('build')
    b.add_argument('recipe'); b.add_argument('out'); b.add_argument('--attempt'); b.add_argument('--chunks')
    b.add_argument('--write', choices=['none', 'mismatch', 'all'], default='mismatch'); b.add_argument('--stitch-cache')
    b.add_argument('--data-root')
    b.add_argument('--expect-only', action='store_true',
                   help="check each chunk against the recipe's recorded hash only; the attempt's own chunk files are not read")
    c = sub.add_parser('check')
    c.add_argument('recipe'); c.add_argument('--data-root')
    a = ap.parse_args()
    if a.cmd == 'make':
        return make(a)
    if a.cmd == 'check':
        a.out, a.attempt, a.chunks, a.write, a.stitch_cache = None, None, None, 'none', None
        root = find_data_root(a.data_root)
        recipe = json.loads(Path(a.recipe).read_text(encoding='utf-8'))
        problems = check_hashes(recipe, root)
        for p in problems:
            print('REFUSED:', p)
        print('all tool, parameter and input hashes match' if not problems else f'{len(problems)} problems')
        return 1 if problems else 0
    return build(a)


if __name__ == '__main__':
    raise SystemExit(main())
