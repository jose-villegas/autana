"""Repository REST requests and complete paged listings for CI jobs, over urllib so
no runner needs the GitHub CLI. GETs and writes use http_get retries. Reads GH_TOKEN and GITHUB_REPOSITORY."""
import json
import os
from pathlib import Path
import sys
import urllib.parse
import urllib.request


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
import http_get

PER_PAGE = 100
REQUEST_TIMEOUT_SECONDS = 30


class _ArtifactRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, new_url):
        redirected = super().redirect_request(request, response, code, message, headers, new_url)
        if (redirected is not None and urllib.parse.urlsplit(request.full_url)[:2] !=
                urllib.parse.urlsplit(redirected.full_url)[:2]):
            redirected.remove_header('Authorization')
        return redirected


def call(method, path, body=None, *, raw=False, token=None, repository=None):
    token = token if token is not None else os.environ['GH_TOKEN']
    repository = repository if repository is not None else os.environ['GITHUB_REPOSITORY']
    request = urllib.request.Request(
        f"https://api.github.com/repos/{repository}{path}", method=method,
        data=None if body is None else json.dumps(body).encode(),
        headers={"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json"})
    opener = urllib.request.build_opener(_ArtifactRedirectHandler()) if raw else None
    fetch = http_get.get if method == 'GET' else http_get.send
    data = fetch(request, timeout=REQUEST_TIMEOUT_SECONDS, opener=opener)
    return data if raw else json.loads(data)


def pages(path, key, request=call):
    page = 1
    separator = '&' if '?' in path else '?'
    while True:
        result = request('GET', f'{path}{separator}per_page={PER_PAGE}&page={page}')
        items = result if key is None else result[key]
        yield from items
        if len(items) < PER_PAGE:
            return
        page += 1
