"""Real archive tests for the maintained fork's test-only package assembly."""
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
import sys

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location('pack', Path(__file__).with_name('accomplish-package.py'))
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


def archive(path, entries):
    with tarfile.open(path, 'w:gz') as output:
        for name, (data, mode) in entries.items():
            item = tarfile.TarInfo('package/' + name)
            item.size = len(data)
            item.mode = mode
            output.addfile(item, io.BytesIO(data))


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.candidate = {
            'package.json': (json.dumps({'name': 'node-pty', 'version': '1.1.0', 'files': ['lib/', 'src/', 'prebuilds/'], 'scripts': {'install': 'node scripts/prebuild.js || node-gyp rebuild'}}).encode(), 0o644),
            'src/unix/pty.cc': (b'unchanged unix native source', 0o644),
            'lib/windowsPtyAgent.js': (b'new javascript', 0o644),
            **{f'prebuilds/win32-x64/{name}': (b'new ' + name.encode(), 0o644) for name in pack.WINDOWS_REQUIRED},
        }
        self.original = {
            'package.json': (b'{"name":"node-pty","version":"1.1.0"}', 0o644),
            'src/unix/pty.cc': self.candidate['src/unix/pty.cc'],
            **{f'prebuilds/darwin-{arch}/{name}': (f'original {arch} {name}'.encode(), 0o755) for arch in ('arm64', 'x64') for name in ('pty.node', 'spawn-helper')},
            'prebuilds/win32-arm64/conpty.node': (b'old native contract', 0o644),
        }

    def build(self):
        candidate = self.root / 'candidate.tgz'
        original = self.root / 'original.tgz'
        archive(candidate, self.candidate)
        archive(original, self.original)
        output = self.root / 'result.tgz'
        pack.assemble(candidate, original, output, '1.1.1-test.123.1.gabcdef12', {'sourceCommit': 'a' * 40})
        return output

    def test_output_preserves_macos_bytes_modes_and_new_windows_only(self):
        output = self.build()
        entries = pack.read_archive(output)
        package = json.loads(entries['package.json'][0])
        self.assertEqual(package['name'], '@accomplish-ai/node-pty')
        self.assertEqual(package['version'], '1.1.1-test.123.1.gabcdef12')
        self.assertEqual(entries['prebuilds/darwin-arm64/spawn-helper'], self.original['prebuilds/darwin-arm64/spawn-helper'])
        self.assertEqual(entries['prebuilds/win32-x64/conpty.node'][0], b'new conpty.node')
        self.assertFalse(any('win32-arm64' in name for name in entries))
        pack.verify(output)

    def test_receipt_is_in_archive_and_declared_file_selection(self):
        entries = pack.read_archive(self.build())
        package = json.loads(entries['package.json'][0])
        self.assertIn(pack.PROVENANCE, entries)
        self.assertEqual(package['files'], ['lib/', 'src/', 'prebuilds/', pack.PROVENANCE])
        self.assertEqual(package['files'].count(pack.PROVENANCE), 1)

    def test_numeric_sha_prefix_is_valid_non_numeric_semver_identifier(self):
        pack.validate_version('1.1.1-test.123.1.g01234567')
        with self.assertRaisesRegex(ValueError, 'test prerelease'):
            pack.validate_version('1.1.1-test.123.1.01234567')

    def test_rejects_stale_build_payload(self):
        self.candidate['build/Release/conpty.node'] = (b'stale', 0o644)
        with self.assertRaisesRegex(ValueError, 'build/'):
            self.build()

    def test_repairs_upstream_macos_helper_mode_without_changing_bytes(self):
        name = 'prebuilds/darwin-x64/spawn-helper'
        self.original[name] = (b'original executable', 0o644)
        entries = pack.read_archive(self.build())
        self.assertEqual(entries[name], (b'original executable', 0o755))

    def test_identical_inputs_produce_identical_archive(self):
        first = self.build().read_bytes()
        self.assertEqual(self.build().read_bytes(), first)

    def test_accepts_checkout_line_ending_conversion(self):
        self.candidate['src/unix/pty.cc'] = (b'line one\r\nline two\r\n', 0o644)
        self.original['src/unix/pty.cc'] = (b'line one\nline two\n', 0o644)
        self.build()

    def test_rejects_missing_native_helper(self):
        del self.candidate['prebuilds/win32-x64/conpty_console_list.node']
        with self.assertRaisesRegex(ValueError, 'conpty_console_list'):
            self.build()

    def test_rejects_changed_unix_native_source(self):
        self.candidate['src/unix/pty.cc'] = (b'changed', 0o644)
        with self.assertRaisesRegex(ValueError, 'Unix'):
            self.build()

    def test_rejects_stable_version(self):
        with self.assertRaisesRegex(ValueError, 'test prerelease'):
            pack.validate_version('1.1.1')

    def test_rejects_tampering_after_assembly(self):
        output = self.build()
        entries = pack.read_archive(output)
        entries['prebuilds/win32-x64/conpty.node'] = (b'tampered', 0o644)
        archive(output, entries)
        with self.assertRaisesRegex(ValueError, 'checksum'):
            pack.verify(output)

    def test_rejects_symlink_and_traversal(self):
        for name, kind in [('package/../outside', tarfile.REGTYPE), ('package/lib/link', tarfile.SYMTYPE)]:
            output = self.root / 'unsafe.tgz'
            with tarfile.open(output, 'w:gz') as target:
                item = tarfile.TarInfo(name)
                item.type = kind
                item.linkname = '/outside'
                target.addfile(item)
            with self.assertRaises(ValueError):
                pack.read_archive(output)

    def test_rejects_unqualified_or_wrong_source_run(self):
        run = {'head_sha': 'a' * 40, 'head_branch': 'codex/windows-conpty-cleanup', 'event': 'push', 'status': 'completed', 'conclusion': 'success', 'path': '.github/workflows/windows-cleanup-candidate.yml', 'head_repository': {'full_name': 'accomplish-ai/node-pty'}}
        jobs = [{'name': f'Windows native cleanup (Node {major})', 'conclusion': 'success'} for major in (20, 22)]
        pack.validate_run(run, jobs, 'a' * 40)
        for key, value in [('head_sha', 'b' * 40), ('conclusion', 'failure'), ('event', 'pull_request')]:
            altered = {**run, key: value}
            with self.assertRaises(ValueError):
                pack.validate_run(altered, jobs, 'a' * 40)
        with self.assertRaisesRegex(ValueError, 'Node 20'):
            pack.validate_run(run, jobs[1:], 'a' * 40)


if __name__ == '__main__':
    unittest.main()
