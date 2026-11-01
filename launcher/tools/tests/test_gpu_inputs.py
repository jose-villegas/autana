"""GPU input decisions on disposable trees and artifact API fixtures."""
import contextlib
import importlib.util
import io
import json
import os
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
sys.path.insert(0, str(ROOT / 'scripts/ci'))
import inputs_recorded as records

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
bake.read_lock = lambda: ({(root / 'key').read_text(): {'sha256': (root / 'lock').read_text()}}
    if (root / 'lock').exists() else {})
bake.bakes = lambda paths: [NS(key=(root / 'key').read_text(), output='mesh')]
bake.submodules = lambda: {}
bake.tracked_modules = lambda: set()
bake.tracked_requirements = lambda: sorted(tools.rglob(bake.REQUIREMENTS_GLOB))
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
                 'mesh.mesh': 'not downloaded', 'lock': 'locked bytes', 'key': 'key',
                 'flags': '-O1', 'launcher/main/apps/example/app.c': 'app', 'README.md': 'docs'}
        for name, content in files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        (self.root / 'tracked.json').write_text(json.dumps(list(files)))

    def fingerprint(self, root=None):
        result = subprocess.run([sys.executable, '-c', BOOTSTRAP, str(Path(__file__).parent),
                                 str(root or self.root)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
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

    def test_rekeyed_bake_with_identical_bytes_moves_fingerprint(self):
        baseline = self.fingerprint()
        (self.root / 'key').write_text('new key')
        self.assertNotEqual(self.fingerprint(), baseline)

    def test_unlocked_bake_key_moves_fingerprint(self):
        (self.root / 'lock').unlink()
        baseline = self.fingerprint()
        (self.root / 'key').write_text('another unlocked key')
        self.assertNotEqual(self.fingerprint(), baseline)

    def test_native_quoted_header_moves_fingerprint(self):
        (self.root / 'launcher/tools/native.c').write_text('#include "only_native.h"\nint value = 1;\n')
        header = self.root / 'launcher/tools/only_native.h'
        header.write_text('#define VALUE 1\n')
        baseline = self.fingerprint()
        header.write_text('#define VALUE 2\n')
        self.assertNotEqual(self.fingerprint(), baseline)

    def test_import_module_level_and_alias_fields_move_fingerprint(self):
        entry = self.root / 'launcher/tools/worker.py'
        package = self.root / 'launcher/tools/p'
        package.mkdir()
        for name in ('__init__', 'one', 'two'):
            (package / (name + '.py')).write_text('')
        for before, after in [('import p.one\nimport p.two\nfrom p.one import f\n',
                               'import p.one\nimport p.two\nfrom p.two import f\n'),
                              ('from . import x\n', 'from .. import x\n'),
                              ('import a as x, b as y\n', 'import a as y, b as x\n')]:
            with self.subTest(before=before):
                entry.write_text(before)
                baseline = self.fingerprint()
                entry.write_text(after)
                self.assertNotEqual(self.fingerprint(), baseline)

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
        self.root = Path(directory.name).resolve()

    def host_cmake_inputs(self, root):
        files = {}
        for name in ('CMakeLists.txt', 'scene/CMakeLists.txt'):
            path = root / 'launcher/tools/render' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('target_link_options(host INTERFACE -Wl,--wrap=malloc)\n')
            files[f'host launcher/tools/render/{name}'] = stages.content_checksum(path).decode()
        return files

    def test_host_link_configuration_bytes_move_fingerprint(self):
        self.host_cmake_inputs(self.root)
        with mock.patch.object(stages, 'ROOT', self.root), mock.patch.object(
                stages.subprocess, 'run', return_value=NS(stdout='flags -O1\n')):
            baseline = bake.digest(stages.host_inputs(self.root / 'scene'))
            for name in ('CMakeLists.txt', 'scene/CMakeLists.txt'):
                path = self.root / 'launcher/tools/render' / name
                original = path.read_text()
                path.write_text(original + 'target_link_options(host INTERFACE -Wl,--wrap=free)\n')
                self.assertNotEqual(bake.digest(stages.host_inputs(self.root / 'scene')), baseline)
                path.write_text(original)

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
        for directory in ('__init__', 'one', 'two'):
            root = self.root / directory
            root.mkdir()
            self.host_cmake_inputs(root)
            (root / 'host.c').write_text('source')
            listing = f'flags -I {root}/include -O1\nhost.o: {root}/host.c\n'
            with mock.patch.object(stages, 'ROOT', root), mock.patch.object(
                    stages.subprocess, 'run', return_value=NS(stdout=listing)):
                outputs.append(stages.host_inputs(root / 'scene'))
        self.assertEqual(outputs[0], outputs[1])
        self.assertEqual(outputs[0]['host flags'], '-I include -O1')
        for listing, message in [('host.o: host.c\n', 'no flags'),
                                  (f'flags -O1\nhost.o: {self.root}/outside.c\n', 'outside'),
                                  ('flags -O1\nhost.o: ../outside.c\n', 'outside')]:
            with mock.patch.object(stages, 'ROOT', root), mock.patch.object(
                    stages.subprocess, 'run', return_value=NS(stdout=listing)):
                with self.assertRaisesRegex(ValueError, message):
                    stages.host_inputs(root / 'scene')

    def test_host_escaped_space_dependency_is_hashed_under_real_name(self):
        cmake_inputs = self.host_cmake_inputs(self.root)
        header = self.root / 'my dir/host.h'
        header.parent.mkdir()
        header.write_text('#define VALUE 1\n')
        listing = 'flags -O1\nhost.o: my\\ dir/host.h\n'
        with mock.patch.object(stages, 'ROOT', self.root), mock.patch.object(
                stages.subprocess, 'run', return_value=NS(stdout=listing)):
            self.assertEqual(stages.host_inputs(self.root / 'scene'),
                             {'host flags': '-O1', **cmake_inputs,
                              'host my dir/host.h': stages.content_checksum(header).decode()})

    def test_host_header_bytes_move_fingerprint_with_same_listing(self):
        self.host_cmake_inputs(self.root)
        header = self.root / 'host.h'
        header.write_text('#define VALUE 1\n')
        listing = 'flags -O1\nhost.o: host.h\n'
        with mock.patch.object(stages, 'ROOT', self.root), mock.patch.object(
                stages.subprocess, 'run', return_value=NS(stdout=listing)):
            baseline = bake.digest(stages.host_inputs(self.root / 'scene'))
            header.write_text('#define VALUE 2\n')
            self.assertNotEqual(bake.digest(stages.host_inputs(self.root / 'scene')), baseline)

    @unittest.skipUnless(sys.platform == 'linux' and shutil.which('cc'), 'Linux shell required')
    def test_scene_deps_flags_include_declared_defines_and_includes(self):
        script = self.root / 'scene.sh'
        script.write_text('scene_name=fixture\nscene_sources=tools/render/scene_viewer.c\n'
                          'scene_renders=fixture\nscene_defines=-DFIXTURE_SCENE=1\n'
                          'scene_includes=tools/render\n'
                          f'. "{ROOT}/launcher/tools/render/render_scene.sh"\n'
                          'render_scene_run --deps\n')
        result = subprocess.run(['sh', '-c', script.read_text(),
                                 str(ROOT / 'launcher/tools/render/scenes/fixture.sh')], cwd=ROOT, capture_output=True, text=True, check=True)
        flags = result.stdout.splitlines()[0]
        self.assertIn('-DFIXTURE_SCENE=1', flags)
        self.assertIn(f'-I {ROOT}/launcher/tools/render', flags)

    @unittest.skipUnless(sys.platform == 'linux', 'Linux shell required')
    def test_gpu_environment_and_python_from_other_directory(self):
        env = self.root / 'environment'
        (env / 'bin').mkdir(parents=True)
        python = env / 'bin/python3'
        python.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
        python.chmod(0o755)
        script = ROOT / 'launcher/tools/r3d/gpu_env.sh'
        variables = ('CC', 'CXX', 'CPATH', 'LIBRARY_PATH', 'CUDA_HOME', 'PATH', 'GPU_ENV')
        command = f'GPU_ENV="{env}"; . "{script}"; env'
        environment = {**os.environ, 'GPU_ENV': str(env)}
        result = subprocess.run(['sh', '-c', command], cwd=self.root, env={key: value for key, value in os.environ.items() if key != 'GPU_ENV'},
                                capture_output=True, text=True, check=True)
        exports = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
        expected = (str(env / 'bin/x86_64-conda-linux-gnu-gcc'),
                    str(env / 'bin/x86_64-conda-linux-gnu-g++'), str(env / 'targets/x86_64-linux/include'),
                    f'{env}/targets/x86_64-linux/lib:{env}/lib', str(env),
                    f'{env}/bin:{os.environ["PATH"]}', str(env))
        for variable, value in zip(variables, expected):
            self.assertEqual(exports.get(variable), value)
        result = subprocess.run(['sh', str(ROOT / 'launcher/tools/r3d/gpu_python.sh'), 'a b', 'c'],
                                cwd=self.root, env=environment, capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, 'a b\nc\n')

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
        self.assertFalse(records.current([old, newest, failure, *ignored], 'A'))
        self.assertFalse(records.current([failure], 'A', force=True))
        self.assertFalse(records.current([], 'A'))


    def test_failed_record_order_expiry_and_branch(self):
        failed = dict(name=records.FAILED_PREFIX + 'F', created_at='2026-01-02',
                      expired=False, workflow_run={'head_branch': 'main'})
        rendered = dict(failed, name=records.RENDERED_PREFIX + 'F', created_at='2026-01-01')
        newer_rendered = dict(rendered, created_at='2026-01-03')
        cases = [([failed], False), ([rendered, failed], False), ([failed, rendered], False),
                 ([failed, newer_rendered], True), ([newer_rendered, failed], True),
                 ([rendered, dict(failed, expired=True)], True),
                 ([rendered, dict(failed, workflow_run={'head_branch': 'feature/test'})], True)]
        for artifacts, expected in cases:
            with self.subTest(artifacts=artifacts), mock.patch.dict(os.environ, FORCE=''), \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(records.current(artifacts, 'F'), expected)
                records.main(['F'], request=mock.Mock(return_value={'artifacts': artifacts}))
            self.assertEqual(output.getvalue(), str(expected).lower() + '\n')

    def test_exact_name_and_empty_fingerprint(self):
        record = dict(name=records.RENDERED_PREFIX + 'longABC', created_at='2026',
                      expired=False, workflow_run={'head_branch': 'main'})
        self.assertFalse(records.current([record], 'ABC'))
        self.assertFalse(records.current([record], ''))
        record['name'] = records.RENDERED_PREFIX
        self.assertFalse(records.current([record], ''))

    def test_main_force_and_paged_api(self):
        record = dict(name=records.RENDERED_PREFIX + 'ABC', created_at='2026',
                      expired=False, workflow_run={'head_branch': 'main'})
        for force, expected in [('true', 'false'), ('', 'true'), ('false', 'true')]:
            with self.subTest(force=force), mock.patch.dict(os.environ, FORCE=force), \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                calls = []
                def request(method, path, body=None):
                    calls.append((method, path))
                    return {'artifacts': [record] if path.endswith('&page=1') else []}
                records.main(['ABC'], request=request)
            self.assertEqual(output.getvalue(), expected + '\n')
            self.assertEqual(calls, [('GET', '/actions/artifacts?per_page=100&page=1')])

    def test_hosted_lookup_defers_to_gate_when_other_run_in_flight(self):
        record = dict(name=records.RENDERED_PREFIX + 'X', created_at='2026',
                      expired=False, workflow_run={'head_branch': 'main'})
        for status in ('queued', 'in_progress', 'pending', 'requested', 'waiting', 'completed'):
            for run_id in (10, 11):
                with self.subTest(status=status, run_id=run_id), mock.patch.dict(
                        os.environ, FORCE='', GITHUB_RUN_ID='10',
                        GITHUB_WORKFLOW_REF='owner/repo/.github/workflows/own.yml@refs/heads/main'), \
                        contextlib.redirect_stdout(io.StringIO()) as output:
                    def request(method, path, body=None):
                        key = 'artifacts' if '/artifacts?' in path else 'workflow_runs'
                        items = [record] if key == 'artifacts' else [{'id': run_id, 'status': status}]
                        return {key: items if path.endswith('&page=1') else []}
                    records.main(['X', '--defer-in-flight'], request=request)
                expected = run_id == 10 or status == 'completed'
                self.assertEqual(output.getvalue(), str(expected).lower() + '\n')

    def test_workflow_records_and_every_post_gate_step_are_guarded(self):
        import yaml
        workflow = yaml.load((ROOT / '.github/workflows/doc-images-gpu.yml').read_text(), Loader=yaml.BaseLoader)
        steps = workflow['jobs']['refresh']['steps']
        gate = next(index for index, step in enumerate(steps) if step.get('id') == 'gate')
        self.assertEqual(gate, 1)
        stage = next(index for index, step in enumerate(steps) if step.get('id') == 'gpu')
        names = []
        for index, step in enumerate(steps[gate + 1:], gate + 1):
            # Past the stage only what it rendered is recorded: it may skip after the lock wait.
            guard = ("steps.gate.outputs.render == 'true'" if index <= stage
                     else "steps.gpu.outputs.rendered == 'true'")
            if index > stage and step.get('if', '').startswith('always()'):
                guard = "steps.gate.outputs.render == 'true'"
            self.assertIn(guard, step.get('if', ''))
            if step.get('uses', '').startswith('actions/upload-artifact@'):
                names.append(step['with']['name'])
        self.assertCountEqual(names, [prefix + '${{ needs.fingerprint.outputs.fingerprint }}'
                                      for prefix in records.RECORD_PREFIXES])
        hosted = workflow['jobs']['fingerprint']['steps'][-1]['run']
        self.assertIn('--defer-in-flight', hosted)
        self.assertEqual(steps[gate]['run'].strip(), 'sh scripts/ci/gpu_refresh_gate.sh')
        self.assertEqual(steps[gate]['env']['FINGERPRINT'], '${{ needs.fingerprint.outputs.fingerprint }}')
        self.assertEqual(workflow['jobs']['fingerprint']['outputs']['current'], '${{ steps.inputs.outputs.current }}')
        refresh = workflow['jobs']['refresh']
        self.assertEqual(refresh['needs'], ['fingerprint', 'marker', 'wait'])
        # A push only marks images pending; the render waits for NIGHT_START.
        self.assertEqual(refresh['if'], "github.event_name != 'push' && needs.fingerprint.outputs.current != 'true'")
        self.assertEqual(refresh['concurrency'], {'group': 'doc-images-gpu', 'cancel-in-progress': 'false', 'queue': 'max'})
        self.assertEqual(workflow['env']['FORCE'], '${{ inputs.force }}')
        self.assertNotIn('. launcher/tools/r3d/gpu_env.sh', '\n'.join(step.get('run', '') for step in steps))

    def test_hosted_interleaving_and_nonmatching_record(self):
        for matching in (True, False):
            with self.subTest(matching=matching), mock.patch.dict(
                    os.environ, FORCE='', GITHUB_RUN_ID='10',
                    GITHUB_WORKFLOW_REF='owner/repo/.github/workflows/own.yml@refs/heads/main'), \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                calls = []
                def request(method, path, body=None):
                    calls.append(path)
                    if '/runs?' in path:
                        return {'workflow_runs': [{'id': 11, 'status': 'completed'},
                                                  {'id': 10, 'status': 'pending'}]}
                    # A record read first sees X before the other run finishes and records B.
                    fingerprint = 'X' if matching and len(calls) == 1 else 'B'
                    return {'artifacts': [dict(name=records.RENDERED_PREFIX + fingerprint,
                        created_at='2026', expired=False, workflow_run={'head_branch': 'main'})]}
                records.main(['X', '--defer-in-flight'], request=request)
            self.assertEqual(output.getvalue(), 'false\n')
            self.assertEqual(calls[0], '/actions/workflows/own.yml/runs?branch=main&per_page=100&page=1')
            self.assertEqual(calls[1], '/actions/artifacts?per_page=100&page=1')

    def test_workflow_ref_is_required(self):
        with mock.patch.dict(os.environ, GITHUB_RUN_ID='10'):
            os.environ.pop('GITHUB_WORKFLOW_REF', None)
            request = mock.Mock()
            with self.assertRaises(KeyError):
                records.main(['X', '--defer-in-flight'], request=request)
            request.assert_not_called()

    def test_trigger_sha_is_required_without_stdout_verdict(self):
        with mock.patch.dict(os.environ), contextlib.redirect_stdout(io.StringIO()) as output:
            os.environ.pop('GITHUB_SHA', None)
            request = mock.Mock(side_effect=[{'object': {'sha': 'X'}}, {'artifacts': []}])
            with self.assertRaisesRegex(KeyError, 'GITHUB_SHA'):
                records.main(['X', '--skip-obsolete'], request=request)
        self.assertEqual(output.getvalue(), '')
        request.assert_called_once_with('GET', '/git/ref/heads/main')

    def test_delayed_push_gate_skips_after_completed_revert(self):
        record = dict(name=records.RENDERED_PREFIX + 'X', created_at='2026',
                      expired=False, workflow_run={'head_branch': 'main'})
        with mock.patch.dict(os.environ, FORCE='', GITHUB_SHA='X'), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            request = mock.Mock(side_effect=[{'object': {'sha': 'X'}}, {'artifacts': [record]}])
            records.main(['X', '--skip-obsolete'], request=request)
        self.assertEqual(output.getvalue(), 'true\n')
        with mock.patch.dict(os.environ, FORCE='true', GITHUB_SHA='B'), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            request = mock.Mock(return_value={'object': {'sha': 'X'}})
            records.main(['B', '--skip-obsolete'], request=request)
        self.assertEqual(output.getvalue(), 'obsolete: main moved to X; dispatch again on main to force\n')
        request.assert_called_once_with('GET', '/git/ref/heads/main')

    def test_in_flight_middle_run_on_each_page(self):
        from github_api import PER_PAGE
        for page in (1, 2):
            for unfinished in (True, False):
                with self.subTest(page=page, unfinished=unfinished), mock.patch.dict(
                        os.environ, FORCE='', GITHUB_RUN_ID='10',
                        GITHUB_WORKFLOW_REF='owner/repo/.github/workflows/own.yml@refs/heads/main'), \
                        contextlib.redirect_stdout(io.StringIO()) as output:
                    runs = [{'id': 100 + index, 'status': 'completed'} for index in range(PER_PAGE)]
                    tail = [{'id': 1000 + index, 'status': 'completed'} for index in range(3)]
                    if unfinished:
                        (runs if page == 1 else tail)[len(runs) // 2 if page == 1 else 1]['status'] = 'pending'
                    record = dict(name=records.RENDERED_PREFIX + 'X', created_at='2026',
                                  expired=False, workflow_run={'head_branch': 'main'})
                    def request(method, path, body=None):
                        if '/artifacts?' in path:
                            return {'artifacts': [record]}
                        return {'workflow_runs': runs if path.endswith('page=1') else tail}
                    records.main(['X', '--defer-in-flight'], request=request)
                self.assertEqual(output.getvalue(), str(not unfinished).lower() + '\n')
