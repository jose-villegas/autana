"""Repository requests retain their authentication and traverse every API page."""
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import github_api


class GithubApiTests(unittest.TestCase):
    def test_call_sends_repository_authentication_and_json(self):
        with mock.patch.dict(os.environ, GH_TOKEN='token', GITHUB_REPOSITORY='owner/repo'), \
                mock.patch.object(github_api.urllib.request, 'urlopen',
                                  return_value=io.BytesIO(b'{"number": 7}')) as open_url:
            self.assertEqual(github_api.call('POST', '/pulls', {'title': 'text'}), {'number': 7})
        request = open_url.call_args.args[0]
        self.assertEqual(request.full_url, 'https://api.github.com/repos/owner/repo/pulls')
        self.assertEqual(request.get_method(), 'POST')
        self.assertEqual(request.get_header('Authorization'), 'Bearer token')
        self.assertEqual(request.data, b'{"title": "text"}')

    def test_pages_stop_after_short_page_with_existing_query(self):
        first = list(range(github_api.PER_PAGE))
        request = mock.Mock(side_effect=[{'items': first}, {'items': ['last']}])
        self.assertEqual(list(github_api.pages('/listing?branch=main', 'items', request)), first + ['last'])
        self.assertEqual(request.call_args_list, [mock.call('GET', f'/listing?branch=main&per_page=100&page={page}')
                                                 for page in (1, 2)])

    def test_ci_entry_points_show_usage_from_repository_root(self):
        root = Path(__file__).resolve().parents[3]
        with mock.patch.dict(os.environ, REFRESH_BRANCH='refresh'):
            for name in ('refresh_pr', 'inputs_recorded'):
                result = subprocess.run([sys.executable, f'scripts/ci/{name}.py'], cwd=root,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn('usage:', result.stderr)
