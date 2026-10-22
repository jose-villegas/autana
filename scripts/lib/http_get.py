"""HTTP GET body bytes with bounded retries for transient transport and server failures."""
import http.client
import time
import urllib.error
import urllib.request

GET_ATTEMPTS = 5
RETRY_BASE_DELAY_SECONDS = 1
RETRY_HTTP_STATUSES = (429, *range(500, 600))


def get(request, *, timeout, opener=None, retry_base_delay=RETRY_BASE_DELAY_SECONDS):
    open_url = opener.open if opener else urllib.request.urlopen
    for attempt in range(GET_ATTEMPTS):
        try:
            with open_url(request, timeout=timeout) as response:
                return response.read()
        except (OSError, http.client.HTTPException) as error:
            permanent = isinstance(error, urllib.error.HTTPError) and error.code not in RETRY_HTTP_STATUSES
            if permanent or attempt == GET_ATTEMPTS - 1:
                noun = "attempt" if attempt == 0 else "attempts"
                error.add_note(f"HTTP GET failed after {attempt + 1} {noun}")
                raise
            if isinstance(error, urllib.error.HTTPError):
                error.close()
            time.sleep(retry_base_delay * 2 ** attempt)
