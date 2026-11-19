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

WORKFLOWS = Path(__file__).resolve().parents[3] / ".github" / "workflows"
REFRESH_TOKEN = "./.github/actions/refresh-token"
APP_TOKEN = "${{ steps.app.outputs.token }}"


class RefreshAppTokenTests(unittest.TestCase):
    """A refresh branch pushed or PR opened with GITHUB_TOKEN starts no checks, so every workflow step
    that runs refresh_branch.py or refresh_pr.py does it as the refresh App, never GITHUB_TOKEN."""

    def refresh_steps(self):
        import yaml
        for path in sorted(WORKFLOWS.glob("*.yml")):
            for name, job in yaml.safe_load(path.read_text(encoding="utf-8"))["jobs"].items():
                steps = job.get("steps", [])
                for index, step in enumerate(steps):
                    if "refresh_branch.py" in step.get("run", "") or "refresh_pr.py" in step.get("run", ""):
                        yield f"{path.name} {name}: {step.get('name')}", steps[:index], step

    def test_refresh_workflows_exist(self):
        self.assertGreater(len(list(self.refresh_steps())), 0)

    def test_the_app_token_is_minted_from_its_secrets_before_every_refresh_step(self):
        for where, before, _ in self.refresh_steps():
            with self.subTest(where):
                mint = [step for step in before if step.get("uses") == REFRESH_TOKEN]
                self.assertEqual(len(mint), 1)
                self.assertEqual(mint[0].get("id"), "app")
                self.assertEqual(mint[0]["with"], {
                    "client-id": "${{ secrets.DOC_REFRESH_APP_ID }}",
                    "private-key": "${{ secrets.DOC_REFRESH_APP_PRIVATE_KEY }}"})

    def test_every_refresh_pr_call_takes_the_app_token(self):
        for where, _, step in self.refresh_steps():
            if "refresh_pr.py" in step["run"]:
                with self.subTest(where):
                    self.assertEqual(step.get("env", {}).get("GH_TOKEN"), APP_TOKEN)


if __name__ == "__main__":
    unittest.main()
