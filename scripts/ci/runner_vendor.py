"""Hosted runners mix CPU vendors, and a traced bake is byte-identical only on the lock's (bake/bake.py's
mesh_cpu_vendor). GitHub offers no way to ask for a vendor, so every Bakes job that traces starts as
candidates (.github/actions/vendor-candidate), and a finished run whose only failures carry WRONG_VENDOR is
run again, up to VENDOR_TRIES attempts, while its commit is still its branch's head.

A re-run waits for the run's slowest job, so a job with traced work starts on CANDIDATES runners at once; a
job with none starts on one. Each candidate marks its vendor with an artifact and waits until it sees its own
marker listed; then the earliest marked on the lock's vendor does the job and the rest finish green. A
wrong-vendor one waits for its started siblings to mark, so when none of the attempt is the lock's vendor
they all fail together and the re-run draws every one of their runners again; a sibling not started yet
leaves it green, so a full queue never holds a runner that sibling needs.
Two may do one job when a marker lists late: each uploads under a name holding its candidate, the join keeps
one copy of a shard once they match, and a lock keeps one row per key. A job's unit is its name without
" (candidate N)".

    python scripts/ci/runner_vendor.py start JOB=COUNT ...   # each job's candidate matrix, as JOB-candidates
    python scripts/ci/runner_vendor.py plan SHARDS           # the shard count and candidate matrix, as outputs
    python scripts/ci/runner_vendor.py mark UNIT CANDIDATE DIR [TRACES [DONE]]   # a candidate's marker, into DIR
    python scripts/ci/runner_vendor.py claim UNIT CANDIDATE  # whether this candidate does the job
    python scripts/ci/runner_vendor.py rerun RUN_ID          # after a Bakes run: re-run its wrong-vendor jobs

Reads GH_TOKEN and GITHUB_REPOSITORY for `rerun`, `mark` and `claim`; GITHUB_RUN_ID, GITHUB_RUN_ATTEMPT and
GITHUB_OUTPUT for `mark` and `claim`, which also reads VENDOR_MARKER, `mark`'s output; GITHUB_OUTPUT for
`start` and `plan`. Standard library only.
"""
import argparse
import fnmatch
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "launcher/tools"))
import github_api  # noqa: E402

# The annotation title `claim` fails with; `rerun` re-runs a run only when every failed job carries it.
WRONG_VENDOR = "Wrong CPU vendor"
# Attempts a run gets in all; each draws its failed jobs' runners again.
VENDOR_TRIES = 5
# Runners that start on one job with traced work; the first of the lock's vendor does it.
CANDIDATES = 4
# How often, and how many times, a candidate reads the run's artifacts for its own marker before it decides.
MARKER_POLL_SECONDS = 5
MARKER_POLLS = 24
# A candidate's vendor marker (unit slug, candidate, attempt, vendor) and its job's name (unit, candidate).
MARKER = re.compile(r"vendor-(.+)-(\d+)-(\d+)-(ok|wrong)")
CANDIDATE_JOB = re.compile(r"(.+) \(candidate (\d+)\)")
# What `claim` decides for a candidate: it does the job, another one does, none can, or a sibling is to mark.
WORK, OTHER, LOST, WAIT = "work", "other", "lost", "wait"


def slug(unit):
    """`unit` as part of an artifact name: Trace reference shard 4 is trace-reference-shard-4."""
    return re.sub(r"[^a-z0-9]+", "-", unit.lower()).strip("-")


def marker_name(unit, candidate, attempt, ok):
    return f"vendor-{slug(unit)}-{candidate}-{attempt}-{'ok' if ok else 'wrong'}"


def done(artifacts, pattern):
    """Whether any attempt of the run uploaded an artifact `pattern` matches; `artifacts` are (name, created_at)."""
    return bool(pattern) and any(fnmatch.fnmatchcase(name, pattern) for name, _ in artifacts)


