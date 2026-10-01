"""Downloads a source model once into a cache beside this file, checks it
against a known SHA-256, and unpacks it. A bake names the archive; nothing
downloaded is ever committed."""

import hashlib
import pathlib
import shutil
import urllib.request
import zipfile

from . import log

CACHE = pathlib.Path(__file__).resolve().parent / ".cache"


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch_zip(url, sha256, name):
    """The unpacked directory of the zip at `url`, fetched on first use."""
    CACHE.mkdir(exist_ok=True)
    archive = CACHE / f"{name}.zip"
    unpacked = CACHE / name
    if not archive.exists():
        log(f"downloading {url}")
        partial = archive.with_suffix(".part")
        request = urllib.request.Request(url, headers={"User-Agent": "autana-r3d-importer/1"})
        with urllib.request.urlopen(request) as response, open(partial, "wb") as destination:
            shutil.copyfileobj(response, destination)
        partial.replace(archive)
    actual = _sha256(archive)
    if actual != sha256:
        raise SystemExit(f"{archive} has SHA-256 {actual}, expected {sha256}: delete it and fetch again")
    if not unpacked.exists():
        with zipfile.ZipFile(archive) as z:
            z.extractall(unpacked)
    return unpacked
