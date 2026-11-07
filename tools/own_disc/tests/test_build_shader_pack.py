"""Synthetic connected producer/consumer checks; no game or key file fixtures."""
import argparse
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(os.environ.get('SHADER_TEST_REPO', Path(__file__).resolve().parents[3])).resolve()
MODULE = Path(os.environ.get('SHADER_TEST_MODULE', Path(__file__).resolve().parents[1] / 'build_shader_pack.py'))
for directory in ('tools/own_disc', 'tools/own_disc/tests', 'tools/rpf7_inventory', 'tools/host_guest_trace', 'tools/host_guest_trace/tests', 'tools/iter'):
    sys.path.insert(0, str(ROOT / directory))
spec = importlib.util.spec_from_file_location('shader_stage_under_test', MODULE)
m = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = m
spec.loader.exec_module(m)
from test_disc_builder import make_xiso
from generic_shader_pack import parse_container, canonical_microcode, fnv1a64

SCRATCH = Path(os.environ.get('SHADER_TEST_SCRATCH', ROOT / 'local-analysis/shader-stage-tests'))
SCRATCH.mkdir(parents=True, exist_ok=True)


def container():
    data = bytearray(80)
    struct.pack_into('>9I', data, 0, 0x102a1100, 68, 12, 0, 36, 0, 36, 0, 0)
    struct.pack_into('>II', data, 36, 0, 12)
    return bytes(data)


def variant(v):
    """A container whose microcode, and so its runtime key, depends on v."""
    data = bytearray(container())
    struct.pack_into('>I', data, 76, v)
    return bytes(data)


def spirv(v):
    return struct.pack('<5I', 0x07230203, 0x10300, v, 1, 0)


def translation(root, variants):
    """A full translation as the translate step leaves it: records-000/shader-NNNN plus translation.json."""
    root.mkdir(parents=True)
    records = []
    for i, (v, status) in enumerate(variants, 1):
        d = root / 'records-000' / f'shader-{i:04d}'
        d.mkdir(parents=True)
        blob = variant(v)
        (d / 'container.xenos').write_bytes(blob)
        (d / 'translated.spv').write_bytes(spirv(i))
        (d / 'checkpoint.json').write_text(json.dumps({'records': [{'index': i, 'stage': 'pixel', 'status': status.split('/')[-1]}]}))
        status = status.split('/')[0]     # 'validated/failed': the report says validated, the record's own checkpoint does not
        key = f"{fnv1a64(canonical_microcode(parse_container(blob, 'pixel')[0])):016x}"
        records.append(dict(container_sha256=m.sha(blob), key=key, stage='pixel', status=status, record=f'records-000/shader-{i:04d}'))
    (root / 'translation.json').write_text(json.dumps(dict(schema=1, status='incomplete', records=records)))
    return [r['key'] for r in records]


def key_file(path, keys, schema='shader-keys/1'):
    path.write_text(json.dumps(dict(schema=schema, keys=keys)))
    return path


def rpf(name, payload):
    names = b'\0' + name.encode() + b'\0'
    root = bytes.fromhex('7fffff0000000000') + struct.pack('>II', 1, 1)
    record = bytes.fromhex('0000010000000001') + struct.pack('>II', len(payload), 0)
    prefix = b'RPF7' + struct.pack('>III', 2, len(names), 0) + root + record + names
    return prefix.ljust(512, b'\0') + payload


