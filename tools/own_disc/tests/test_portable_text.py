#!/usr/bin/env python3
"""Every text file the builder's stage tools write is written as UTF-8 with LF line ends, on any host.

Left to the host's default, Windows would write CRLF and its own code page, and a stage output would differ from
the Mac's byte for byte. The check reads the source of every Python file the stages run: the tools a profile names
(TOOLS below: files, and folders that stand for every Python file in them) and what those import from this
project. In them, each call that writes text (write_text; open, Path.open, os.fdopen or a temporary file in a
writing text mode) must name encoding and newline. PORTABLE_TEXT_ROOT points the check at another copy of the
tools (the mutant runner beside this file). If the private profiles are there, every tool they name must be in
TOOLS: the list comes from what the stages run, not from memory."""
import ast
import json
import os
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
ROOT = Path(os.environ.get('PORTABLE_TEXT_ROOT', REPO))
TOOLS = '''
tools/fp_codegen/auto_prefetch/ap_pass.py tools/fp_codegen/direct_calls.py tools/fp_codegen/fence_pass.py
tools/fp_codegen/gpr_fastpath.py tools/fp_codegen/job_code/loop_locals_pass.py tools/fp_codegen/leaf_locals.py
tools/fp_codegen/main_code/dcbt_build.py tools/fp_codegen/recipe tools/fp_codegen/recipe_build.py
tools/fp_codegen/render_guest/rg_pass.py tools/fp_codegen/stitch_split_functions.py tools/fp_codegen/vmx_all.py
tools/host_guest_trace/content_catalog.py tools/host_guest_trace/file_metadata_catalog.py
tools/host_guest_trace/stfs_inventory.py tools/iter/generic_shader_pack.py
tools/own_disc/build_boot_shaders.py tools/own_disc/build_catalogs.py tools/own_disc/build_code_headers.py
tools/own_disc/build_native_packs.py tools/own_disc/build_shader_extras.py tools/own_disc/build_shader_pack.py
tools/own_disc/compile_rehearsal.py tools/own_disc/disc_builder.py tools/own_disc/regen_check.py
tools/own_disc/shader_override_rules.py tools/own_disc/host_support.py
tools/own_disc/builder.py tools/rpf7_inventory
tools/vulkan_probe tools/own_disc/build_shader_fixes.py tools/own_disc/build_xma_decoder.py tools/own_disc/elf_sections.py
tools/own_disc/shader_toolchain.py
'''.split()
SEARCH = ('tools/own_disc', 'tools/iter', 'tools/fp_codegen', 'tools/fp_codegen/recipe', 'tools/rpf7_inventory', 'tools/host_guest_trace', 'tools')
NOT_FILES = {'os', 'io', 'gzip', 'zipfile', 'tarfile', 'webbrowser', 'bz2', 'lzma'}
MODE = re.compile(r'[rwxabt+]+')


def python_files(root: Path, names) -> set:
    found = set()
    for name in names:
        path = root / name
        if path.is_dir():
            found |= {p for p in path.rglob('*.py') if 'tests' not in p.relative_to(root).parts and '__pycache__' not in p.parts}
        elif path.is_file():
            found.add(path)
        else:
            raise AssertionError(f'a tool of the list is missing: {name}')
    return found


def closure(root: Path, start: set) -> set:
    """The files of start and every project file they import (by name, from beside the importer or a tools folder)."""
    seen, todo = set(), list(start)
    while todo:
        path = todo.pop()
        if path in seen:
            continue
        seen.add(path)
        for node in ast.walk(ast.parse(path.read_text(encoding='utf-8'))):
            names = [a.name for a in node.names] if isinstance(node, ast.Import) else \
                    [node.module] + [node.module + '.' + a.name for a in node.names] if isinstance(node, ast.ImportFrom) and node.module else []
            for name in names:
                for base in (path.parent, *(root / s for s in SEARCH)):
                    module = base.joinpath(*name.split('.')).with_suffix('.py')
                    if module.is_file():
                        todo.append(module)
                        break
    return seen


def text_write(node):
    """None, or the keyword names of a call that writes text; {'?'} when a built-in open's mode cannot be read."""
    if not isinstance(node, ast.Call):
        return None
    keywords = {k.arg for k in node.keywords}
    named = next((k.value for k in node.keywords if k.arg == 'mode'), None)
    f = node.func
    if isinstance(f, ast.Attribute) and f.attr == 'write_text':
        return keywords
    if isinstance(f, ast.Name) and f.id == 'open':
        mode = node.args[1] if len(node.args) > 1 else named
        if mode is None:
            return None
        if not isinstance(mode, ast.Constant):
            return {'?'}
    elif isinstance(f, ast.Attribute) and f.attr == 'fdopen' and isinstance(f.value, ast.Name) and f.value.id == 'os':
        mode = node.args[1] if len(node.args) > 1 else named
    elif isinstance(f, ast.Attribute) and f.attr in ('NamedTemporaryFile', 'TemporaryFile', 'SpooledTemporaryFile'):
        mode = node.args[0] if node.args else named
    elif isinstance(f, ast.Attribute) and f.attr == 'open' and not (isinstance(f.value, ast.Name) and f.value.id in NOT_FILES):
        mode = node.args[0] if node.args else named        # (a path's open; an archive's open takes a member first)
    else:
        return None
    if not isinstance(mode, ast.Constant) or not isinstance(mode.value, str) or not MODE.fullmatch(mode.value):
        return None
    return keywords if 'b' not in mode.value and any(c in mode.value for c in 'wax+') else None


