"""A pinned download: fetched beside its target, checked against its SHA-256, and only then moved into place, so a
stalled, failed or tampered fetch leaves nothing at the target."""
import hashlib
import os
import shutil
import urllib.request

TIMEOUT_SECONDS = 60
CHUNK = 1 << 22


def file_sha256(path):
    with open(path, "rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def fetch_verified(url, target, sha256, timeout=TIMEOUT_SECONDS):
    """Writes `url`'s body to `target` when its SHA-256 is `sha256`; raises ValueError naming both digests when it
    is not, and whatever the fetch raised when it fails. Returns `target`."""
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + ".part")
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response, open(partial, "wb") as out:
            shutil.copyfileobj(response, out, CHUNK)
        actual = file_sha256(partial)
        if actual != sha256:
            raise ValueError(f"{target.name}: SHA-256 {actual}, pinned {sha256}")
        os.replace(partial, target)
    finally:
        partial.unlink(missing_ok=True)
    return target
