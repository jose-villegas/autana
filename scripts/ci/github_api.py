"""Repository REST requests and complete paged listings for CI jobs, over urllib so
no runner needs the GitHub CLI. Reads GH_TOKEN and GITHUB_REPOSITORY."""
import http.client
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request


PER_PAGE = 100
GET_ATTEMPTS = 5
RETRY_BASE_DELAY_SECONDS = 1
REQUEST_TIMEOUT_SECONDS = 30
RETRY_HTTP_STATUSES = (429, *range(500, 600))


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
    open_url = urllib.request.build_opener(_ArtifactRedirectHandler()).open if raw else urllib.request.urlopen
    attempts = GET_ATTEMPTS if method == 'GET' else 1
    for attempt in range(attempts):
        try:
            with open_url(request, timeout=REQUEST_TIMEOUT_SECONDS) as response:
                return response.read() if raw else json.load(response)
        except (OSError, http.client.HTTPException) as error:
            if (attempt == attempts - 1 or
                    isinstance(error, urllib.error.HTTPError) and error.code not in RETRY_HTTP_STATUSES):
                raise
            time.sleep(RETRY_BASE_DELAY_SECONDS * 2 ** attempt)


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