def faults(path: Path) -> list:
    out = []
    for node in ast.walk(ast.parse(path.read_text(encoding='utf-8'))):
        keywords = text_write(node)
        if keywords is None:
            continue
        if '?' in keywords:
            out.append((node.lineno, 'a mode that cannot be read'))
            continue
        given = {k.arg: k.value for k in node.keywords}
        if not (isinstance(given.get('encoding'), ast.Constant) and str(given['encoding'].value).lower().replace('-', '') == 'utf8'):
            out.append((node.lineno, 'no encoding="utf-8"'))
        if not (isinstance(given.get('newline'), ast.Constant) and given['newline'].value in ('\n', '')):
            out.append((node.lineno, 'no newline="\\n"'))
    return out


class PortableText(unittest.TestCase):
    def test_every_text_write_of_the_stage_tools_names_utf8_and_lf(self):
        files = closure(ROOT, python_files(ROOT, TOOLS))
        found = {str(p.relative_to(ROOT)): f for p in sorted(files) for f in [faults(p)] if f}
        self.assertEqual(found, {}, 'text written with the host\'s default line ends or encoding')

    def test_the_check_sees_each_form_of_text_write(self):
        def lines(source):
            tree = ast.parse(source)
            return sorted({n.lineno for n in ast.walk(tree) if text_write(n) is not None})
        source = '\n'.join([
            "p.write_text(t)",                                   # 1
            "open(p, 'w')",                                      # 2
            "p.open('x')",                                       # 3
            "os.fdopen(fd, 'w')",                                # 4
            "tempfile.NamedTemporaryFile('w', dir=d)",           # 5
            "open(p, mode='a')",                                 # 6
            "open(p, mode)",                                     # 7 (cannot be read: reported)
            "open(p, 'wb')", "open(p)", "p.open('rb')", "p.read_text()", "zf.open(member, 'w')", "os.open(p, flags)",
            "os.fdopen(fd, 'wb')", "disc.open(rel)", "gzip.open(p, 'wt')", "p.write_bytes(b)",
        ])
        self.assertEqual(lines(source), [1, 2, 3, 4, 5, 6, 7])
        good = "p.write_text(t, encoding='utf-8', newline='\\n')\nopen(p, 'w', encoding='UTF-8', newline='')\n"
        bad = "p.write_text(t, encoding='utf-8')\np.write_text(t, newline='\\n')\np.write_text(t, encoding='latin-1', newline='\\r\\n')\nopen(p, m)\n"
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'good.py').write_text(good, encoding='utf-8', newline='\n')
            (Path(tmp) / 'bad.py').write_text(bad, encoding='utf-8', newline='\n')
            self.assertEqual(faults(Path(tmp) / 'good.py'), [])
            self.assertEqual(faults(Path(tmp) / 'bad.py'), [(1, 'no newline="\\n"'), (2, 'no encoding="utf-8"'), (3, 'no encoding="utf-8"'),
                                                           (3, 'no newline="\\n"'), (4, 'a mode that cannot be read')])

    def test_the_closure_follows_imports_and_folders(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, text in (('tools/own_disc/a.py', 'import b\nfrom c import d\n'), ('tools/own_disc/b.py', 'import json\n'),
                               ('tools/iter/c.py', 'd = 1\n'), ('tools/rpf7_inventory/x/y.py', ''), ('tools/rpf7_inventory/tests/t.py', ''),
                               ('tools/own_disc/unused.py', '')):
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text, encoding='utf-8', newline='\n')
            got = closure(root, python_files(root, ['tools/own_disc/a.py', 'tools/rpf7_inventory']))
            self.assertEqual(sorted(str(p.relative_to(root)) for p in got),
                             ['tools/iter/c.py', 'tools/own_disc/a.py', 'tools/own_disc/b.py', 'tools/rpf7_inventory/x/y.py'])
            with self.assertRaises(AssertionError):
                python_files(root, ['tools/own_disc/gone.py'])

    def test_the_list_holds_every_tool_the_private_profiles_name(self):
        # the builder's current profiles (the -3 generation, 2026-11-05: tools/fp_codegen)
        profiles = sorted((REPO / 'local-analysis/builder/thor-rc1').glob('profile-thor-fivepocket-*-3.json'))
        if not profiles:
            self.skipTest('no private profile on this machine')
        listed = python_files(REPO, TOOLS)
        for profile in profiles[-2:]:
            for stage in json.loads(profile.read_text(encoding='utf-8'))['stages']:
                for tool in stage.get('tools', []):
                    tool = tool['path'] if isinstance(tool, dict) else tool
                    if tool.startswith('${repo}/') and (tool.endswith('.py') or (REPO / tool[8:]).is_dir()):
                        for path in python_files(REPO, [tool[8:]]):
                            self.assertTrue(path in listed, f'{profile.name} runs {tool} ({path.name}): add it to TOOLS')


if __name__ == '__main__':
    unittest.main()
