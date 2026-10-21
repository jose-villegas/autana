"""Repository REST requests and complete paged listings for CI jobs, over urllib so
no runner needs the GitHub CLI. Reads GH_TOKEN and GITHUB_REPOSITORY."""
import json
import os
import time
import urllib.error
import urllib.request


PER_PAGE = 100
GET_ATTEMPTS = 3
RETRY_BASE_DELAY_SECONDS = 1
REQUEST_TIMEOUT_SECONDS = 30
RETRY_HTTP_STATUSES = (429, *range(500, 600))


def call(method, path, body=None):
    request = urllib.request.Request(
        f"https://api.github.com/repos/{os.environ['GITHUB_REPOSITORY']}{path}", method=method,
        data=None if body is None else json.dumps(body).encode(),
        headers={"Authorization": f"Bearer {os.environ['GH_TOKEN']}", "Accept": "application/vnd.github+json"})
    attempts = GET_ATTEMPTS if method == 'GET' else 1
    for attempt in range(attempts):
        try:
            with urllib.request.urlopen(request, timeout=REQUEST_TIMEOUT_SECONDS) as response:
                return json.load(response)
        except (urllib.error.URLError, OSError) as error:
            if (attempt == attempts - 1 or
                    isinstance(error, urllib.error.HTTPError) and error.code not in RETRY_HTTP_STATUSES):
                raise
            time.sleep(RETRY_BASE_DELAY_SECONDS * 2 ** attempt)


def pages(path, key, request=call):
    page = 1
    separator = '&' if '?' in path else '?'
    while True:
        items = request('GET', f'{path}{separator}per_page={PER_PAGE}&page={page}')[key]
        yield from items
        if len(items) < PER_PAGE:
            return
        page += 1
