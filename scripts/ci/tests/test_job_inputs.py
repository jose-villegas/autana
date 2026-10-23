"""Job gates match recorded reads and fail open without usable evidence."""
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import job_inputs
import inputs_recorded


class JobInputsTests(unittest.TestCase):
    def test_added_c_under_launcher_runs_firmware(self):
        manifest = {'files': ['launcher/main/core.c'], 'directories': []}
        for status in ('added', 'renamed'):
            with self.subTest(status=status):
                change = {'filename': 'launcher/new/deep/source.c', 'status': status}
                self.assertEqual(job_inputs.matching_paths([change], manifest,
                    {'launcher/main/core.c'}), [change['filename']])
        change = {'filename': 'launcher/main/new.c', 'status': 'removed'}
        self.assertEqual(job_inputs.matching_paths([change], manifest,
            {'launcher/main/core.c', change['filename']}), [])

    def test_added_docs_gif_skips_firmware(self):
        manifest = {'files': ['launcher/main/core.c', 'docs/README.md'], 'directories': []}
        change = {'filename': 'docs/new/images/x.gif', 'status': 'added'}
        self.assertEqual(job_inputs.matching_paths([change], manifest,
            set(manifest['files'])), [])

    def test_directory_matching_uses_nearest_base_ancestor(self):
        base_files = {'docs/x/existing.md'}
        for status in ('added', 'removed', 'renamed'):
            for name in ('docs/x/new.gif', 'docs/x/new/sub/new.gif'):
                for directories, expected in [(['.', 'docs'], []),
                        (['.', 'docs', 'docs/x'], [name])]:
                    with self.subTest(status=status, name=name, directories=directories):
                        change = {'filename': name, 'status': status}
                        self.assertEqual(job_inputs.matching_paths([change],
                            {'files': [], 'directories': directories}, base_files), expected)
        change = {'filename': 'elsewhere/new.gif', 'previous_filename': 'docs/x/old.gif',
                  'status': 'renamed'}
        self.assertEqual(job_inputs.matching_paths([change],
            {'files': [], 'directories': ['docs/x']}, base_files), ['docs/x/old.gif'])

    def test_tracked_agents_read_establishes_extension_coverage(self):
        change = {'filename': 'NEW.md', 'status': 'added'}
        self.assertEqual(job_inputs.matching_paths([change],
            {'files': ['AGENTS.md'], 'directories': []}, {'AGENTS.md'}), ['NEW.md'])

    def test_added_tool_shell_script_runs_only_shell_lint(self):
        source = 'launcher/main/apps/render_lab/scene.c'
        script = 'launcher/main/apps/render_lab/tools/existing.sh'
        change = {'filename': 'launcher/main/apps/render_lab/tools/y.sh', 'status': 'added'}
        for reads, expected in [([source], []), ([source, script], [change['filename']])]:
            with self.subTest(reads=reads):
                self.assertEqual(job_inputs.matching_paths([change],
                    {'files': reads, 'directories': []}, {source, script}), expected)

    def test_added_root_markdown_runs_only_math_mermaid(self):
        source = 'launcher/main/core.c'
        markdown = 'docs/math/README.md'
        change = {'filename': 'NEW.md', 'status': 'added'}
        for reads, expected in [([source], []), ([markdown], [change['filename']])]:
            with self.subTest(reads=reads):
                self.assertEqual(job_inputs.matching_paths([change],
                    {'files': reads, 'directories': []},
                    {source, markdown}), expected)
        change = {'filename': 'NOTICE', 'status': 'added'}
        self.assertEqual(job_inputs.matching_paths([change],
            {'files': ['docs/LICENSE'], 'directories': []}, {'docs/LICENSE'}), ['NOTICE'])

    def test_path_matching(self):
        manifest = {'files': ['src/read.c', 'old/file.c'], 'directories': ['src/new']}
        cases = [({'filename': 'src/read.c', 'status': 'modified'}, ['src/read.c']),
                 ({'filename': 'src/other.c', 'status': 'modified'}, []),
                 ({'filename': 'src/new/file.c', 'status': 'added'}, ['src/new/file.c']),
                 ({'filename': 'src/new/file.c', 'status': 'modified'}, []),
                 ({'filename': 'src/new/file.c', 'status': 'removed'}, ['src/new/file.c']),
                 ({'filename': 'new/file.c', 'previous_filename': 'old/file.c',
                   'status': 'renamed'}, ['old/file.c']),
                 ({'filename': 'src/new/sub/file.c', 'status': 'added'},
                  ['src/new/sub/file.c'])]
        for change, expected in cases:
            with self.subTest(change=change):
                self.assertEqual(job_inputs.matching_paths([change], manifest,
                    {'src/new/existing.h'}), expected)

    def test_latest_artifact_uses_base_branch_and_excludes_pr_runs(self):
        artifacts = [dict(id=number, name='job-inputs-ci-build', expired=expired,
                          created_at=str(number), workflow_run={'id': number, 'head_branch': branch})
                     for number, expired, branch in [(1, False, 'feature/base'),
                         (2, True, 'feature/base'), (3, False, 'main'), (4, False, 'feature/base')]]
        request = mock.Mock(side_effect=[{'event': 'pull_request'}, {'event': 'workflow_dispatch'}])
        self.assertEqual(inputs_recorded.latest_job_artifact(
            artifacts, 'job-inputs-ci-build', 'feature/base', request)['id'], 1)
        self.assertEqual(request.call_count, 2)

    def test_affected_missing_manifest_and_api_cap(self):
        for count in (1, job_inputs.PR_FILES_LIMIT):
            with self.subTest(count=count), mock.patch('sys.stdout', new_callable=io.StringIO) as log:
                request = mock.Mock()
                with mock.patch.object(job_inputs, 'pages', return_value=iter([
                        {'filename': 'other', 'status': 'modified'}] * count)), \
                        mock.patch.object(job_inputs, 'latest_job_artifact', return_value=None):
                    self.assertEqual(job_inputs.affected('ci', ['build'], 7, 'main', request),
                                     {'build': 'true'})
                self.assertIn('build', log.getvalue())
                self.assertIn('3000' if count == job_inputs.PR_FILES_LIMIT else '::notice::', log.getvalue())

    def test_affected_reads_zip_and_reports_matches(self):
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, 'w') as output:
            output.writestr(job_inputs.MANIFEST_NAME, json.dumps(
                {'files': ['src/read.c'], 'directories': []}))
        request = mock.Mock(return_value=archive.getvalue())
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(
                os.environ, GITHUB_STEP_SUMMARY=str(Path(directory) / 'summary')), \
                mock.patch.object(job_inputs, 'pages', side_effect=[
                    iter([{'filename': 'src/new/file.c', 'status': 'added'}]), iter([])]), \
                mock.patch.object(job_inputs, 'tracked_paths', return_value=(
                    {'src/read.c', 'src/new/file.c'}, {'src', 'src/new'})), \
                mock.patch.object(job_inputs, 'latest_job_artifact', return_value={'id': 5}):
            self.assertEqual(job_inputs.affected('ci', ['build'], 7, 'main', request), {'build': 'true'})
            self.assertIn('src/new/file.c', (Path(directory) / 'summary').read_text())
            request.assert_called_once_with('GET', '/actions/artifacts/5/zip', raw=True)

    def test_affected_nonmatching_manifest_reports_false(self):
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, 'w') as output:
            output.writestr(job_inputs.MANIFEST_NAME, json.dumps(
                {'files': ['read.c'], 'directories': []}))
        with mock.patch.object(job_inputs, 'pages', side_effect=[
                iter([{'filename': 'unread.c', 'status': 'modified'}]), iter([])]), \
                mock.patch.object(job_inputs, 'latest_job_artifact', return_value={'id': 5}):
            self.assertEqual(job_inputs.affected('ci', ['build'], 7, 'main',
                mock.Mock(return_value=archive.getvalue())), {'build': 'false'})

    def test_record_rejects_broken_atime(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / job_inputs.MARKER_NAME).write_text('1')
            (root / '.git').mkdir()
            (root / job_inputs.CANARY).write_text('canary')
            os.utime(root / job_inputs.CANARY, ns=(0, 0))
            with mock.patch.object(job_inputs, 'tracked_paths', return_value=(set(), set())), \
                    mock.patch.object(Path, 'read_bytes', return_value=b'canary'):
                with self.assertRaisesRegex(RuntimeError, 'canary.*atime'):
                    job_inputs.record(root, root, 'unused', 'build')

    def test_arm_rejects_non_linux_runner(self):
        with mock.patch.dict(os.environ, RUNNER_OS='Windows'):
            with self.assertRaisesRegex(RuntimeError, 'Linux'):
                job_inputs.arm(Path('.'), Path('.'))

    @unittest.skipUnless(sys.platform == 'linux', 'symlink atime requires Linux')
    def test_collection_does_not_follow_unread_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'input').write_text('read')
            (root / 'link').symlink_to('input')
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
            with mock.patch.dict(os.environ, RUNNER_OS='Linux'):
                job_inputs.arm(root, root)
            (root / 'input').read_bytes()
            if (root / 'input').stat().st_atime_ns == 0:
                self.skipTest('filesystem does not update atime on reads')
            with mock.patch.object(job_inputs, 'workflow_inputs', return_value=set()):
                result = job_inputs.record(root, root, 'unused', 'build')
            self.assertNotIn('link', result['files'])

    @unittest.skipUnless(sys.platform == 'linux', 'git read recording requires Linux atime')
    def test_git_metadata_does_not_record_tracked_reads(self):
        for namespace in (False, True):
            with self.subTest(root_user=namespace), tempfile.TemporaryDirectory() as directory, \
                    mock.patch.dict(os.environ, RUNNER_OS='Linux'):
                prefix = ['unshare', '-Ur'] if namespace else []
                if namespace:
                    try:
                        available = subprocess.run(prefix + ['true'], capture_output=True)
                    except FileNotFoundError:
                        self.skipTest('unshare is unavailable for root-user Git check')
                    if available.returncode:
                        self.skipTest('user namespaces are unavailable for root-user Git check')
                root = Path(directory)
                git = ['git', '-C', str(root)]
                subprocess.run(git + ['init', '-q'], check=True)
                names = ('first.txt', 'second.txt')
                for name in names:
                    (root / name).write_text(name)
                subprocess.run(git + ['add', '.'], check=True)
                subprocess.run(git + ['-c', 'user.name=Test', '-c', 'user.email=test@example.org',
                                     'commit', '-qm', 'fixture'], check=True)
                # Whole-second mtimes reproduce checkout on coarse-timestamp filesystems.
                index_second = (root / '.git/index').stat().st_mtime_ns // job_inputs.NANOSECONDS_PER_SECOND
                for name in names:
                    os.utime(root / name, ns=(0, index_second * job_inputs.NANOSECONDS_PER_SECOND))
                subprocess.run(git + ['update-index', '--really-refresh'], check=True)
                timestamp = index_second * job_inputs.NANOSECONDS_PER_SECOND
                os.utime(root / '.git/index', ns=(timestamp, timestamp))
                job_inputs.arm(root, root / '.git')
                reader = prefix + git
                self.assertEqual(subprocess.check_output(reader + ['status', '--porcelain']), b'')
                subprocess.check_output(reader + ['describe', '--always', '--dirty'])
                (root / job_inputs.CANARY).read_bytes()
                if (root / job_inputs.CANARY).stat().st_atime_ns == 0:
                    self.skipTest('filesystem does not update atime on reads')
                with mock.patch.object(job_inputs, 'workflow_inputs', return_value=set()):
                    with self.assertRaisesRegex(RuntimeError, 'no tree input read'):
                        job_inputs.record(root, root / '.git', 'unused', 'build')
                self.assertEqual([name for name in names
                                  if (root / name).stat().st_atime_ns != 0], [])
                (root / names[0]).write_text('changed content')
                self.assertIn(names[0].encode(), subprocess.check_output(
                    reader + ['status', '--porcelain']))
                self.assertTrue(subprocess.check_output(
                    reader + ['describe', '--always', '--dirty']).strip().endswith(b'-dirty'))

    def test_filesystem_round_trip(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(os.environ, RUNNER_OS='Linux'):
            root = Path(directory) / 'repo'
            root.mkdir()
            temporary = Path(directory) / 'temp'
            temporary.mkdir()
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            for name, content in {'src/read.c': 'read me', 'src/unread.c': 'unread',
                                  '.github/workflows/ci.yml': 'jobs:\n  build:\n    steps:\n'
                                  '      - uses: ./.github/actions/example\n',
                                  '.github/actions/example/action.yml': 'runs: {}',
                                  'AGENTS.md': 'job guidance'}.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
            before = (root / 'src/read.c').stat().st_mtime_ns
            job_inputs.arm(root, temporary)
            self.assertTrue((root / job_inputs.CANARY).is_file())
            self.assertEqual((root / job_inputs.CANARY).stat().st_atime_ns, 0)
            self.assertNotIn(job_inputs.CANARY, job_inputs.tracked_paths(root)[0])
            self.assertEqual((root / 'src/read.c').stat().st_mtime_ns, before)
            (root / 'AGENTS.md').read_bytes()
            (root / 'src/read.c').read_bytes()
            if (root / 'src/read.c').stat().st_atime_ns == 0:
                self.skipTest('filesystem does not update atime on reads')
            list((root / 'src').iterdir())
            result = job_inputs.record(root, temporary, '.github/workflows/ci.yml', 'build')
            self.assertIn('AGENTS.md', result['files'])
            self.assertNotIn(job_inputs.CANARY, result['files'])
            self.assertIn('src/read.c', result['files'])
            self.assertNotIn('src/unread.c', result['files'])
            self.assertIn('src', result['directories'])
            self.assertIn('.github/actions/example/action.yml', result['files'])
            self.assertIn('.github/workflows/ci.yml', result['files'])
            job_inputs.arm(root, temporary)
            with self.assertRaisesRegex(RuntimeError, 'no.*read|no.*input'):
                job_inputs.record(root, temporary, '.github/workflows/ci.yml', 'build')


if __name__ == '__main__':
    unittest.main()
