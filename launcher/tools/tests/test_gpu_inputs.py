"""GPU input decisions on disposable trees and artifact API fixtures."""
import contextlib
import importlib.util
import io
import json
import runpy
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace as NS
import unittest
from unittest import mock

from test_doc_stages import stages, PATH
from bake import bake

ROOT = PATH.parents[3]

BOOTSTRAP = r"""
import json, sys
from pathlib import Path
from unittest import mock
from types import SimpleNamespace as NS
sys.path.insert(0, sys.argv[1])
from test_doc_stages import stages
from bake import bake
from r3d import build_pack
root = Path(sys.argv[2])
tools = root / 'launcher/tools'
stages.ROOT = bake.REPO = root
stages.TOOLS = bake.TOOLS = tools
stages.__file__ = str(tools / 'entry.py')
stages.tracked_files = lambda *args: json.loads((root / 'tracked.json').read_text())
stages.host_inputs = lambda scene: {'host flags': (root / 'flags').read_text(),
    'host host.c': stages.content_checksum(root / 'host.c').decode()}
bake.read_lock = lambda: {'key': {'sha256': (root / 'lock').read_text()}}
bake.bakes = lambda paths: [NS(key='key', output='mesh')]
bake.submodules = lambda: {}
bake.packages.include_roots = lambda: []
build_pack.pack_jobs = lambda paths: ({'scene': {'clip': root / 'clip.anim.toml',
    'mesh': root / 'mesh.mesh'}}, {root / 'mesh.mesh': None})
print(bake.digest(stages.gpu_inputs(root / 'scene.toml', 'object')))
"""


class GpuFingerprintTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        files = {'launcher/tools/entry.py': 'from pathlib import Path\nimport a\n'
                 'NATIVE = Path(__file__).parent / "native.c"\n'
                 'DATA = "data.txt"\nSCRIPT = "launcher/tools/worker.py"\nVALUE = 1\n',
                 'launcher/tools/a.py': '', 'launcher/tools/b.py': '',
                 'launcher/tools/worker.py': 'import helper\nimport a\nimport b\n',
                 'launcher/tools/helper.py': 'VALUE = 2\n',
                 'launcher/tools/native.c': 'int value = 1;\n',
                 'launcher/tools/requirements.txt': 'package==1\n',
                 'data.txt': 'weights', 'host.c': 'int host = 1;\n',
                 'scene.toml': 'scene', 'clip.anim.toml': 'clip',
                 'mesh.mesh': 'not downloaded', 'lock': 'locked bytes',
                 'flags': '-O1', 'launcher/main/apps/example/app.c': 'app', 'README.md': 'docs'}
        for name, content in files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        (self.root / 'tracked.json').write_text(json.dumps(list(files)))

    def fingerprint(self, root=None):
        result = subprocess.run([sys.executable, '-c', BOOTSTRAP, str(Path(__file__).parent),
                                 str(root or self.root)], capture_output=True, text=True, check=True)
        return result.stdout.strip()

    def test_comment_blank_token_and_import_binding_decisions(self):
        baseline = self.fingerprint()
        self.assertEqual(self.fingerprint(), baseline)
        entry = self.root / 'launcher/tools/entry.py'
        original = entry.read_text()
        entry.write_text(original + '\n# constraint\n')
        self.assertEqual(self.fingerprint(), baseline)
        entry.write_text(original.replace('VALUE = 1', 'VALUE = 3'))
        self.assertNotEqual(self.fingerprint(), baseline)
        entry.write_text(original.replace('import a', 'import b as a'))
        self.assertNotEqual(self.fingerprint(), baseline)

    def test_each_input_kind_changes_the_decision(self):
        baseline = self.fingerprint()
        for name in ('host.c', 'flags', 'scene.toml', 'data.txt', 'lock',
                     'launcher/tools/requirements.txt', 'launcher/tools/native.c', 'clip.anim.toml'):
            with self.subTest(input=name):
                path = self.root / name
                original = path.read_text()
                path.write_text(original.replace('1', '2') if '1' in original else original + ' changed')
                self.assertNotEqual(self.fingerprint(), baseline)
                path.write_text(original)

    def test_app_and_readme_do_not_change_the_decision(self):
        baseline = self.fingerprint()
        for name in ('launcher/main/apps/example/app.c', 'README.md'):
            (self.root / name).write_text('changed')
            self.assertEqual(self.fingerprint(), baseline)

    def test_string_named_scripts_imports_join_the_closure(self):
        baseline = self.fingerprint()
        (self.root / 'launcher/tools/helper.py').write_text('VALUE = 3\n')
        self.assertNotEqual(self.fingerprint(), baseline)

    def test_fingerprint_is_checkout_independent(self):
        other = self.root / 'checkout'
        shutil.copytree(self.root, other, ignore=shutil.ignore_patterns('checkout'))
        self.assertEqual(self.fingerprint(), self.fingerprint(other))


class InputContractTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def test_named_files_all_bases_tracked_only_and_single_line(self):
        tools = self.root / 'launcher/tools'
        own = tools / 'module'
        own.mkdir(parents=True)
        paths = [self.root / 'root.txt', tools / 'tools.txt', own / 'own.txt']
        untracked = own / 'untracked.txt'
        for path in [*paths, untracked]:
            path.touch()
        entry = own / 'entry.py'
        entry.write_text('A="root.txt"\nB="tools.txt"\nC="own.txt"\n'
                         'D="untracked.txt"\nE="""own.txt\n"""\n')
        with mock.patch.object(stages, 'ROOT', self.root), mock.patch.object(stages, 'TOOLS', tools):
            self.assertEqual(list(stages.named_files(entry, set(paths))), paths)

    @unittest.skipUnless(sys.platform == 'linux', 'POSIX compiler listing required')
    def test_host_flags_and_deps_relative_and_invalid_listing_refused(self):
        outputs = []
        for directory in ('one', 'two'):
            root = self.root / directory
            root.mkdir()
            (root / 'host.c').write_text('source')
            listing = f'flags -I {root}/include -O1\nhost.o: {root}/host.c\n'
            with mock.patch.object(stages, 'ROOT', root), mock.patch.object(
                    stages.subprocess, 'run', return_value=NS(stdout=listing)):
                outputs.append(stages.host_inputs(root / 'scene'))
        self.assertEqual(outputs[0], outputs[1])
        self.assertEqual(outputs[0]['host flags'], '-I include -O1')
        for listing, message in [('host.o: host.c\n', 'no flags'),
                                  (f'flags -O1\nhost.o: {self.root}/outside.c\n', 'outside')]:
            with mock.patch.object(stages, 'ROOT', root), mock.patch.object(
                    stages.subprocess, 'run', return_value=NS(stdout=listing)):
                with self.assertRaisesRegex(ValueError, message):
                    stages.host_inputs(root / 'scene')

    def test_check_gpu_matches_current_source(self):
        args = NS(scene=Path('scene'), object='object')
        manifest = self.root / 'source.json'
        with mock.patch.object(stages, 'ROOT', self.root), mock.patch.object(
                stages, 'gpu_inputs', return_value={'input': 'current'}), mock.patch.object(
                stages, 'apply_tables', return_value=False):
            manifest.write_text(json.dumps({'source': 'stale', 'files': {}}))
            with self.assertRaisesRegex(ValueError, 'source changed'):
                stages.check_gpu(args, self.root)
            manifest.write_text(json.dumps({'source': bake.digest({'input': 'current'}), 'files': {}}))
            self.assertEqual(stages.check_gpu(args, self.root), 0)

    def test_inputs_print_only_digest_and_preserve_out(self):
        out = self.root / 'out/gpu'
        out.mkdir(parents=True)
        sentinel = out / 'existing'
        sentinel.write_bytes(b'preserve')
        with mock.patch.object(stages, 'RESULTS', self.root), mock.patch.object(
                stages, 'gpu_inputs', return_value={'input': 'current'}), contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(stages.main(['--stage', 'gpu', '--inputs', '--scene', 'scene', '--object', 'object']), 0)
        self.assertEqual(output.getvalue(), bake.digest({'input': 'current'}) + '\n')
        self.assertEqual(sentinel.read_bytes(), b'preserve')
        self.assertEqual(list(self.root.iterdir()), [self.root / 'out'])

    def test_inputs_require_python_312(self):
        with mock.patch.object(stages.sys, 'version_info', (3, 11)), contextlib.redirect_stderr(io.StringIO()) as error:
            self.assertEqual(stages.main(['--stage', 'gpu', '--inputs', '--scene', 'scene', '--object', 'object']), 2)
        self.assertIn('Python 3.12', error.getvalue())

    def test_old_python_exits_before_importing_tools(self):
        with mock.patch.object(sys, 'version_info', (3, 10)), mock.patch.object(
                sys, 'argv', [str(PATH), '--inputs']), mock.patch.dict(sys.modules, tomllib=None):
            with self.assertRaisesRegex(SystemExit, 'Python 3.12'):
                runpy.run_path(str(PATH), run_name='__main__')

    @unittest.skipUnless(sys.platform == 'linux' and shutil.which('cc'), 'Linux host compiler required')
    def test_render_scene_deps_flags_first_no_binary_or_packs(self):
        result = subprocess.run(['sh', str(ROOT / 'launcher/tools/render/scenes/scene_viewer_render_host.sh'),
                                 '--deps', '--asset-file', str(ROOT / 'launcher/demo/sponza/sponza.scene.toml'),
                                 '-o', str(self.root)], capture_output=True, text=True, check=True)
        self.assertTrue(result.stdout.startswith('flags '), result.stdout)
        self.assertIn('.c', result.stdout.split('\n', 1)[1])
        self.assertEqual(list(self.root.iterdir()), [])


