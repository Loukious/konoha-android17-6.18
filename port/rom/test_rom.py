#!/usr/bin/env python3
"""Failure-path tests for the Android source-build boundary."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent

def module(name):
    spec = importlib.util.spec_from_file_location('rom_' + name, HERE / (name + '.py'))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value

build, headers = module('build'), module('headers')

class Boundaries(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.output = self.root / 'output'
        self.output.mkdir()
        (self.output / 'previous').write_text('preserve')

    def test_nested_repo_init_uses_an_independent_client(self):
        import sys
        spec = importlib.util.spec_from_file_location('prepare', HERE.parent / 'prepare.py')
        prepare = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(prepare)
        outer = self.root / 'rom/.repo/repo'
        outer.mkdir(parents=True)
        (outer / 'main.py').write_text('# existing repo client')
        workspace = self.root / 'rom/out/kernel'
        def run(command, cwd):
            self.assertEqual(cwd, workspace)
            self.assertTrue((workspace / '.repo/repo/main.py').is_file())
            self.assertEqual(command[:2], ['repo', 'init'])
        with patch.object(sys, 'argv', ['prepare.py', str(workspace), '--init-only']), \
             patch.object(prepare, 'run', side_effect=run):
            prepare.main()
        self.assertEqual((outer / 'main.py').read_text(), '# existing repo client')
        self.assertFalse((self.root / 'rom/.repo/local_manifests').exists())

    def test_reject_source_or_parent_output(self):
        for output in [self.source, self.source / 'out', self.root]:
            with self.subTest(output=output), self.assertRaises(ValueError):
                build.outside_source(self.source, output)
            with self.assertRaises(ValueError):
                headers.generate(self.source, output)

    def test_reject_artifact_overlap_before_replacing_output(self):
        with self.assertRaises(ValueError):
            build.package(self.source, self.output / 'artifact', self.output)
        self.assertEqual((self.output / 'previous').read_text(), 'preserve')

    def test_corrupt_artifact_does_not_replace_previous_output(self):
        artifact = self.root / 'artifact'
        artifact.mkdir()
        (artifact / 'Image').write_bytes(b'corrupted')
        (artifact / 'report.json').write_text(json.dumps({
            'qdss_diagnostic': False, 'sha256': {'Image': '0' * 64}}))
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            build.package(self.source, artifact, self.output)
        self.assertEqual((self.output / 'previous').read_text(), 'preserve')

    def test_traversing_artifact_hash_is_rejected(self):
        artifact = self.root / 'artifact'
        artifact.mkdir()
        (artifact / 'report.json').write_text(json.dumps({
            'qdss_diagnostic': False, 'sha256': {'../output/previous': 'irrelevant'}}))
        with self.assertRaises(ValueError):
            build.package(self.source, artifact, self.output)

    def test_failed_compile_never_packages_an_old_artifact(self):
        workspace = self.root / 'workspace'
        old = workspace / 'recipe/artifacts/old'
        old.mkdir(parents=True)
        def run(command, **kwargs):
            if str(self.source / 'port/build.py') in command:
                raise subprocess.CalledProcessError(1, command)
        with patch.object(build, 'recipe_key', return_value='recipe'), \
             patch.object(build, 'source_state', return_value={'revision': 'fixed'}), \
             patch.object(build.shutil, 'which', return_value='/usr/bin/repo'), \
             patch.object(build.subprocess, 'run', side_effect=run), \
             patch.object(build, 'package') as package:
            with self.assertRaises(subprocess.CalledProcessError):
                build.build(self.source, self.output, workspace, 2, 4096)
            package.assert_not_called()
        self.assertTrue(old.exists())
        self.assertEqual((self.output / 'previous').read_text(), 'preserve')

    def test_success_without_new_artifact_is_rejected(self):
        with patch.object(build, 'recipe_key', return_value='recipe'), \
             patch.object(build, 'source_state', return_value={'revision': 'fixed'}), \
             patch.object(build.shutil, 'which', return_value='/usr/bin/repo'), \
             patch.object(build.subprocess, 'run'), \
             patch.object(build, 'package') as package:
            with self.assertRaisesRegex(ValueError, 'ambiguous'):
                build.build(self.source, self.output, self.root / 'workspace', 2, 4096)
            package.assert_not_called()

if __name__ == '__main__':
    unittest.main()
