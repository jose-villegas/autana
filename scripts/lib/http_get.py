"""HTTP response body bytes with bounded retries: a GET retries transient transport and server
failures; a write retries only a failure to connect or send, which the server never answered."""
import http.client
import time
import urllib.error
import urllib.request

GET_ATTEMPTS = 5
RETRY_BASE_DELAY_SECONDS = 1
RETRY_HTTP_STATUSES = (429, *range(500, 600))


def transient(error):
    return not isinstance(error, urllib.error.HTTPError) or error.code in RETRY_HTTP_STATUSES


def unsent(error):
    """urllib raises URLError (not HTTPError) only while connecting or sending, so the server
    never acted on the request and repeating a write cannot apply it twice."""
    return isinstance(error, urllib.error.URLError) and not isinstance(error, urllib.error.HTTPError)


def get(request, *, timeout, opener=None, retry_base_delay=RETRY_BASE_DELAY_SECONDS, retryable=transient):
    open_url = opener.open if opener else urllib.request.urlopen
    method = request.get_method() if isinstance(request, urllib.request.Request) else 'GET'
    for attempt in range(GET_ATTEMPTS):
        try:
            with open_url(request, timeout=timeout) as response:
                return response.read()
        except (OSError, http.client.HTTPException) as error:
            if not retryable(error) or attempt == GET_ATTEMPTS - 1:
                noun = "attempt" if attempt == 0 else "attempts"
                error.add_note(f"HTTP {method} failed after {attempt + 1} {noun}")
                raise
            if isinstance(error, urllib.error.HTTPError):
                error.close()
            time.sleep(retry_base_delay * 2 ** attempt)


def send(request, *, timeout, opener=None, retry_base_delay=RETRY_BASE_DELAY_SECONDS):
    return get(request, timeout=timeout, opener=opener, retry_base_delay=retry_base_delay, retryable=unsent)
