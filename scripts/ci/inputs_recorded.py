"""Print true when the newest unexpired main record, rendered or failed, is this fingerprint's,
no other workflow run is in flight, and FORCE is not "true". The concurrency gate omits the run check."""
import argparse
import os

from github_api import call, pages

RECORD_PREFIXES = ("doc-images-inputs-", "doc-images-failed-")
IN_FLIGHT_STATUSES = ("queued", "in_progress")


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
    parser.add_argument("--workflow", help="hosted lookup: defer while another run is in flight")
    args = parser.parse_args(argv)
    result = current(pages("/actions/artifacts", "artifacts", request), args.fingerprint,
                     os.environ.get("FORCE") == "true")
    if result and args.workflow:
        own_run = os.environ["GITHUB_RUN_ID"]
        runs = pages(f"/actions/workflows/{args.workflow}/runs?branch=main", "workflow_runs", request)
        result = not any(str(run["id"]) != own_run and run["status"] in IN_FLIGHT_STATUSES for run in runs)
    print(str(result).lower())


if __name__ == "__main__":
    main()
