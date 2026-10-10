"""Compare inputs with the latest unexpired GPU doc record on main."""
import argparse
import json
import os
import subprocess

ARTIFACTS_PER_PAGE = 100
RECORD_PREFIXES = ("doc-images-inputs-", "doc-images-failed-")


def current(artifacts, fingerprint, force=False):
    records = (item for item in artifacts if not item["expired"]
               and item["name"].startswith(RECORD_PREFIXES)
               and item.get("workflow_run", {}).get("head_branch") == "main")
    latest = max(records, key=lambda item: item["created_at"], default=None)
    return not force and latest is not None and latest["name"].endswith(fingerprint)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fingerprint")
    args = parser.parse_args()
    result = subprocess.run(["gh", "api", "--paginate", "--slurp",
                             f"repos/{os.environ['GITHUB_REPOSITORY']}/actions/artifacts?per_page={ARTIFACTS_PER_PAGE}"],
                            capture_output=True, text=True, check=True)
    artifacts = [item for page in json.loads(result.stdout) for item in page["artifacts"]]
    print(str(current(artifacts, args.fingerprint, os.environ.get("FORCE") == "true")).lower())


if __name__ == "__main__":
    main()
