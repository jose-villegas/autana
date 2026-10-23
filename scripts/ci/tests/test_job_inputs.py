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
                self.assertEqual(job_inputs.matching_paths([change], manifest), expected)

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
                    iter([{'filename': 'src/read.c', 'status': 'modified'}]), iter([]), iter([])]), \
                mock.patch.object(job_inputs, 'latest_job_artifact', return_value={'id': 5}):
            self.assertEqual(job_inputs.affected('ci', ['build'], 7, 'main', request), {'build': 'true'})
            self.assertIn('src/read.c', (Path(directory) / 'summary').read_text())
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
            (root / job_inputs.CANARY).write_text('canary')
            os.utime(root / job_inputs.CANARY, ns=(0, 0))
            with mock.patch.object(job_inputs, 'tracked_paths', return_value=({job_inputs.CANARY}, set())), \
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
            (root / job_inputs.CANARY).write_text('canary')
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
                                  job_inputs.CANARY: 'canary'}.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
            before = (root / 'src/read.c').stat().st_mtime_ns
            job_inputs.arm(root, temporary)
            self.assertEqual((root / 'src/read.c').stat().st_mtime_ns, before)
            (root / 'src/read.c').read_bytes()
            if (root / 'src/read.c').stat().st_atime_ns == 0:
                self.skipTest('filesystem does not update atime on reads')
            list((root / 'src').iterdir())
            result = job_inputs.record(root, temporary, '.github/workflows/ci.yml', 'build')
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