def pack(key=1, stage=1, code=None, outputs=0):
    code = code or struct.pack('<5I', 0x07230203, 0x10300, 0, 1, 0)
    entry = struct.pack('<QIIII', key, stage, 168, len(code)//4, 0) + bytes(96) + struct.pack('<II', outputs, 0) + bytes(24)
    return struct.pack('<4sIII', b'GSP2', 2, 1, 0) + entry + code


def shader_fields():
    import shader_toolchain
    return shader_toolchain.BASE


def write_inventory(path):
    path.mkdir()
    blob = container()
    (path/'a.xenos').write_bytes(blob)
    microcode, *_ = parse_container(blob, 'pixel')
    canon = canonical_microcode(microcode)
    row = dict(file='a.xenos', bytes=len(blob), sha256=m.sha(blob), stage='pixel',
               key=f'{fnv1a64(canon):016x}', canonical_sha256=m.sha(canon),
               origins=[dict(origin='play:/common.rpf!/shaders/fxl_final_final/a.fxc', offset=9)])
    manifest = dict(schema=1, source='disc', key_files_used=False, containers=[row],
                    unresolved=[], status='complete')
    (path/'inventory.json').write_text(json.dumps(manifest))
    return manifest


class Checks(unittest.TestCase):
    def temporary(self):
        return tempfile.TemporaryDirectory(dir=SCRATCH)

    def test_connected_disc_archive_container_runtime_key_and_pack(self):
        with self.temporary() as temp:
            root = Path(temp)
            code = container()
            fxc = b'rgxe' + struct.pack('<I', 7) + b'x' + code
            archive = rpf('nested.rpf', rpf('effect.fxc', fxc))
            play, install = root/'play.iso', root/'install.iso'
            make_xiso(play, {'common.rpf': archive})
            make_xiso(install, {'common.rpf': archive})
            generated = root/'generated'; generated.mkdir()
            text = 'PPC_FUNC_IMPL(__imp__sub_10000) {\n'
            for i in range(32):
                text += f'// lis r4,1\n// li r3,{i}\n// stb r3,{i}(r4)\n'
            (generated/'ppc_recomp.0.cpp').write_text(text + '}\n')
            args = argparse.Namespace(output=root/'out', generated=generated, key_address=0x10000,
                play=play, install=install, title_id=1, xmem=Path('/unused'))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(m.extract(args), 0)
            inv, rows = m.validate_inventory(args.output)
            self.assertEqual((len(inv['archives']), len(inv['effects']), len(rows)), (4, 2, 1))
            self.assertEqual(inv['status'], 'complete')
            self.assertEqual((args.output/rows[0]['file']).read_bytes(), code)
            self.assertEqual(len(rows[0]['origins']), 2)
            self.assertTrue(all(p['offset'] == 9 for p in rows[0]['origins']))
            # Feed the recovered container into the production pack producer.
            corpus = root/'records'/'shader-0001'; corpus.mkdir(parents=True)
            (corpus/'container.xenos').write_bytes(code)
            spv = struct.pack('<5I', 0x07230203, 0x10300, 0, 1, 0)  # framing-only authored module
            (corpus/'translated.spv').write_bytes(spv)
            (corpus/'checkpoint.json').write_text(json.dumps({'records':[{'index':1,'stage':'pixel','status':'validated'}]}))
            result = subprocess.run([sys.executable,str(ROOT/'tools/iter/generic_shader_pack.py'),
                '--corpus',str(corpus.relative_to(ROOT)), '--out',str(root/'pack.bin')], capture_output=True)
            self.assertEqual(result.returncode, 0)
            entries = m.read_pack((root/'pack.bin').read_bytes())
            self.assertEqual(set(entries), {rows[0]['key']})
            self.assertEqual(entries[rows[0]['key']]['spirv_sha256'], m.sha(spv))

    def test_extract_records_the_title_packages_it_keeps(self):
        from stfs_fixture import fixture as stfs_package, TITLE
        with self.temporary() as temp:
            root = Path(temp)
            stfs_package(root/'package.bin')
            blob = (root/'package.bin').read_bytes()
            archive = rpf('nested.rpf', rpf('effect.fxc', b'rgxe' + struct.pack('<I', 7) + b'x' + container()))
            play, install = root/'play.iso', root/'install.iso'
            make_xiso(play, {'common.rpf': archive})
            make_xiso(install, {'AB00': blob, 'common.rpf': archive})
            generated = root/'generated'; generated.mkdir()
            text = 'PPC_FUNC_IMPL(__imp__sub_10000) {\n'
            for i in range(32):
                text += f'// lis r4,1\n// li r3,{i}\n// stb r3,{i}(r4)\n'
            (generated/'ppc_recomp.0.cpp').write_text(text + '}\n')
            args = argparse.Namespace(output=root/'out', generated=generated, key_address=0x10000,
                play=play, install=install, title_id=TITLE, xmem=Path('/unused'))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(m.extract(args), 0)
            inventory = json.loads((args.output/'inventory.json').read_text())
            self.assertEqual(inventory['packages'], [dict(origin='install:/ab00', file='packages/package-000', bytes=len(blob))])
            self.assertEqual((args.output/'packages/package-000').read_bytes(), blob)
            self.assertEqual(inventory['skipped_packages'], [])

    def select(self, root, keys, variants=((1, 'validated'), (2, 'validated'), (3, 'validated')), expect=None, schema='shader-keys/1'):
        made = translation(root / 'full', variants)
        chosen = keys(made) if callable(keys) else keys
        args = argparse.Namespace(repo=ROOT, translation=(root / 'full').resolve(), keys=key_file(root / 'keys.json', chosen, schema),
                                  output=(root / 'out').resolve(), expect_sha256=expect)
        return made, args

    def test_select_writes_exactly_the_listed_keys_with_the_production_packer(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp)
            made, args = self.select(root, lambda k: sorted([k[0], k[2]]))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(m.select(args), 0)
            data = (args.output / 'generic-shaders.bin').read_bytes()
            entries = m.read_pack(data)
            self.assertEqual(set(entries), {made[0], made[2]})
            self.assertEqual(entries[made[2]]['spirv_sha256'], m.sha(spirv(3)))
            # the same bytes as the production packer gives when handed those two records directly
            direct = subprocess.run([sys.executable, str(ROOT / 'tools/iter/generic_shader_pack.py'), '--out', str(root / 'direct.bin'),
                '--corpus', str((root / 'full/records-000/shader-0001').relative_to(ROOT)),
                '--corpus', str((root / 'full/records-000/shader-0003').relative_to(ROOT))], capture_output=True)
            self.assertEqual(direct.returncode, 0)
            self.assertEqual(data, (root / 'direct.bin').read_bytes())
            record = json.loads((args.output / 'selection.json').read_text())
            self.assertEqual((record['status'], record['keys'], record['pack_sha256']), ('complete', 2, m.sha(data)))

    def test_select_refuses_a_key_the_translation_does_not_hold(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp)
            made, args = self.select(root, lambda k: sorted([k[0], 'ffffffffffffffff']))
            with self.assertRaises(ValueError):
                m.select(args)
            self.assertFalse((args.output / 'generic-shaders.bin').exists())

    def test_select_refuses_a_key_whose_translation_was_not_validated(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp)
            made, args = self.select(root, lambda k: sorted(k[:2]), variants=((1, 'validated'), (2, 'failed')))
            with self.assertRaises(ValueError):
                m.select(args)
            self.assertFalse(args.output.exists())

    def test_select_refuses_a_pack_that_lost_a_listed_key(self):
        # the report calls the second record validated, its own checkpoint does not: the production packer leaves it out
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp)
            made, args = self.select(root, lambda k: sorted(k[:2]), variants=((1, 'validated'), (2, 'validated/failed')))
            with self.assertRaises(ValueError):
                m.select(args)
            self.assertFalse((args.output / 'selection.json').exists())

    def test_select_refuses_key_lists_of_another_shape(self):
        for bad, schema in ((lambda k: list(reversed(sorted(k))), 'shader-keys/1'), (lambda k: [k[0], k[0]], 'shader-keys/1'),
                            (lambda k: [], 'shader-keys/1'), (lambda k: [k[0], 5], 'shader-keys/1'),
                            (lambda k: sorted(k), 'shader-keys/2')):
            with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
                made, args = self.select(Path(tmp), bad, schema=schema)
                with self.assertRaises(ValueError):
                    m.select(args)
                self.assertFalse(args.output.exists())

    def test_select_reports_a_pack_that_is_not_the_expected_one(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp)
            made, args = self.select(root, lambda k: sorted(k), expect='0' * 64)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(m.select(args), 1)
            self.assertEqual(json.loads((args.output / 'selection.json').read_text())['status'], 'differs')
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp)
            made, args = self.select(root, lambda k: sorted(k))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(m.select(args), 0)
            digest = m.sha((args.output / 'generic-shaders.bin').read_bytes())
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp)
            made, args = self.select(root, lambda k: sorted(k), expect=digest)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(m.select(args), 0)

    def test_select_never_writes_into_an_existing_output(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as tmp:
            root = Path(tmp)
            made, args = self.select(root, lambda k: sorted(k))
            args.output.mkdir()
            (args.output / 'earlier.txt').write_text('evidence')
            with self.assertRaises(FileExistsError):
                m.select(args)
            self.assertEqual(sorted(p.name for p in args.output.iterdir()), ['earlier.txt'])

    def test_bounded_view_refuses_overread_and_short_read(self):
        view = m.View(3, lambda off, size: b'abc'[off:off+size])
        self.assertEqual(view.sub(1, 2).read(0, 2), b'bc')
        with self.assertRaises(ValueError): view.read(2, 2)
        with self.assertRaises(ValueError): m.View(4, lambda off, n: b'x').read(0, 3)

    def test_source_identity_is_checked(self):
        with self.temporary() as temp:
            p=Path(temp)/'inventory'; write_inventory(p)
            (p/'a.xenos').write_bytes(container()[:-1]+b'x')
            with self.assertRaisesRegex(ValueError, 'identity'): m.validate_inventory(p)

    def test_runtime_key_is_checked(self):
        with self.temporary() as temp:
            p=Path(temp)/'inventory'; inv=write_inventory(p)
            inv['containers'][0]['key']='ffffffffffffffff'
            (p/'inventory.json').write_text(json.dumps(inv))
            with self.assertRaisesRegex(ValueError, 'runtime key'): m.validate_inventory(p)

    def test_pack_comparison_includes_metadata_and_modules(self):
        base=m.read_pack(pack())
        changed=m.read_pack(pack(outputs=1))
        self.assertEqual(m.compare_entries(base,changed)['different'], ['0000000000000001'])
        self.assertEqual(m.compare_entries(base,base)['same'], ['0000000000000001'])
        other=m.read_pack(pack(key=2))
        self.assertEqual((len(m.compare_entries(base,other)['missing']),len(m.compare_entries(base,other)['extra'])),(1,1))
        altered=m.read_pack(pack(code=struct.pack('<5I',0x07230203,0x10300,0,2,0)))
        self.assertEqual(len(m.compare_entries(base,altered)['different']),1)

    def test_pack_duplicate_keys_refused(self):
        single=pack();entry=bytearray(single[16:168]);blob=single[168:]
        struct.pack_into('<I',entry,12,320)
        second=bytearray(entry);struct.pack_into('<I',second,12,320+len(blob))
        data=struct.pack('<4sIII',b'GSP2',2,2,0)+entry+second+blob+blob
        with self.assertRaisesRegex(ValueError,'pack entry'):m.read_pack(data)

    def test_pack_bounds_refuse_truncation(self):
        self.assertEqual(len(m.read_pack(pack())),1)
        with self.assertRaisesRegex(ValueError,'pack entry'):m.read_pack(pack()[:-4])
        bad=bytearray(pack()); struct.pack_into('<I',bad,28,16)
        with self.assertRaisesRegex(ValueError,'pack entry'):m.read_pack(bytes(bad))

    def test_pack_trailing_bytes_refused(self):
        with self.assertRaisesRegex(ValueError,'trailing'):m.read_pack(pack()+bytes(4))

    def test_bulk_requires_effective_nice(self):
        with mock.patch.object(m.os,'getpriority',return_value=0):
            m.require_bulk_priority(8)
            with self.assertRaisesRegex(ValueError,'nice'):m.require_bulk_priority(9)
        with mock.patch.object(m.os,'getpriority',return_value=5):m.require_bulk_priority(8294)
        # a host without nice (Windows): the priority class stands for it; normal is refused, below normal passes
        with mock.patch.object(m.host_support.sys,'platform','win32'), mock.patch.object(m.os,'getpriority',side_effect=AssertionError('no POSIX call')):
            with mock.patch.object(m.host_support,'_windows_priority_class',return_value=0x20):
                m.require_bulk_priority(8)
                with self.assertRaisesRegex(ValueError,'nice'):m.require_bulk_priority(9)
            with mock.patch.object(m.host_support,'_windows_priority_class',return_value=0x4000):m.require_bulk_priority(8294)

    def test_dashboard_exclusion_does_not_skip_unknown_game_content(self):
        self.assertEqual(m.package_disposition(1,1,0x30000),'dashboard-picture-or-theme')
        self.assertIsNone(m.package_disposition(1,1,2))
        self.assertIsNone(m.package_disposition(1,1,0xB0000))
        self.assertEqual(m.package_disposition(2,1,2),'different-title')

    def test_translation_rejects_changed_producer_container(self):
        import xenos_translate_batch as converter
        with self.temporary() as temp:
            root=Path(temp); inventory=root/'inventory';write_inventory(inventory)
            header=root/'common.h';header.write_text('// authored fixture')
            libraries=root/'libraries';libraries.mkdir()
            config={name:str(Path(sys.executable).resolve()) for name in ('translator','preflight','dxc','spirv_validator')}
            config.update(common_header=str(header),compiler_library_directory=str(libraries))
            config_path=root/'toolchain.json';config_path.write_text(json.dumps(config))
            def changed(inputs, directory, **kwargs):
                record=directory/'shader-0001';record.mkdir(parents=True)
                (record/'container.xenos').write_bytes(container()[:-1]+b'x')
                (directory/'artifact-identities.json').write_text(json.dumps({'artifacts':[]}))
                return {'records':[{'index':1,'stage':'pixel','status':'preflight-failed'}], 'validated_container_count':0}
            args=argparse.Namespace(inventory=inventory, toolchain=config_path, output=root/'translated',
                repo=ROOT, limit=1, check=False)
            with mock.patch.object(converter,'translate_batch',side_effect=changed):
                with self.assertRaisesRegex(ValueError,'changed from inventory'):m.translate(args)

    def translated(self, root, unresolved, status='validated', unresolved_ok=False):
        import xenos_translate_batch as converter
        inventory = root/'inventory'
        manifest = write_inventory(inventory)
        if unresolved:
            manifest.update(unresolved=[dict(origin='play:/x.fxc', reason='incomplete-fxc-staging')], status='incomplete')
            (inventory/'inventory.json').write_text(json.dumps(manifest))
        header = root/'common.h'; header.write_text('// authored fixture')
        libraries = root/'libraries'; libraries.mkdir()
        config = {name: str(Path(sys.executable).resolve()) for name in ('translator', 'preflight', 'dxc', 'spirv_validator')}
        config.update(common_header=str(header), compiler_library_directory=str(libraries))
        (root/'toolchain.json').write_text(json.dumps(config))

        def batch(inputs, directory, **kwargs):
            record = directory/'shader-0001'; record.mkdir(parents=True)
            blob, spv = container(), spirv(1)
            (record/'container.xenos').write_bytes(blob)
            (record/'translated.spv').write_bytes(spv)
            (record/'checkpoint.json').write_text(json.dumps({'records': [{'index': 1, 'stage': 'pixel', 'status': status}]}))
            (directory/'artifact-identities.json').write_text(json.dumps({'artifacts': [
                {'index': 1, 'container_sha256': m.sha(blob), 'spirv_sha256': m.sha(spv)}]}))
            return {'records': [{'index': 1, 'stage': 'pixel', 'status': status}], 'validated_container_count': int(status == 'validated')}
        args = argparse.Namespace(inventory=inventory, toolchain=root/'toolchain.json', output=root/'translated', repo=ROOT,
                                  limit=None, check=False, jobs=1, unresolved_ok=unresolved_ok)
        with mock.patch.object(converter, 'translate_batch', side_effect=batch), contextlib.redirect_stdout(io.StringIO()):
            try:
                return m.translate(args)
            except ValueError as error:
                return str(error)

    def test_unresolved_effects_fail_the_translation_unless_coverage_is_left_to_selection(self):
        with self.temporary() as temp:
            self.assertEqual(self.translated(Path(temp), unresolved=False), 0)
        with self.temporary() as temp:
            self.assertEqual(self.translated(Path(temp), unresolved=True), 1)
        with self.temporary() as temp:
            self.assertEqual(self.translated(Path(temp), unresolved=True, unresolved_ok=True), 0)

    def test_a_failed_translation_fails_the_stage_whatever_the_flag(self):
        with self.temporary() as temp:
            self.assertNotEqual(self.translated(Path(temp), unresolved=True, status='preflight-failed', unresolved_ok=True), 0)

    def test_workers_capped_at_four(self):
        with self.temporary() as temp:
            root=Path(temp);inventory=root/'inventory';write_inventory(inventory)
            header=root/'common.h';header.write_text('// authored fixture')
            libraries=root/'libraries';libraries.mkdir()
            config={name:str(Path(sys.executable).resolve()) for name in ('translator','preflight','dxc','spirv_validator')}
            config.update(common_header=str(header),compiler_library_directory=str(libraries))
            config_path=root/'toolchain.json';config_path.write_text(json.dumps(config))
            args=argparse.Namespace(inventory=inventory,toolchain=config_path,output=root/'out',
                repo=ROOT,limit=None,check=True,jobs=5)
            with self.assertRaisesRegex(ValueError,'1..4'):m.translate(args)
            self.assertFalse(args.output.exists())

    def test_worker_preserves_project_cli_failure_reason(self):
        with self.temporary() as temp:
            root=Path(temp)
            result=subprocess.run([sys.executable,str(MODULE),'--repo',str(ROOT),'worker',
                str(root/'records'),str(root/'absent.xenos'),'--spirv-validator',str(Path(sys.executable).resolve())],
                capture_output=True)
            self.assertEqual(result.returncode,2)
            self.assertIn(b'SHADER_WORKER_REASON XenosPreflightError private input is unavailable',result.stderr)
            self.assertFalse((root/'records').exists())

    def toolchain_folders(self, root):
        """A host-tools folder and an NDK as shader_toolchain names them on this host: every file there, programs startable."""
        import shader_toolchain
        host_tools, ndk = root/'host-tools', root/'ndk'
        named = shader_toolchain.names(host_tools, ndk, extras=True)
        for field, path in named.items():
            path = Path(path)
            if field == 'compiler_library_directory':
                path.mkdir(parents=True, exist_ok=True)
                continue
            path.parent.mkdir(parents=True, exist_ok=True); path.write_text('authored fixture'); path.chmod(0o755)
        return host_tools, ndk, named

    def test_the_toolchain_named_from_the_two_folders_is_what_the_translation_takes(self):
        with self.temporary() as temp:
            root = Path(temp); inventory = root/'inventory'; write_inventory(inventory)
            host_tools, ndk, named = self.toolchain_folders(root)
            args = argparse.Namespace(inventory=inventory, toolchain=None, host_tools=host_tools, ndk=ndk, output=root/'out',
                                      repo=ROOT, limit=None, check=True, jobs=1)
            with contextlib.redirect_stdout(io.StringIO()) as said:
                self.assertEqual(m.translate(args), 0)
            self.assertIn('SHADER_STAGE_CHECK PASS', said.getvalue())
            # the command line takes the two folders too
            done = subprocess.run([sys.executable, str(MODULE), '--repo', str(ROOT), 'translate', '--inventory', str(inventory),
                                   '--host-tools', str(host_tools), '--ndk', str(ndk), '--output', str(root/'out'), '--check'], capture_output=True)
            self.assertEqual((done.returncode, b'SHADER_STAGE_CHECK PASS' in done.stdout), (0, True), done.stderr)
            # both ways at once, or neither: refused
            file = root/'toolchain.json'; file.write_text(json.dumps({k: named[k] for k in shader_fields()}))
            for given in (dict(toolchain=file), dict(host_tools=None), dict(ndk=None)):
                with self.assertRaisesRegex(ValueError, 'toolchain refused'):
                    m.translate(argparse.Namespace(**{**vars(args), **given}))
            # the file alone is taken as before; a file with other fields than the six is refused
            alone = argparse.Namespace(**{**vars(args), 'toolchain': file, 'host_tools': None, 'ndk': None})
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(m.translate(alone), 0)
            file.write_text(json.dumps(named))
            with self.assertRaisesRegex(ValueError, 'toolchain fields refused'):
                m.translate(alone)
            # a program the folders should hold is missing: refused before anything is written
            Path(named['preflight']).unlink()
            with self.assertRaises(FileNotFoundError):
                m.translate(args)
            self.assertFalse(args.output.exists())

    def test_recorded_names_have_forward_slashes_on_every_host(self):
        from pathlib import PureWindowsPath
        self.assertEqual(m.record_name(PureWindowsPath(r'D:\work\out\containers\a.xenos'), PureWindowsPath(r'D:\work\out')),
                         'containers/a.xenos')

    def test_every_recorded_name_is_made_by_the_one_rule(self):
        # the inventory's three kinds of file and the translation's record: with the rule replaced by a marked one,
        # every recorded name carries the mark
        from stfs_fixture import fixture as stfs_package, TITLE
        marked = lambda path, base: 'RULE:' + path.relative_to(base).as_posix()
        with self.temporary() as temp:
            root = Path(temp)
            stfs_package(root/'package.bin')
            blob = (root/'package.bin').read_bytes()
            archive = rpf('nested.rpf', rpf('effect.fxc', b'rgxe' + struct.pack('<I', 7) + b'x' + container()))
            play, install = root/'play.iso', root/'install.iso'
            make_xiso(play, {'common.rpf': archive})
            make_xiso(install, {'AB00': blob, 'common.rpf': archive})
            generated = root/'generated'; generated.mkdir()
            text = 'PPC_FUNC_IMPL(__imp__sub_10000) {\n'
            for i in range(32):
                text += f'// lis r4,1\n// li r3,{i}\n// stb r3,{i}(r4)\n'
            (generated/'ppc_recomp.0.cpp').write_text(text + '}\n')
            args = argparse.Namespace(output=root/'out', generated=generated, key_address=0x10000,
                play=play, install=install, title_id=TITLE, xmem=Path('/unused'))
            with mock.patch.object(m, 'record_name', marked), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(m.extract(args), 0)
            inventory = json.loads((args.output/'inventory.json').read_text())
            self.assertTrue(inventory['effects'] and all(e['file'].startswith('RULE:fxc/') for e in inventory['effects']))
            self.assertTrue(inventory['containers'] and all(c['file'].startswith('RULE:containers/') for c in inventory['containers']))
            self.assertEqual([p['file'] for p in inventory['packages']], ['RULE:packages/package-000'])
        with self.temporary() as temp:
            root = Path(temp)
            with mock.patch.object(m, 'record_name', marked):
                self.assertEqual(self.translated(root, unresolved=False), 0)
            records = json.loads((root/'translated'/'translation.json').read_text())['records']
            self.assertEqual([r['record'] for r in records], ['RULE:records-000/shader-0001'])

    def test_output_never_overwrites_evidence(self):
        with self.temporary() as temp:
            p=Path(temp)/'report.json';m.write_json(p,{'a':1})
            before=p.read_bytes()
            with self.assertRaises(FileExistsError):m.write_json(p,{'a':2})
            self.assertEqual(p.read_bytes(),before)


if __name__ == '__main__':
    unittest.main(verbosity=2)
