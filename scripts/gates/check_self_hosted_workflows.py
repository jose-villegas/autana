"""Reject untrusted event triggers in workflows containing self-hosted jobs."""
from pathlib import Path
import sys

import yaml


BOARD_CI_CONDITION = (
    "github.event_name != 'pull_request_target' || "
    "(github.event.label.name == 'board-ci' && "
    "github.event.pull_request.head.repo.full_name == github.repository)"
)


def untrusted(event):
    return event in {"pull_request", "pull_request_target", "issue_comment"} or event.startswith("pull_request_review")


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
        for name, job in workflow.get("jobs", {}).items():
            runner = job.get("runs-on", [])
            labels = [runner] if isinstance(runner, str) else runner
            forbidden = blocked
            condition = " ".join(str(job.get("if", "")).split())
            labeled_only = (isinstance(events, dict)
                            and events.get("pull_request_target") == {"types": ["labeled"]})
            if labeled_only and condition == BOARD_CI_CONDITION:
                forbidden = [event for event in blocked if event != "pull_request_target"]
            if "self-hosted" in labels and forbidden:
                violations.append(f"{path}: job {name} uses self-hosted with forbidden triggers: {', '.join(forbidden)}")
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
