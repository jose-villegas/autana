#!/usr/bin/env python3
"""Fail when an LFS pointer tracked at HEAD has no object on the LFS server.

    GH_TOKEN=... GITHUB_REPOSITORY=owner/repo python scripts/gates/check_lfs_objects.py

A pushed pointer whose object was never uploaded checks out fine and breaks
every fresh `git lfs pull` with a 404. The server's batch API reports each
missing object, so the gate needs no download.
"""
import base64
import json
import os
import subprocess
import sys
import urllib.request


def pointers():
    """(oid, size) for every LFS pointer at HEAD, from `git lfs ls-files --long --json`."""
    out = subprocess.run(["git", "lfs", "ls-files", "--long", "--json"], check=True,
                         capture_output=True, text=True).stdout
    return sorted({(f["oid"], f["size"]) for f in json.loads(out or "{}").get("files") or []})


def batch(objects, repository, token):
    credentials = base64.b64encode(f"x-access-token:{token}".encode()).decode()
    request = urllib.request.Request(
        f"https://github.com/{repository}.git/info/lfs/objects/batch", method="POST",
        data=json.dumps({"operation": "download", "transfers": ["basic"],
                         "objects": [{"oid": oid, "size": size} for oid, size in objects]}).encode(),
        headers={"Authorization": f"Basic {credentials}", "Accept": "application/vnd.git-lfs+json",
                 "Content-Type": "application/vnd.git-lfs+json"})
    with urllib.request.urlopen(request) as response:
        return json.load(response)["objects"]


def missing(objects, request=batch):
    """Oids the server reports an error for, asked in batches of 100."""
    gone = []
    for start in range(0, len(objects), 100):
        gone += [o["oid"] for o in request(objects[start:start + 100]) if "error" in o]
    return gone


def main():
    objects = pointers()
    gone = missing(objects, lambda chunk: batch(chunk, os.environ["GITHUB_REPOSITORY"], os.environ["GH_TOKEN"]))
    if gone:
        print(f"{len(gone)} of {len(objects)} LFS objects are missing on the server; "
              "upload them with `git lfs push --all origin`:", file=sys.stderr)
        for oid in gone:
            print(f"  {oid}", file=sys.stderr)
        return 1
    print(f"all {len(objects)} LFS objects exist on the server")
    return 0


if __name__ == "__main__":
    sys.exit(main())
