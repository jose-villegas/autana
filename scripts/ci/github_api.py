"""Repository REST requests and complete paged listings for CI jobs, over urllib so
no runner needs the GitHub CLI. GETs use http_get retries. Reads GH_TOKEN and GITHUB_REPOSITORY."""
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


def call(method, path, body=None, *, raw=False):
    request = urllib.request.Request(
        f"https://api.github.com/repos/{os.environ['GITHUB_REPOSITORY']}{path}", method=method,
        data=None if body is None else json.dumps(body).encode(),
        headers={"Authorization": f"Bearer {os.environ['GH_TOKEN']}", "Accept": "application/vnd.github+json"})
    opener = urllib.request.build_opener(_ArtifactRedirectHandler()) if raw else None
    if method == 'GET':
        data = http_get.get(request, timeout=REQUEST_TIMEOUT_SECONDS, opener=opener)
    else:
        with (opener.open if opener else urllib.request.urlopen)(request, timeout=REQUEST_TIMEOUT_SECONDS) as response:
            data = response.read()
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
