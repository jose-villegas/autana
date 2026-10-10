"""Repository REST requests and complete paged listings for CI jobs."""
import json
import os
import urllib.request


PER_PAGE = 100


def call(method, path, body=None):
    request = urllib.request.Request(
        f"https://api.github.com/repos/{os.environ['GITHUB_REPOSITORY']}{path}", method=method,
        data=None if body is None else json.dumps(body).encode(),
        headers={"Authorization": f"Bearer {os.environ['GH_TOKEN']}", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(request) as response:
        return json.load(response)


def pages(path, key, request=call):
    page = 1
    separator = '&' if '?' in path else '?'
    while True:
        items = request('GET', f'{path}{separator}per_page={PER_PAGE}&page={page}')[key]
        if not items:
            return
        yield from items
        page += 1