class RecordedInputsTests(unittest.TestCase):
    def test_latest_main_record_decides_including_failures_and_force(self):
        spec = importlib.util.spec_from_file_location('inputs_recorded', ROOT / 'scripts/ci/inputs_recorded.py')
        records = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(records)
        def artifact(prefix, fingerprint, date, branch='main', expired=False):
            return dict(name=prefix + fingerprint, created_at=date, expired=expired,
                        workflow_run={'head_branch': branch})
        rendered, failed = 'doc-images-inputs-', 'doc-images-failed-'
        old = artifact(rendered, 'A', '2026-01-01T00:00:00Z')
        newest = artifact(rendered, 'B', '2026-01-02T00:00:00Z')
        ignored = [artifact(rendered, 'A', '2026-01-04T00:00:00Z', branch='feature/test'),
                   artifact(rendered, 'A', '2026-01-05T00:00:00Z', expired=True),
                   artifact('unrelated-', 'A', '2026-01-06T00:00:00Z')]
        self.assertFalse(records.current([old, newest, *ignored], 'A'))
        self.assertTrue(records.current([newest, old, *ignored], 'B'))
        failure = artifact(failed, 'A', '2026-01-03T00:00:00Z')
        self.assertTrue(records.current([old, newest, failure, *ignored], 'A'))
        self.assertFalse(records.current([failure], 'A', force=True))
        self.assertFalse(records.current([], 'A'))

        pages = json.dumps([{'artifacts': [old]}, {'artifacts': [newest, *ignored]}])
        with mock.patch.object(records.subprocess, 'run', return_value=NS(stdout=pages)) as run, \
                mock.patch.dict(records.os.environ, GITHUB_REPOSITORY='owner/repo', FORCE=''), \
                mock.patch.object(sys, 'argv', ['inputs_recorded.py', 'A']), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            records.main()
        self.assertEqual(output.getvalue(), 'false\n')
        self.assertEqual(run.call_args.args[0], ['gh', 'api', '--paginate', '--slurp',
                                               'repos/owner/repo/actions/artifacts'])