def decide(artifacts, running, unit, candidate, attempt, ok, queued=frozenset()):
    """WORK, OTHER, LOST or WAIT for a candidate of `unit` whose runner is (`ok`) or is not the lock's vendor.

    `artifacts` hold this candidate's own marker. Of this attempt's candidates on the lock's vendor, the
    earliest marked does the job, so one always does. A wrong-vendor one is LOST when none of the attempt is
    the lock's, WAITs while a sibling still to mark is `running` (started, not completed, in this attempt),
    and is OTHER while one is only `queued`, since a started sibling marks before any install and a queued
    one may wait for this candidate's runner."""
    markers = [(int(m[2]), m[4] == "ok", created) for name, created in artifacts
               if (m := MARKER.fullmatch(name)) and m[1] == slug(unit) and int(m[3]) == attempt]
    claims = sorted((created, index) for index, good, created in markers if good)
    if ok:
        return WORK if not claims or claims[0][1] == candidate else OTHER
    if claims:
        return OTHER
    marked = {index for index, _, _ in markers}
    if set(running) - marked - {candidate}:
        return WAIT
    return OTHER if set(queued) - marked - {candidate} else LOST


def running_candidates(unit, request=github_api.call):
    """The candidates of `unit` whose jobs of this attempt have started and not completed, and those not
    started yet."""
    path = f"/actions/runs/{os.environ['GITHUB_RUN_ID']}/attempts/{os.environ['GITHUB_RUN_ATTEMPT']}/jobs"
    jobs = [(int(m[2]), job.get("status")) for job in github_api.pages(path, "jobs", request=request)
            if (m := CANDIDATE_JOB.fullmatch(job["name"])) and m[1] == unit and job.get("status") != "completed"]
    return ({index for index, status in jobs if status == "in_progress"},
            {index for index, status in jobs if status != "in_progress"})


