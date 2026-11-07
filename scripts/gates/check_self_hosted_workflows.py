"""Workflow rules the self-hosted runners depend on.

A self-hosted job never takes an untrusted event trigger. A concurrency group
shared across branches that does not cancel in progress queues every pending
run (`queue: max`): GitHub otherwise keeps one pending run per group, so a new
run silently cancels the one another branch queued."""
from pathlib import Path
import sys

import yaml


def untrusted(event):
    return event in {"pull_request", "pull_request_target", "issue_comment"} or event.startswith("pull_request_review")


def shared_group_drops_pending(concurrency):
    """True for a group shared across refs that keeps one pending run and cancels the rest."""
    if isinstance(concurrency, str):
        concurrency = {"group": concurrency}
    if not isinstance(concurrency, dict) or "group" not in concurrency:
        return False
    per_ref = "github.ref" in concurrency["group"] or "github.head_ref" in concurrency["group"]
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
    print(f"self-hosted workflow gate: {len(violations)} violation(s)")
    return int(bool(violations))


if __name__ == "__main__":
    raise SystemExit(main())
