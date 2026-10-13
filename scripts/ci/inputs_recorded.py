"""Print true when no other run of this workflow on main is unfinished (with
--defer-in-flight), and the newest unexpired main record, rendered or failed,
names this fingerprint, and FORCE is not "true". The GPU gate holds the
concurrency group and omits --defer-in-flight. With --skip-obsolete, prints
an obsolete-run reason instead if GITHUB_SHA differs from main's head."""
import argparse
import os

from github_api import call, pages

RECORD_PREFIXES = ("doc-images-inputs-", "doc-images-failed-")


def current(artifacts, fingerprint, force=False):
    records = (item for item in artifacts if not item["expired"]
               and item["name"].startswith(RECORD_PREFIXES)
               and item.get("workflow_run", {}).get("head_branch") == "main")
    latest = max(records, key=lambda item: item["created_at"], default=None)
    return bool(fingerprint) and not force and latest is not None and latest["name"] in (
        prefix + fingerprint for prefix in RECORD_PREFIXES)


def main(argv=None, request=call):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fingerprint")
    parser.add_argument("--defer-in-flight", action="store_true",
                        help="hosted lookup: defer while another run is unfinished")
    parser.add_argument("--skip-obsolete", action="store_true",
                        help="GPU gate: skip a trigger SHA that is no longer main's head")
    args = parser.parse_args(argv)
    if args.skip_obsolete:
        head = request("GET", "/git/ref/heads/main")["object"]["sha"]
        if os.environ["GITHUB_SHA"] != head:
            print(f"obsolete: main moved to {head}; dispatch again on main to force")
            return
    if args.defer_in_flight:
        own_run = os.environ["GITHUB_RUN_ID"]
        workflow_ref = os.environ["GITHUB_WORKFLOW_REF"]
        workflow = workflow_ref.split("@", 1)[0].rsplit("/", 1)[-1]
        runs = pages(f"/actions/workflows/{workflow}/runs?branch=main", "workflow_runs", request)
        if any(str(run["id"]) != own_run and run["status"] != "completed" for run in runs):
            print("false")
            return
    result = current(pages("/actions/artifacts", "artifacts", request), args.fingerprint,
                     os.environ.get("FORCE") == "true")
    print(str(result).lower())


if __name__ == "__main__":
    main()
