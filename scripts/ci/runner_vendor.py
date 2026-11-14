"""Hosted runners mix CPU vendors, and a traced bake is byte-identical only on the lock's (bake/bake.py's
mesh_cpu_vendor). GitHub offers no way to ask for a vendor, so a tracing job checks its runner first and a
finished run whose only failures are that check is run again, up to VENDOR_TRIES attempts.

    python scripts/ci/runner_vendor.py check          # first step of a tracing job; fails in seconds
    python scripts/ci/runner_vendor.py rerun RUN_ID   # after a Bakes run: re-run its wrong-vendor jobs

Reads GH_TOKEN and GITHUB_REPOSITORY for `rerun`. Standard library only.
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "launcher/tools"))
import github_api  # noqa: E402

# The annotation title `check` fails with; `rerun` re-runs a run only when every failed job carries it.
WRONG_VENDOR = "Wrong CPU vendor"
# Attempts a run gets in all; each draws its failed jobs' runners again.
VENDOR_TRIES = 5


def check(lock_vendor, host_vendor):
    """0 when this runner may trace for the lock (bake.wrong_vendor); else prints the annotation and returns 1."""
    from bake import bake
    refusal = bake.wrong_vendor(lock_vendor, host_vendor)
    if refusal:
        print(f"::error title={WRONG_VENDOR}::{refusal}; {Path(__file__).name} re-runs it")
        return 1
    print(f"CPU vendor {host_vendor or 'not named'}; the lock's: {lock_vendor or 'not named'}")
    return 0


def to_rerun(attempt, failed):
    """Whether a finished attempt is run again: `failed` maps each failed job to its annotation titles."""
    return bool(failed) and attempt < VENDOR_TRIES and all(WRONG_VENDOR in titles for titles in failed.values())


def failed_jobs(run, attempt, request=github_api.call):
    jobs = github_api.pages(f"/actions/runs/{run}/attempts/{attempt}/jobs", "jobs", request=request)
    return {job["name"]: {note.get("title") for note in request("GET", f"/check-runs/{job['id']}/annotations")}
            for job in jobs if job.get("conclusion") == "failure"}


def rerun(run, request=github_api.call):
    attempt = request("GET", f"/actions/runs/{run}")["run_attempt"]
    failed = failed_jobs(run, attempt, request)
    if not to_rerun(attempt, failed):
        print(f"run {run} attempt {attempt}: not re-run ({len(failed)} failed jobs: {', '.join(failed) or 'none'})")
        return 0
    request("POST", f"/actions/runs/{run}/rerun-failed-jobs", {})
    print(f"run {run} attempt {attempt}: re-ran {', '.join(failed)} on fresh runners")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=("check", "rerun"))
    parser.add_argument("run", nargs="?", type=int)
    args = parser.parse_args(argv)
    if args.command == "check":
        from bake import bake
        return check(bake.read_lock_tables().get("mesh_cpu_vendor"), bake.cpu_vendor())
    if args.run is None:
        parser.error("rerun takes RUN_ID")
    return rerun(args.run)


if __name__ == "__main__":
    sys.exit(main())
