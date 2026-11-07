# SPDX-License-Identifier: GPL-3.0-or-later
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import bootstrap


class SourceBootstrap(unittest.TestCase):
    def test_unpinned_source_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'lock.json'
            path.write_text(json.dumps({'schema':'duo-source-lock/1','sources':{'tool':{'revision':'main'}}}))
            with self.assertRaisesRegex(ValueError,'exact Git commits'):
                bootstrap.load_lock(path)

    def test_an_existing_checkout_must_match_its_pin(self):
        with tempfile.TemporaryDirectory() as temporary:
            work=Path(temporary);(work/'tool').mkdir()
            source={'directory':'tool','url':'https://example.invalid/tool','revision':'1'*40,'submodules':[]}
            with mock.patch.object(bootstrap,'run',return_value=subprocess.CompletedProcess([],0,'2'*40+'\n','')) as run:
                with self.assertRaisesRegex(ValueError,'another revision'):
                    bootstrap.checkout(source,work)
            self.assertEqual(run.call_count,1)

    def test_verified_ndk_cache_does_not_request_new_license_acceptance(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache=Path(temporary)
            pin=bootstrap.load_lock()['ndk']
            receipt=cache/pin['sha256'][:16]/'installed.json'
            receipt.parent.mkdir();receipt.write_text('{}')
            with mock.patch.object(bootstrap.ndk,'fetch',side_effect=AssertionError('must reuse cache')), \
                    mock.patch.object(bootstrap.ndk,'existing_ndk',return_value=cache/'compiler'):
                self.assertEqual(bootstrap.ensure_ndk(cache),cache/'compiler')

    def test_changed_cached_source_archive_is_not_used_for_audio(self):
        with tempfile.TemporaryDirectory() as temporary:
            work=Path(temporary);(work/'ffmpeg-source.tar').write_bytes(b'changed cached source')
            def archive(command,**kwargs):
                kwargs['stdout'].write(b'pinned git archive')
            with mock.patch.object(bootstrap,'run',side_effect=archive), \
                    mock.patch.object(bootstrap.build_xma_decoder,'build',side_effect=AssertionError('cannot compile unverified source')):
                with self.assertRaisesRegex(ValueError,'differs from its pinned Git tree'):
                    bootstrap.build_ffmpeg(work,work/'source',work/'xcode',1)


if __name__=='__main__':
    unittest.main()
