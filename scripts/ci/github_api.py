"""Repository REST requests and complete paged listings for CI jobs, over urllib so
no runner needs the GitHub CLI. GETs and writes use http_get retries. Reads GH_TOKEN and GITHUB_REPOSITORY."""
from http import HTTPStatus
import json
import os
from pathlib import Path
import sys
import urllib.error
import urllib.parse
import urllib.request


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
import http_get

PER_PAGE = 100
# Each smaller size must divide every larger size so a retry can resume at the same offset.
PAGE_SIZES = (PER_PAGE, 50, 25)
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
    size_index = 0
    offset = 0
    separator = '&' if '?' in path else '?'
    while True:
        per_page = PAGE_SIZES[size_index]
        page = offset // per_page + 1
        try:
            result = request('GET', f'{path}{separator}per_page={per_page}&page={page}')
        except urllib.error.HTTPError as error:
            if error.code != HTTPStatus.BAD_GATEWAY or size_index == len(PAGE_SIZES) - 1:
                raise
            size_index += 1
            continue
        items = result if key is None else result[key]
        yield from items
        if len(items) < per_page:
            return
        offset += per_page
