"""Repository REST requests and complete paged listings for CI jobs, over urllib so
no runner needs the GitHub CLI. GETs use http_get retries. Reads GH_TOKEN and GITHUB_REPOSITORY."""
import json
import os
from pathlib import Path
import sys
import urllib.request


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
import http_get

PER_PAGE = 100
REQUEST_TIMEOUT_SECONDS = 30


def call(method, path, body=None):
    request = urllib.request.Request(
        f"https://api.github.com/repos/{os.environ['GITHUB_REPOSITORY']}{path}", method=method,
        data=None if body is None else json.dumps(body).encode(),
        headers={"Authorization": f"Bearer {os.environ['GH_TOKEN']}", "Accept": "application/vnd.github+json"})
    if method == 'GET':
        return json.loads(http_get.get(request, timeout=REQUEST_TIMEOUT_SECONDS))
    with urllib.request.urlopen(request, timeout=REQUEST_TIMEOUT_SECONDS) as response:
        return json.load(response)


def pages(path, key, request=call):
    page = 1
    separator = '&' if '?' in path else '?'
    while True:
        items = request('GET', f'{path}{separator}per_page={PER_PAGE}&page={page}')[key]
        yield from items
        if len(items) < PER_PAGE:
            return
        page += 1
