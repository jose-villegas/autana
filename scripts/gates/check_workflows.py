"""Rules every GitHub workflow here keeps.

A self-hosted job never takes an untrusted event trigger. A concurrency group
shared across branches that does not cancel in progress queues every pending
run (`queue: max`): GitHub otherwise keeps one pending run per group, so a new
run silently cancels the one another branch queued. A step installs Debian
packages through APT_INSTALL, which retries and falls back to another mirror,
never with apt-get itself: one mirror outage would fail every job."""
from pathlib import Path
import re
import sys

import yaml


def untrusted(event):
    return event in {"pull_request", "pull_request_target", "issue_comment"} or event.startswith("pull_request_review")


# A group naming one of these holds one branch's or one run's runs, never another branch's.
PER_REF_KEYS = ("github.ref", "github.head_ref", "github.sha", "github.run_id", "github.event.number",
                "github.event.pull_request.number")


APT_INSTALL = "scripts/ci/apt_install.sh"
RAW_APT = re.compile(r"\bapt(?:-get)?\s+(?:-\S+\s+)*(?:update|install)\b")


def shared_group_drops_pending(concurrency):
    """True for a group shared across refs that keeps one pending run and cancels the rest."""
    if isinstance(concurrency, str):
        concurrency = {"group": concurrency}
    if not isinstance(concurrency, dict) or "group" not in concurrency:
        return False
    per_ref = any(key in concurrency["group"] for key in PER_REF_KEYS)
    cancels = str(concurrency.get("cancel-in-progress", "false")).strip() not in ("false", "")
    return not per_ref and not cancels and concurrency.get("queue") != "max"


def check(root="."):
    violations = []
    for path in sorted((Path(root) / ".github/workflows").glob("*.yml")):
        try:
            workflow = yaml.load(path.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
        except yaml.YAMLError as error:
            raise ValueError(f"{path}: invalid workflow YAML: {error}") from error
        if not isinstance(workflow, dict):
            raise ValueError(f"{path}: workflow must be a mapping")
        events = workflow.get("on", {})
        if isinstance(events, str):
            events = [events]
        blocked = sorted(event for event in events if untrusted(event))
        if shared_group_drops_pending(workflow.get("concurrency")):
            violations.append(f"{path}: its concurrency group is shared across branches without queue: max")
        for name, job in workflow.get("jobs", {}).items():
            if shared_group_drops_pending(job.get("concurrency")):
                violations.append(f"{path}: job {name}'s concurrency group is shared across branches without queue: max")
            for step in job.get("steps", []) or []:
                if isinstance(step, dict) and RAW_APT.search(step.get("run", "")):
                    violations.append(f"{path}: job {name} step {step.get('name', '?')!r} runs apt-get; use {APT_INSTALL}")
            runner = job.get("runs-on", [])
            labels = [runner] if isinstance(runner, str) else runner
            if "self-hosted" in labels and blocked:
                violations.append(f"{path}: job {name} uses self-hosted with forbidden triggers: {', '.join(blocked)}")
    return violations


def main():
    try:
        violations = check()
    except (ValueError, OSError) as error:
        print(error, file=sys.stderr)
        return 2
    for violation in violations:
        print(violation)
    print(f"workflow gate: {len(violations)} violation(s)")
    return int(bool(violations))


if __name__ == "__main__":
    raise SystemExit(main())
