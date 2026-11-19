"""Hosted runners mix CPU vendors, and a traced bake is byte-identical only on the lock's (bake/bake.py's
mesh_cpu_vendor). GitHub offers no way to ask for a vendor, so a tracing job checks its runner first, and
a finished run whose only failures carry WRONG_VENDOR is run again, up to VENDOR_TRIES attempts.

A re-run waits for the run's slowest shard, so a reference shard starts on SHARD_CANDIDATES runners at once.
Each candidate marks its vendor with an artifact and waits until it sees its own marker listed; then the
earliest marked on the lock's vendor traces, the rest finish green, and a wrong-vendor one fails only when
no candidate of the attempt is the lock's vendor and none is still to mark. Two may trace one shard when a
marker lists late; each uploads under its own name and the join keeps one (`dedupe`).

    python scripts/ci/runner_vendor.py check          # first step of a CPU bake job; fails in seconds
    python scripts/ci/runner_vendor.py plan SHARDS    # the shard count and candidate matrix, as outputs
    python scripts/ci/runner_vendor.py mark SHARD CANDIDATE DIR   # a shard candidate's marker, into DIR
    python scripts/ci/runner_vendor.py claim SHARD CANDIDATE      # whether this candidate traces the shard
    python scripts/ci/runner_vendor.py dedupe DIR     # one downloaded artifact per shard in DIR
    python scripts/ci/runner_vendor.py rerun RUN_ID   # after a Bakes run: re-run its wrong-vendor jobs

Reads GH_TOKEN and GITHUB_REPOSITORY for `rerun`, `mark` and `claim`; GITHUB_RUN_ID, GITHUB_RUN_ATTEMPT and
GITHUB_OUTPUT for `mark` and `claim`, which also reads VENDOR_MARKER, `mark`'s output; GITHUB_OUTPUT for
`plan`. Standard library only.
"""
import argparse
import json
import os
import re
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "launcher/tools"))
import github_api  # noqa: E402

# The annotation title `check` and `claim` fail with; `rerun` re-runs a run only when every failed job carries it.
WRONG_VENDOR = "Wrong CPU vendor"
# Attempts a run gets in all; each draws its failed jobs' runners again.
VENDOR_TRIES = 5
# Runners that start on one reference shard; the first of the lock's vendor traces it.
SHARD_CANDIDATES = 4
# How often, and how many times, a candidate reads the run's artifacts for its own marker before it decides.
MARKER_POLL_SECONDS = 5
MARKER_POLLS = 24
# A traced shard's artifact, a candidate's vendor marker, and its job's name.
SHARD_ARTIFACT = re.compile(r"reference-shard-(\d+)-\d+-\d+")
MARKER = re.compile(r"reference-vendor-(\d+)-(\d+)-(\d+)-(ok|wrong)")
CANDIDATE_JOB = re.compile(r"Trace reference shard (\d+) \(candidate (\d+)\)")
# What `claim` decides for a candidate.
TRACE, OTHER, LOST = "trace", "other", "lost"


def check(lock_vendor, host_vendor):
    """0 when this runner may trace for the lock (bake.wrong_vendor); else prints the annotation and returns 1."""
    from bake import bake
    refusal = bake.wrong_vendor(lock_vendor, host_vendor)
    if refusal:
        print(f"::error title={WRONG_VENDOR}::{refusal}; {Path(__file__).name} re-runs it")
        return 1
    print(f"CPU vendor {host_vendor or 'not named'}; the lock's: {lock_vendor or 'not named'}")
    return 0


def marker_name(shard, candidate, attempt, ok):
    return f"reference-vendor-{shard}-{candidate}-{attempt}-{'ok' if ok else 'wrong'}"


def shard_artifact_name(shard, candidate, attempt):
    return f"reference-shard-{shard}-{attempt}-{candidate}"


def shard_done(artifacts, shard):
    """Whether any attempt of the run uploaded `shard` traced; `artifacts` are the run's (name, created_at)."""
    return any((m := SHARD_ARTIFACT.fullmatch(name)) and int(m[1]) == shard for name, _ in artifacts)


def decide(artifacts, running, shard, candidate, attempt, ok):
    """TRACE, OTHER or LOST for a candidate of `shard` whose runner is (`ok`) or is not the lock's vendor.

    `artifacts` hold this candidate's own marker. Of this attempt's candidates on the lock's vendor, the
    earliest marked traces, so one always does. A wrong-vendor one is LOST
    when none of the attempt is the lock's and none still to mark is `running` (the candidates whose jobs of
    this attempt have not completed)."""
    markers = [(int(m[2]), m[4] == "ok", created) for name, created in artifacts
               if (m := MARKER.fullmatch(name)) and int(m[1]) == shard and int(m[3]) == attempt]
    claims = sorted((created, index) for index, good, created in markers if good)
    if ok:
        return TRACE if not claims or claims[0][1] == candidate else OTHER
    if claims:
        return OTHER
    marked = {index for index, _, _ in markers}
    return OTHER if set(running) - marked - {candidate} else LOST


