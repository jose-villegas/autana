"""Repository requests retain their authentication and traverse every API page."""
from email.message import Message
import http.client
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import urllib.error
import urllib.response
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

    def test_binary_artifact_download_keeps_repository_request_owner(self):
        with mock.patch.dict(os.environ, GH_TOKEN='token', GITHUB_REPOSITORY='owner/repo'), \
                mock.patch.object(github_api.urllib.request, 'build_opener') as build_opener:
            build_opener.return_value.open.return_value = io.BytesIO(b'zip bytes')
            self.assertEqual(github_api.call('GET', '/actions/artifacts/5/zip', raw=True), b'zip bytes')

    def test_artifact_redirect_omits_authentication_and_retries_both_hops(self):
        artifact_url = 'https://api.github.com/repos/owner/repo/actions/artifacts/5/zip'
        storage_url = 'https://storage.example/artifact.zip?signature=signed'
        payload = b'zip bytes'
        for transient_errors in (False, True):
            requests = []
            attempts = {artifact_url: 0, storage_url: 0}

            class ArtifactTransport(urllib.request.HTTPSHandler):
                def https_open(self, request):
                    url = request.full_url
                    requests.append((url, request.get_header('Authorization')))
                    attempts[url] += 1
                    headers = Message()
                    if transient_errors and attempts[url] == 1:
                        status = http.client.SERVICE_UNAVAILABLE
                        body = b'try again'
                    elif url == artifact_url:
                        status = http.client.FOUND
                        headers['Location'] = storage_url
                        body = b''
                    elif request.has_header('Authorization'):
                        status = http.client.UNAUTHORIZED
                        body = b'credentials rejected'
                    else:
                        status = http.client.OK
                        body = payload
                    response = urllib.response.addinfourl(io.BytesIO(body), headers, url, status)
                    response.msg = http.client.responses[status]
                    return response

            build_opener = urllib.request.build_opener
            transport = ArtifactTransport()
            opener = build_opener(transport)
            with self.subTest(transient_errors=transient_errors), mock.patch.dict(
                    os.environ, GH_TOKEN='token', GITHUB_REPOSITORY='owner/repo'), \
                    mock.patch.object(urllib.request, 'urlopen', side_effect=opener.open), \
                    mock.patch.object(urllib.request, 'build_opener',
                                      side_effect=lambda *handlers: build_opener(transport, *handlers)), \
                    mock.patch('time.sleep') as sleep:
                self.assertEqual(github_api.call('GET', '/actions/artifacts/5/zip', raw=True), payload)
                self.assertTrue(all(auth == 'Bearer token' for url, auth in requests if url == artifact_url))
                self.assertTrue(all(auth is None for url, auth in requests if url == storage_url))
                self.assertEqual(attempts[storage_url], 2 if transient_errors else 1)
                self.assertEqual(sleep.call_count, 2 if transient_errors else 0)

    def test_pages_handles_a_bare_list(self):
        request = mock.Mock(side_effect=[list(range(github_api.PER_PAGE)), ['last']])
        self.assertEqual(list(github_api.pages('/pulls/7/files', None, request)),
                         list(range(github_api.PER_PAGE)) + ['last'])

    def test_get_retries_transient_errors_with_backoff_and_timeout(self):
        errors = (urllib.error.URLError('connect'), TimeoutError('timeout'),
                  ConnectionError('connect'), OSError('connect'),
                  urllib.error.HTTPError('url', 503, 'unavailable', {}, None),
                  urllib.error.HTTPError('url', 429, 'limited', {}, None),
                  urllib.error.HTTPError('url', 500, 'server error', {}, None),
                  urllib.error.HTTPError('url', 599, 'server error', {}, None),
                  http.client.IncompleteRead(b'partial'))
        for error in errors:
            with self.subTest(error=error), mock.patch.dict(
                    os.environ, GH_TOKEN='token', GITHUB_REPOSITORY='owner/repo'), \
                    mock.patch('time.sleep') as sleep, \
                    mock.patch.object(github_api.urllib.request, 'urlopen', side_effect=[
                        *([error] * (github_api.http_get.GET_ATTEMPTS - 1)),
                        io.BytesIO(b'{"ok": true}')]) as open_url:
                self.assertEqual(github_api.call('GET', '/actions/artifacts'), {'ok': True})
                self.assertEqual(open_url.call_count, github_api.http_get.GET_ATTEMPTS)
                self.assertTrue(all(call.kwargs == {'timeout': github_api.REQUEST_TIMEOUT_SECONDS}
                                    for call in open_url.call_args_list))
                self.assertEqual(sleep.call_args_list, [
                    mock.call(github_api.http_get.RETRY_BASE_DELAY_SECONDS * 2 ** attempt)
                    for attempt in range(github_api.http_get.GET_ATTEMPTS - 1)])

    def test_nontransient_or_non_get_errors_are_not_retried(self):
        rejected = [('GET', urllib.error.HTTPError('url', code, 'rejected', {}, None))
                    for code in (400, 401, 403, 404, 499, 600)]
        unavailable = urllib.error.HTTPError('url', 503, 'unavailable', {}, None)
        for method, error in [*rejected, ('POST', unavailable),
                              ('PATCH', TimeoutError('timeout')), ('DELETE', ConnectionError('connect'))]:
            with self.subTest(method=method, error=error), mock.patch.dict(
                    os.environ, GH_TOKEN='token', GITHUB_REPOSITORY='owner/repo'), \
                    mock.patch('time.sleep') as sleep, \
                    mock.patch.object(github_api.urllib.request, 'urlopen', side_effect=error) as open_url:
                with self.assertRaises(type(error)) as raised:
                    github_api.call(method, '/pulls')
                self.assertIs(raised.exception, error)
                self.assertEqual(open_url.call_count, 1)
                sleep.assert_not_called()

    def test_writes_retry_only_a_request_that_never_reached_the_server(self):
        handshake = urllib.error.URLError(TimeoutError('_ssl.c:993: The handshake operation timed out'))
        for method in ('POST', 'PATCH', 'DELETE'):
            with self.subTest(method=method), mock.patch.dict(
                    os.environ, GH_TOKEN='token', GITHUB_REPOSITORY='owner/repo'),                     mock.patch('time.sleep') as sleep,                     mock.patch.object(github_api.urllib.request, 'urlopen', side_effect=[
                        handshake, io.BytesIO(b'{"number": 7}')]) as open_url:
                self.assertEqual(github_api.call(method, '/pulls', {'title': 'text'}), {'number': 7})
                self.assertEqual(open_url.call_count, 2)
                self.assertEqual(sleep.call_args_list, [mock.call(github_api.http_get.RETRY_BASE_DELAY_SECONDS)])

    def test_get_exhausts_attempts_and_propagates_last_error(self):
        errors = [urllib.error.URLError(f'connect {attempt}')
                  for attempt in range(github_api.http_get.GET_ATTEMPTS)]
        with mock.patch.dict(os.environ, GH_TOKEN='token', GITHUB_REPOSITORY='owner/repo'), \
                mock.patch('time.sleep') as sleep, \
                mock.patch.object(github_api.urllib.request, 'urlopen', side_effect=errors) as open_url:
            with self.assertRaises(urllib.error.URLError) as raised:
                github_api.call('GET', '/actions/runs')
            self.assertIs(raised.exception, errors[-1])
            self.assertEqual(open_url.call_count, github_api.http_get.GET_ATTEMPTS)
            self.assertEqual(sleep.call_count, github_api.http_get.GET_ATTEMPTS - 1)

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
