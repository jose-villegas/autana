import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import refresh_pr


class Recorder:
    def __init__(self, open_pulls):
        self.open_pulls = open_pulls
        self.calls = []

    def __call__(self, method, path, body=None):
        self.calls.append((method, path, body))
        return self.open_pulls if method == "GET" else {}


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


if __name__ == "__main__":
    unittest.main()