def running_candidates(shard, request=github_api.call):
    """The candidates of `shard` whose jobs of this attempt have not completed."""
    path = f"/actions/runs/{os.environ['GITHUB_RUN_ID']}/attempts/{os.environ['GITHUB_RUN_ATTEMPT']}/jobs"
    return {int(m[2]) for job in github_api.pages(path, "jobs", request=request)
            if job.get("status") != "completed" and (m := CANDIDATE_JOB.fullmatch(job["name"])) and int(m[1]) == shard}


def candidates(count):
    """The candidate numbers for `count` shards: SHARD_CANDIDATES, or fewer when the matrix would pass its cap."""
    from bake import bake
    return list(range(max(1, min(SHARD_CANDIDATES, bake.MATRIX_JOBS // count))))


def plan(shards):
    """Outputs `count`, the shards in the JSON list `shards`, and `candidates`, their candidate matrix."""
    count = len(json.loads(shards))
    output(count=count, candidates=json.dumps(candidates(count)) if count else "[]")
    return 0


def dedupe(folder):
    """Keeps one downloaded shard artifact per shard in `folder`: two candidates' traces of a shard are the same
    bytes, and the join reads every file it is given."""
    kept = set()
    for path in sorted(Path(folder).iterdir()):
        m = SHARD_ARTIFACT.fullmatch(path.name)
        if not m:
            continue
        if m[1] in kept:
            print(f"{path.name}: shard {m[1]} traced twice; keeping one")
            shutil.rmtree(path)
        kept.add(m[1])
    return 0


def run_artifacts(request=github_api.call):
    path = f"/actions/runs/{os.environ['GITHUB_RUN_ID']}/artifacts"
    return [(item["name"], item["created_at"]) for item in github_api.pages(path, "artifacts", request=request)]


def output(**values):
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as out:
        out.writelines(f"{key}={value}\n" for key, value in values.items())


def mark(shard, candidate, folder, lock_vendor, host_vendor, request=github_api.call):
    """Writes this candidate's marker into `folder` and names it in the output `marker`, and its traced shard's
    artifact in `shard_artifact`; no marker when an earlier attempt traced the shard already."""
    if shard_done(run_artifacts(request), shard):
        print(f"shard {shard} was traced by an earlier attempt")
        output(marker="")
        return 0
    from bake import bake
    refusal = bake.wrong_vendor(lock_vendor, host_vendor)
    attempt = int(os.environ["GITHUB_RUN_ATTEMPT"])
    name = marker_name(shard, candidate, attempt, not refusal)
    Path(folder).mkdir(parents=True, exist_ok=True)
    (Path(folder) / name).write_text(f"{host_vendor}\n")
    print(f"CPU vendor {host_vendor or 'not named'}; the lock's: {lock_vendor or 'not named'}")
    output(marker=name, shard_artifact=shard_artifact_name(shard, candidate, attempt))
    return 0


def claim(shard, candidate, marker, request=github_api.call, sleep=time.sleep):
    """Sets the output `trace` for this candidate; fails with WRONG_VENDOR when no candidate can trace the shard."""
    for _ in range(MARKER_POLLS):
        sleep(MARKER_POLL_SECONDS)
        artifacts = run_artifacts(request)
        if any(name == marker for name, _ in artifacts):
            break
    else:
        print(f"{marker} is not listed after {MARKER_POLLS} reads; deciding without it")
    attempt, ok = int(os.environ["GITHUB_RUN_ATTEMPT"]), marker.endswith("-ok")
    verdict = decide(artifacts, running_candidates(shard, request), shard, candidate, attempt, ok)
    output(trace="true" if verdict == TRACE else "false")
    if verdict == LOST:
        print(f"::error title={WRONG_VENDOR}::every candidate for shard {shard} ran on another CPU vendor than "
              f"the lock's; {Path(__file__).name} re-runs it")
        return 1
    print({TRACE: f"candidate {candidate} traces shard {shard}",
           OTHER: f"another candidate traces shard {shard}"}[verdict])
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
    parser.add_argument("command", choices=("check", "plan", "mark", "claim", "dedupe", "rerun"))
    parser.add_argument("args", nargs="*")
    args = parser.parse_args(argv)
    if args.command in ("plan", "dedupe"):
        if len(args.args) != 1:
            parser.error(f"{args.command} takes {'SHARDS' if args.command == 'plan' else 'DIR'}")
        return (plan if args.command == "plan" else dedupe)(args.args[0])
    if args.command in ("check", "mark"):
        from bake import bake
        lock_vendor, host_vendor = bake.read_lock_tables().get("mesh_cpu_vendor"), bake.cpu_vendor()
        if args.command == "check":
            return check(lock_vendor, host_vendor)
        if len(args.args) != 3:
            parser.error("mark takes SHARD CANDIDATE DIR")
        return mark(int(args.args[0]), int(args.args[1]), args.args[2], lock_vendor, host_vendor)
    if args.command == "claim":
        if len(args.args) != 2:
            parser.error("claim takes SHARD CANDIDATE")
        return claim(int(args.args[0]), int(args.args[1]), os.environ["VENDOR_MARKER"])
    if len(args.args) != 1:
        parser.error("rerun takes RUN_ID")
    return rerun(int(args.args[0]))


if __name__ == "__main__":
    sys.exit(main())
