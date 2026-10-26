"""refresh_pr opens, updates, labels and closes the refresh pull request."""
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import github_api
import refresh_pr


class Recorder:
    def __init__(self, open_pulls):
        self.open_pulls = open_pulls
        self.calls = []

    def __call__(self, method, path, body=None):
        self.calls.append((method, path, body))
        return self.open_pulls if method == "GET" else {"number": 9}


class RefreshPrTests(unittest.TestCase):
    def setUp(self):
        os.environ["GITHUB_REPOSITORY"] = "owner/repo"

    def test_sync_opens_a_pull_request_when_none_is_open(self):
        request = Recorder([])
        refresh_pr.sync("refresh", "title", "text", request)
        self.assertEqual(request.calls[0], ("GET", "/pulls?state=open&head=owner:refresh", None))
        self.assertEqual(request.calls[1][:2], ("POST", "/pulls"))
        self.assertEqual(request.calls[1][2]["head"], "refresh")
        self.assertEqual(request.calls[1][2]["body"], "text")
        self.assertEqual(request.calls[1][2]["title"], "title")

    def test_sync_updates_the_body_of_the_open_pull_request(self):
        request = Recorder([{"number": 7}])
        refresh_pr.sync("refresh", "title", "text", request)
        self.assertEqual(request.calls[1], ("PATCH", "/pulls/7", {"body": "text"}))
        self.assertEqual(len(request.calls), 2)

    def test_sync_labels_the_pull_request_it_opened_or_updated(self):
        for open_pulls, number in (([], 9), ([{"number": 7}], 7)):
            request = Recorder(open_pulls)
            refresh_pr.sync("refresh", "title", "text", request, labels=["needs-board-perf"])
            self.assertEqual(request.calls[-1], ("POST", f"/issues/{number}/labels", {"labels": ["needs-board-perf"]}))

    def test_close_comments_closes_and_deletes_the_branch(self):
        request = Recorder([{"number": 7}])
        refresh_pr.close("refresh", "clean", request)
        self.assertEqual([call[:2] for call in request.calls[1:]],
                         [("POST", "/issues/7/comments"), ("PATCH", "/pulls/7"),
                          ("DELETE", "/git/refs/heads/refresh")])

    def test_close_without_an_open_pull_request_does_nothing(self):
        request = Recorder([])
        refresh_pr.close("refresh", "clean", request)
        self.assertEqual(len(request.calls), 1)

    def test_main_close_and_sync_reach_http_layer(self):
        for action in ('close', 'sync'):
            with self.subTest(action=action), tempfile.TemporaryDirectory() as directory, \
                    mock.patch.dict(os.environ, GH_TOKEN='token', GITHUB_REPOSITORY='owner/repo',
                                    REFRESH_BRANCH='refresh'):
                body = Path(directory) / 'body.txt'
                body.write_text('body')
                responses = ([[], {'number': 9}] if action == 'sync' else [[{'number': 7}], {}, {}, {}])
                with mock.patch.object(github_api.urllib.request, 'urlopen',
                        side_effect=[io.BytesIO(json.dumps(value).encode()) for value in responses]) as http:
                    argv = ['refresh_pr.py', 'sync', 'title', str(body)] if action == 'sync' else [
                        'refresh_pr.py', 'close', 'clean']
                    self.assertEqual(refresh_pr.main(argv), 0)
                self.assertEqual([item.args[0].get_method() for item in http.call_args_list],
                                 ['GET', 'POST'] if action == 'sync' else ['GET', 'POST', 'PATCH', 'DELETE'])
                self.assertTrue(all(item.args[0].full_url.startswith(
                    'https://api.github.com/repos/owner/repo/') for item in http.call_args_list))

if __name__ == "__main__":
    unittest.main()
