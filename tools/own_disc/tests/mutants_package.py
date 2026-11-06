#!/usr/bin/env python3
"""Mutant check for the portable builder: for each check of test_package.py (beside this file) one copy of the tools
with one rule changed; the check of that number must fail on it. First a control: all checks pass on an unchanged copy.
Exits 1 if the control fails, a mutant survives, no longer applies or does not run. The tools are not touched."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
COPIED = ('tools/own_disc', 'tools/fp_codegen', 'tools/iter')     # what test_package.py reads under PACKAGE_SRC
H = 'tools/own_disc/host_support.py'
B = 'tools/own_disc/builder.py'
D = 'tools/own_disc/disc_builder.py'
R = 'tools/fp_codegen/recipe_build.py'
MUTANTS = [
 (38, B, "    return root / 'local-analysis' / 'builder' / 'work'", "    return root / 'work'"),
 (38, B, "    c = sub.add_parser('check'); c.add_argument('--work', type=Path)", "    c = sub.add_parser('check'); c.add_argument('--work', type=Path, required=True)"),
 (38, B, "    b.add_argument('--work', type=Path); b.add_argument('--play-disc', type=Path, required=True)", "    b.add_argument('--work', type=Path, required=True); b.add_argument('--play-disc', type=Path, required=True)"),
 (39, B, "'--repo', str(root), '--data-root', str(work), '--allow-heavy']", "'--repo', str(root), '--data-root', str(work)]"),
 (37, B, "            ctx.discs = {role: DiscNamedAtBuild(role) for role in ('play', 'install')}", "            ctx.discs = {'play': None, 'install': None}"),
 (37, B, "            ctx.discs = {role: DiscNamedAtBuild(role) for role in ('play', 'install')}", "            ctx.discs = {role: DiscNamedAtBuild(role) for role in ('play',)}"),
 (37, B, "            ctx.discs = {role: DiscNamedAtBuild(role) for role in ('play', 'install')}", "            ctx.discs = __import__('collections').defaultdict(lambda: DiscNamedAtBuild('any'))"),
 (36, B, "    return Path(root) / 'release.json' if root else Path(__file__).with_name('release.json')", "    return Path(__file__).with_name('release.json')"),
 (36, B, "    return Path(root) / 'release.json' if root else Path(__file__).with_name('release.json')", "    return Path(root or '.') / 'release.json'"),
 (36, B, "    ap.add_argument('--release', type=Path, default=default_release())", "    ap.add_argument('--release', type=Path, default=Path(__file__).with_name('release.json'))"),
 (2, H, "re.search(r':(?:\\s|$)', line)", "re.search(':', line)"),
 (3, H, "{'creationflags': 0x00004000} if amount else {}", "{}"),
 (4, H, "if sys.platform == 'darwin':", "if True:"),
 (6, R, "command, options = host_support.low_priority(host_support.python_command(self.tool, *argv))", "command, options = ['nice', '-n', '10', *host_support.python_command(self.tool, *argv)], {}"),
 (7, H, "if not path.is_relative_to(parent / 'local-analysis'):", "if False:"),
 (8, B, "    if not accepted:\n        raise Refusal('licence not accepted; read those terms, then rerun fetch with --accept-ndk-license if you agree')", "    if False:\n        raise Refusal('licence not accepted; read those terms, then rerun fetch with --accept-ndk-license if you agree')"),
 (9, B, "if total != n['size'] or h.hexdigest() != n['sha256']:", "if False:"),
 (10, B, "if not p.is_file() or db.sha256_file(p) != digest:", "if not p.is_file():"),
 (11, B, "raise Refusal('reserved Windows package path; obtain a corrected release')", "pass"),
 (13, B, "if not p.is_file() or db.sha256_file(p) != rec['sha256']:", "if not p.is_file():"),
 (15, D, "reason = receipt_verdict(read_receipt(ctx, sid), parts, outputs)", "reason = 'always rebuild'"),
 (16, D, "if len(match) != 1:", "if False:"),
 (17, D, "return '.exe' if sys.platform == 'win32' else ''", "return ''"),
 (18, 'tools/fp_codegen/leaf_locals.py', "newline='\\n'", "newline=None"),
 (19, D, "'host-support': hashlib.sha256(host_support.__loader__.get_data(host_support.__file__)).hexdigest()", "'host-support': '0' * 64"),
 (20, B, "not re.fullmatch('[0-9a-f]{64}', str(n.get('sha256')))", "False"),
 (25, R, "if name == 'host_support' and not q.exists():", "if False:"),
 (26, H, "return Path(os.environ.get('BUILDER_PACKAGE_ROOT', Path(source).resolve().parents[2])).resolve()", "return Path(source).resolve().parents[2]"),
 (27, 'tools/iter/generic_shader_pack.py', "ROOT = args.root.resolve()", "ROOT = Path(__file__).resolve().parents[2]"),
 (28, 'tools/fp_codegen/direct_calls.py', "encoding='utf-8', check=True", "encoding='utf-8', check=False"),
 (29, 'tools/own_disc/regen_check.py', "command, options = host_support.low_priority([str(a.generator), str(cfg), str(a.context)])", "command, options = ['nice', '-n', '10', str(a.generator), str(cfg), str(a.context)], {}"),
 (31, D, "key=lambda q: q.relative_to(directory).as_posix()", "key=lambda q: q"),
 (17, D, "                return host_support.ndk_host_tag()\n", "                return 'darwin-x86_64'\n"),
 (17, D, "                if not os.environ.get('BUILDER_HOST_TOOLS'):\n                    raise BuilderError('host tools missing: BUILDER_HOST_TOOLS names no folder', EXIT_PREREQ)\n", ""),
 (17, D, "                return os.environ['BUILDER_HOST_TOOLS']\n", "                return str(self.repo)\n"),
 (17, H, "NDK_HOST_TAGS.get('linux' if platform.startswith('linux') else platform)", "NDK_HOST_TAGS.get(platform)"),
 (17, H, "    if tag is None:\n        raise ValueError(f'no NDK host for {platform}')\n", "    if tag is None:\n        return 'linux-x86_64'\n"),
 (17, H, "'win32': 'windows-x86_64'", "'win32': 'windows'"),
 (34, H, "0x00004000: 10, 0x00000020: 0,", "0x00004000: 0, 0x00000020: 0,"),
 (34, H, "    if sys.platform == 'win32':\n        return nice_of_priority_class(_windows_priority_class())\n", ""),
 (34, H, "    if value not in WINDOWS_CLASS_NICE:\n        raise ValueError('unknown process priority class')\n    return WINDOWS_CLASS_NICE[value]", "    return WINDOWS_CLASS_NICE.get(value, 0)"),
 (35, H, "        os.lseek(fd, offset, os.SEEK_SET)\n", ""),
 (35, H, "        while left > 0:\n            chunk = os.read(fd, left)\n            if not chunk:\n                break\n            chunks.append(chunk)\n            left -= len(chunk)\n        return b''.join(chunks)", "        return os.read(fd, min(size, 64))"),
 (35, H, "    with _SEEK_LOCK:\n        os.lseek(fd, offset, os.SEEK_SET)", "    if True:\n        os.lseek(fd, offset, os.SEEK_SET)"),
 (35, H, "pread = os.pread if hasattr(os, 'pread') else pread_by_seek", "pread = pread_by_seek"),
]


def run(overlay, only=None):
    command = [sys.executable, '-B', '-m', 'unittest', 'discover', '-s', str(HERE), '-p', 'test_package.py', '-v']
    if only is not None:
        command += ['-k', f'test_{only:02}_']
    return subprocess.run(command, capture_output=True, text=True, env=dict(os.environ, PACKAGE_SRC=str(overlay), PYTHONDONTWRITEBYTECODE='1'))


def copy_tools(overlay):
    for folder in COPIED:
        shutil.copytree(REPO / folder, overlay / folder, ignore=shutil.ignore_patterns('__pycache__', 'tests'))


def main() -> int:
    killed = 0
    with tempfile.TemporaryDirectory() as tmp:
        overlay = Path(tmp).resolve() / 'control'            # resolved: a check compares the package root with this path
        copy_tools(overlay)
        if run(overlay).returncode != 0:
            print('CONTROL FAILED: the checks do not pass on an unchanged copy')
            return 1
        print('control: the checks pass on an unchanged copy of the tools', flush=True)
        for number, file, old, new in MUTANTS:
            text = (overlay / file).read_text(encoding='utf-8')
            if text.count(old) != 1:
                print(f'mutant {number:3d} DOES NOT APPLY ({text.count(old)} matches): {file}')
                continue
            (overlay / file).write_text(text.replace(old, new), encoding='utf-8', newline='\n')
            result = run(overlay, number)
            (overlay / file).write_text(text, encoding='utf-8', newline='\n')
            body = result.stdout + result.stderr
            # the named check must really have run: an import or syntax failure is no kill
            ran = 'Ran 1 test' in body and '_FailedTest' not in body and 'SyntaxError' not in body
            if not ran:
                print(f'mutant {number:3d} DID NOT RUN its check: {file}')
            elif result.returncode == 0:
                print(f'mutant {number:3d} SURVIVED: {file}: {old[:70]}')
            else:
                killed += 1
                print(f'mutant {number:3d} killed: {file}', flush=True)
    print(f'{killed} of {len(MUTANTS)} mutants killed')
    return 0 if killed == len(MUTANTS) else 1


if __name__ == '__main__':
    sys.exit(main())
