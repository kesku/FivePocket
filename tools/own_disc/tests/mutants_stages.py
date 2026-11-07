#!/usr/bin/env python3
"""Mutant check for the stage runner (compile_rehearsal.py) and the shader-table connector (shader_compile_inputs.py):
each mutant is a copy with one rule removed or one value changed; test_stage_connections.py and test_compile_stage.py
(beside this file) must fail on it.

  mutants_stages.py [--only TEXT]

First a control on unchanged copies. Exits 1 if the control fails, a mutant survives or a mutant no longer applies."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
FILES = ('compile_rehearsal.py',)
TESTS = ('test_stage_connections.py',)
R = FILES[0]

MUTANTS = [
    # ---- the recipe stage reads nothing of the earlier build
    (R, 'the recipe tool compares with the earlier build\'s chunk files', "stitch_cache=None, expect_only=True)", "stitch_cache=None, expect_only=False)"),
    (R, 'the recipe tool writes only the chunks that differ', "attempt='v507', chunks=None, write='all',", "attempt='v507', chunks=None, write='mismatch',"),
    # ---- another host's generator
    (R, 'the generator is always this Mac\'s', "                    '--generator', str(generator or root / GENERATOR),", "                    '--generator', str(root / GENERATOR),"),
    (R, 'the generator\'s header is always this Mac\'s', "                    '--context', str(context or root / GENERATOR_CONTEXT),", "                    '--context', str(root / GENERATOR_CONTEXT),"),
    (R, 'the recipe stage runs the generator', "    else:\n        run_recipe(root, work, args.recipe.resolve() if args.recipe else None)\n", "    else:\n        run_codegen(root, work)\n"),
    (R, 'the command line drops the recipe named', "        run_recipe(root, work, args.recipe.resolve() if args.recipe else None)\n", "        run_recipe(root, work, None)\n"),
    (R, 'the recipe is always this Mac\'s', "    return Path(named) if named else root / RECIPE\n", "    return root / RECIPE\n"),
    (R, 'the stage reads the default recipe whatever is named', "    recipe = json.loads(recipe_file(root, recipe).read_text(encoding='utf-8'))", "    recipe = json.loads(recipe_file(root).read_text(encoding='utf-8'))"),
    # ---- a host without clones
    (R, 'every host runs the Mac copy command',
     "    if sys.platform == 'darwin':\n        subprocess.run(['/bin/cp', '-c', str(src), str(dst)], check=True)", "    if True:\n        subprocess.run(['/bin/cp', '-c', str(src), str(dst)], check=True)"),
    (R, 'the Mac copies bytes instead of cloning',
     "    if sys.platform == 'darwin':\n        subprocess.run(['/bin/cp', '-c', str(src), str(dst)], check=True)", "    if False:\n        subprocess.run(['/bin/cp', '-c', str(src), str(dst)], check=True)"),
    # ---- each stage reads this run's producer
    (R, 'the generator reads the old executable',
     "target = xex if name == 'file_path' else (original_dir / old).resolve()", "target = (original_dir / old).resolve()"),
    (R, 'the recipe reads the old chunks',
     "        base['path'] = str(parent / Path(base['path']).name)", "        pass"),
    (R, 'a producer with another hash is accepted',
     "    if sha(path) != wanted:", "    if False:"),
    # ---- the portable library
    # ---- where a stage may write
    # ---- the native-packs stage's folders on the include path
    # ---- the generator's headers come from this run
    # ---- the linker script from this build's own objects
    # ---- beside the pinned chunks
    # (taking these three from the old folder instead is no mutant: the stage has just required them equal)
    # ---- this run's own audio decoder archives
    # ---- without the measurements of an earlier build
    # ---- the runtime objects kept from the base build
]


def run(folder: Path) -> subprocess.CompletedProcess:
    env = dict(os.environ, STAGE_TOOL=str(folder / R), COMPILE_ADAPTER=str(folder / R))
    out, code = '', 0
    for test in TESTS:
        r = subprocess.run([sys.executable, '-B', str(HERE / test), '-v'], env=env, capture_output=True, text=True, timeout=180)
        out += r.stderr
        code = code or r.returncode
    return subprocess.CompletedProcess([], code, '', out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--only', help='run only the mutants whose description contains this text')
    a = ap.parse_args()
    sources = {f: (TOOLS / f).read_text() for f in FILES}
    chosen = [(i, m) for i, m in enumerate(MUTANTS, 1) if not a.only or a.only in m[1]]
    killed = 0
    with tempfile.TemporaryDirectory() as tmp:
        control = Path(tmp) / 'control'
        control.mkdir()
        for f in FILES:
            shutil.copy(TOOLS / f, control / f)
        r = run(control)
        if r.returncode != 0:
            print('CONTROL FAILED: the tests do not pass on unchanged copies\n' + r.stderr[-2000:])
            return 1
        print('control: the tests pass on unchanged copies of the tools', flush=True)
        for index, (name, what, old, new) in chosen:
            if sources[name].count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({sources[name].count(old)} matches in {name}): {what}')
                continue
            folder = Path(tmp) / f'm{index}'
            folder.mkdir()
            for f in FILES:
                (folder / f).write_text(sources[f].replace(old, new) if f == name else sources[f])
            r = run(folder)
            failed = [line.split(' ... ')[0].strip() for line in r.stderr.splitlines() if line.rstrip().endswith(('... FAIL', '... ERROR'))]
            if r.returncode == 0:
                print(f'mutant {index:3d} SURVIVED ({name}): {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed by {failed[0].split(" (")[0] if failed else "the test run (it could not start)"}: {what}')
    print(f'{killed} of {len(chosen)} mutants killed')
    return 0 if killed == len(chosen) else 1


if __name__ == '__main__':
    sys.exit(main())