def candidates(count):
    """The candidate numbers for each of `count` reference shards: CANDIDATES, or fewer when the shard matrix
    would pass its cap."""
    from bake import bake
    return list(range(max(1, min(CANDIDATES, bake.MATRIX_JOBS // count))))


def start(counts):
    """Outputs JOB-candidates, the candidate numbers of each JOB that `counts` maps to its number of missing
    traced bakes: CANDIDATES runners when it has any, else one, since a job that traces nothing may run on
    any vendor."""
    output(**{f"{job}-candidates": json.dumps(list(range(CANDIDATES if count else 1))) for job, count in counts.items()})
    return 0


def plan(shards):
    """Outputs `count`, the shards in the JSON list `shards`, and `candidates`, their candidate matrix."""
    count = len(json.loads(shards))
    output(count=count, candidates=json.dumps(candidates(count)) if count else "[]")
    return 0


def run_artifacts(request=github_api.call):
    path = f"/actions/runs/{os.environ['GITHUB_RUN_ID']}/artifacts"
    return [(item["name"], item["created_at"]) for item in github_api.pages(path, "artifacts", request=request)]


def output(**values):
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as out:
        out.writelines(f"{key}={value}\n" for key, value in values.items())


def mark(unit, candidate, folder, lock_vendor, host_vendor, traces=True, done_pattern="", request=github_api.call):
    """Writes this candidate's marker into `folder` and names it in the output `marker`. No marker when nothing
    `traces`, and candidate 0 does the job (output `work`), nor when an earlier attempt did it (an artifact
    `done_pattern` matches is listed)."""
    if not traces:
        output(marker="", work="true" if candidate == 0 else "false")
        return 0
    if done(run_artifacts(request), done_pattern):
        print(f"{unit} is done already")
        output(marker="", work="false")
        return 0
    from bake import bake
    refusal = bake.wrong_vendor(lock_vendor, host_vendor)
    name = marker_name(unit, candidate, int(os.environ["GITHUB_RUN_ATTEMPT"]), not refusal)
    Path(folder).mkdir(parents=True, exist_ok=True)
    (Path(folder) / name).write_text(f"{host_vendor}\n")
    print(f"CPU vendor {host_vendor or 'not named'}; the lock's: {lock_vendor or 'not named'}")
    output(marker=name)
    return 0


def claim(unit, candidate, marker, request=github_api.call, sleep=time.sleep):
    """Sets the output `work` for this candidate; fails with WRONG_VENDOR when no candidate can do the job."""
    for _ in range(MARKER_POLLS):
        sleep(MARKER_POLL_SECONDS)
        artifacts = run_artifacts(request)
        if any(name == marker for name, _ in artifacts):
            break
    else:
        print(f"{marker} is not listed after {MARKER_POLLS} reads; deciding without it")
    attempt, ok = int(os.environ["GITHUB_RUN_ATTEMPT"]), marker.endswith("-ok")
    for read in range(MARKER_POLLS + 1):
        if read:
            sleep(MARKER_POLL_SECONDS)
        # Jobs first: a sibling whose job completes after this read uploaded its marker before then.
        started, queued = running_candidates(unit, request)
        verdict = decide(run_artifacts(request), started, unit, candidate, attempt, ok, queued)
        if verdict != WAIT:
            break
    else:
        # A started sibling that never lists its marker may yet be the lock's vendor; failing would fail its run.
        print(f"a sibling of {unit} has not marked after {MARKER_POLLS} reads; leaving the job to it")
        verdict = OTHER
    output(work="true" if verdict == WORK else "false")
    if verdict == LOST:
        print(f"::error title={WRONG_VENDOR}::every candidate for {unit} ran on another CPU vendor than "
              f"the lock's; {Path(__file__).name} re-runs it")
        return 1
    print({WORK: f"candidate {candidate} does {unit}", OTHER: f"another candidate does {unit}"}[verdict])
    return 0


def to_rerun(attempt, failed):
    """Whether a finished attempt is run again: `failed` maps each failed job to its annotation titles."""
    return bool(failed) and attempt < VENDOR_TRIES and all(WRONG_VENDOR in titles for titles in failed.values())


def failed_jobs(run, attempt, request=github_api.call):
    """Each failed job's annotation titles, leaving out a matrix's one-name stand-in: it fails whenever its
    candidates did, and re-running the failed jobs re-runs it too."""
    jobs = list(github_api.pages(f"/actions/runs/{run}/attempts/{attempt}/jobs", "jobs", request=request))
    names = [job["name"] for job in jobs if job.get("conclusion") == "failure"]
    return {job["name"]: {note.get("title") for note in request("GET", f"/check-runs/{job['id']}/annotations")}
            for job in jobs if job["name"] in names
            and not any(other.startswith(f"{job['name']} (candidate ") for other in names)}


def branch_head(branch, request=github_api.call):
    """The branch's head commit in this repository, or None once it is deleted; a fork's branch is looked up
    here too, so a fork's run is not re-run."""
    try:
        return request("GET", f"/branches/{urllib.parse.quote(branch, safe='')}").get("commit", {}).get("sha")
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise


def rerun(run, request=github_api.call):
    record = request("GET", f"/actions/runs/{run}")
    attempt = record["run_attempt"]
    if branch_head(record["head_branch"], request) != record["head_sha"]:
        # A re-run would take the branch's one Bakes slot and cancel the newer head's run.
        print(f"run {run} attempt {attempt}: not re-run, its branch has moved past {record['head_sha'][:9]}")
        return 0
    failed = failed_jobs(run, attempt, request)
    if not to_rerun(attempt, failed):
        print(f"run {run} attempt {attempt}: not re-run ({len(failed)} failed jobs: {', '.join(failed) or 'none'})")
        return 0
    request("POST", f"/actions/runs/{run}/rerun-failed-jobs", {})
    print(f"run {run} attempt {attempt}: re-ran {', '.join(failed)} on fresh runners")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=("start", "plan", "mark", "claim", "rerun"))
    parser.add_argument("args", nargs="*")
    args = parser.parse_args(argv)
    if args.command == "start":
        if not args.args or not all(re.fullmatch(r"[\w-]+=\d+", arg) for arg in args.args):
            parser.error("start takes JOB=COUNT ...")
        return start({job: int(count) for job, count in (arg.split("=") for arg in args.args)})
    if args.command == "plan":
        if len(args.args) != 1:
            parser.error("plan takes SHARDS")
        return plan(args.args[0])
    if args.command == "mark":
        if not 3 <= len(args.args) <= 5:
            parser.error("mark takes UNIT CANDIDATE DIR [TRACES [DONE]]")
        unit, candidate, folder, traces, done_pattern = args.args + ["true", ""][len(args.args) - 3:]
        from bake import bake
        return mark(unit, int(candidate), folder, bake.read_lock_tables().get("mesh_cpu_vendor"), bake.cpu_vendor(),
                    traces == "true", done_pattern)
    if args.command == "claim":
        if len(args.args) != 2:
            parser.error("claim takes UNIT CANDIDATE")
        return claim(args.args[0], int(args.args[1]), os.environ["VENDOR_MARKER"])
    if len(args.args) != 1:
        parser.error("rerun takes RUN_ID")
    return rerun(int(args.args[0]))


if __name__ == "__main__":
    sys.exit(main())
