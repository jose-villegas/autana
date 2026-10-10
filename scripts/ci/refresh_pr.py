#!/usr/bin/env python3
"""Open, update or close a doc refresh pull request over the REST API.

The doc-image workflows call this instead of the GitHub CLI, so no runner needs it. Reads GH_TOKEN,
GITHUB_REPOSITORY and REFRESH_BRANCH from the environment.
"""
import os
import sys
from github_api import call


def open_number(branch, request=call):
    owner = os.environ["GITHUB_REPOSITORY"].split("/")[0]
    found = request("GET", f"/pulls?state=open&head={owner}:{branch}")
    return found[0]["number"] if found else None


def sync(branch, title, body, request=call):
    number = open_number(branch, request)
    if number is None:
        request("POST", "/pulls", {"title": title, "head": branch,
                                   "base": "main", "body": body})
    else:
        request("PATCH", f"/pulls/{number}", {"body": body})


def close(branch, comment, request=call):
    number = open_number(branch, request)
    if number is not None:
        request("POST", f"/issues/{number}/comments", {"body": comment})
        request("PATCH", f"/pulls/{number}", {"state": "closed"})
        request("DELETE", f"/git/refs/heads/{branch}")


def main(argv):
    branch = os.environ["REFRESH_BRANCH"]
    if len(argv) == 4 and argv[1] == "sync":
        with open(argv[3]) as body:
            sync(branch, argv[2], body.read())
        return 0
    if len(argv) == 3 and argv[1] == "close":
        close(branch, argv[2])
        return 0
    print("usage: refresh_pr.py sync TITLE BODY_FILE | close COMMENT", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
