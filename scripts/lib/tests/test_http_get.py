"""HTTP retries exercise real sockets, timeouts, statuses and signed redirects."""
import contextlib
import http.server
from pathlib import Path
import sys
import tempfile
import threading
import traceback
import unittest
import urllib.error
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts/lib'))
sys.path.insert(0, str(ROOT / 'launcher/tools'))
import http_get
from bake import bake

TIMEOUT_SECONDS = 0.2
SILENCE_SECONDS = TIMEOUT_SECONDS * 4
RUN = 9
BODY = b'artifact bytes'


@contextlib.contextmanager
def server(statuses, *, location=None):
    requests = []
    release = threading.Event()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append(self.headers)
            status = statuses[min(len(requests) - 1, len(statuses) - 1)]
            if status is None:
                release.wait(SILENCE_SECONDS)
                return
            self.send_response(status)
            if location:
                self.send_header('Location', location)
            self.end_headers()
            self.wfile.write(BODY)

        def log_message(self, *args):
            pass

    with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as host:
        worker = threading.Thread(target=host.serve_forever, kwargs={'poll_interval': TIMEOUT_SECONDS})
        worker.start()
        try:
            yield f'http://127.0.0.1:{host.server_port}/artifact', requests
        finally:
            release.set()
            host.shutdown()
            worker.join()


class HttpGetTests(unittest.TestCase):
    def test_silent_first_connection_retries(self):
        with server([None, 200]) as (url, requests):
            self.assertEqual(http_get.get(url, timeout=TIMEOUT_SECONDS, retry_base_delay=0), BODY)
            self.assertEqual(len(requests), 2)

    def test_service_unavailable_retries(self):
        with server([503, 200]) as (url, requests):
            self.assertEqual(http_get.get(url, timeout=TIMEOUT_SECONDS, retry_base_delay=0), BODY)
            self.assertEqual(len(requests), 2)

    def test_not_found_raises_immediately(self):
        with server([404]) as (url, requests):
            with self.assertRaises(urllib.error.HTTPError) as raised:
                http_get.get(url, timeout=TIMEOUT_SECONDS, retry_base_delay=0)
            raised.exception.close()
            self.assertIn('after 1 attempt', raised.exception.__notes__[0])
            self.assertNotIn('after 1 attempts', raised.exception.__notes__[0])
            self.assertEqual(raised.exception.code, 404)
            self.assertEqual(len(requests), 1)

    def test_exhaustion_reports_attempt_count(self):
        with server([503]) as (url, requests):
            with self.assertRaises(urllib.error.HTTPError) as raised:
                http_get.get(url, timeout=TIMEOUT_SECONDS, retry_base_delay=0)
            raised.exception.close()
            self.assertEqual(len(requests), http_get.GET_ATTEMPTS)
            self.assertIn(f'after {http_get.GET_ATTEMPTS} attempts',
                          ''.join(traceback.format_exception_only(raised.exception)))

    def test_from_run_reason_reports_attempt_count(self):
        for status, attempts, noun in ((404, 1, 'attempt'), (503, http_get.GET_ATTEMPTS, 'attempts')):
            with self.subTest(status=status), server([status]) as (url, requests), \
                    tempfile.TemporaryDirectory() as folder, \
                    mock.patch.object(bake, 'API_URL', url), \
                    mock.patch.object(bake, 'github_token', return_value='secret'), \
                    mock.patch.object(http_get.time, 'sleep'):
                path, reason = bake.from_run({'run': RUN}, bake.MESH_SUFFIX, Path(folder))
                self.assertIsNone(path)
                self.assertEqual(len(requests), attempts)
                self.assertIn('HTTPError: HTTP Error', reason)
                self.assertIn(f'HTTP GET failed after {attempts} {noun}', reason)
                if attempts == 1:
                    self.assertNotIn('1 attempts', reason)

    def test_artifact_redirect_drops_authorization(self):
        with server([200]) as (storage, storage_requests), \
                server([302], location=storage) as (api, api_requests):
            self.assertEqual(bake.github_get(api, 'secret'), BODY)
            self.assertEqual(len(api_requests), 1)
            self.assertEqual(api_requests[0]['Authorization'], 'Bearer secret')
            self.assertEqual(len(storage_requests), 1)
            self.assertIsNone(storage_requests[0]['Authorization'])
