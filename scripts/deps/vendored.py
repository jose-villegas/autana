#!/usr/bin/env python3
"""Keep each vendored copy taken from the commit its upstream submodule is pinned at.

third_party/vendored.json names, for each vendored directory, the submodule it comes from,
the folder inside it (`from`), files that moved on the way in (`moved`) and the upstream
commit the copy was last taken from (`taken_from`). A submodule bump moves the pin; until
the copy takes the same upstream change, `check` fails.

    python3 scripts/deps/vendored.py check    # each pin equals its copy's taken_from
    python3 scripts/deps/vendored.py update   # merge each moved upstream into its copy

`check` reads the pins from the index, so it needs no submodule checkout. `update` needs the
submodule checked out at its new pin with the old commit fetched; each upstream file changed
between the two is merged into the vendored file with `git merge-file`, so this project's own
edits stay. A file the copy cut out stays cut, and a file upstream added or deleted is listed
for a person to decide. Exits 1 when a merge conflicts or an entry needs a decision, after
recording the new commit, with the conflict markers left in the vendored file.
"""
import argparse
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
REGISTRY = ROOT / "third_party" / "vendored.json"
GITLINK_MODE = "160000"


def git(*args, cwd=ROOT, text=True):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True,
                          text=text).stdout


def load(registry=REGISTRY):
    return json.loads(registry.read_text(encoding="utf-8"))


def pinned(submodule, root=ROOT):
    """The commit the index pins `submodule` at."""
    fields = git("ls-files", "-s", "--", submodule, cwd=root).split()
    if not fields or fields[0] != GITLINK_MODE:
        sys.exit(f"{submodule} is not a submodule in the index")
    return fields[1]


def stale(entries, root=ROOT):
    """(vendored dir, entry, pinned commit) of every copy taken from another commit."""
    return [(copy, entry, commit) for copy, entry in entries.items()
            if (commit := pinned(entry["upstream"], root)) != entry["taken_from"]]


def check(entries, root=ROOT):
    behind = stale(entries, root)
    for copy, entry, commit in behind:
        print(f"{entry['upstream']} is pinned at {commit[:12]}, but {copy} was taken from "
              f"{entry['taken_from'][:12]}: run `python3 scripts/deps/vendored.py update` "
              "and commit the merged copy", file=sys.stderr)
    return 1 if behind else 0


def vendored_path(entry, name):
    """Where upstream file `name` (relative to `from`) lives in the copy."""
    return entry.get("moved", {}).get(name, name)


def show(submodule, commit, path):
    return git("show", f"{commit}:{path}", cwd=submodule, text=False)


def update_entry(copy, entry, commit, root=ROOT):
    """Merge upstream's change from taken_from to `commit` into `copy`; the lines to report."""
    submodule, base = root / entry["upstream"], entry["taken_from"]
    folder = entry["from"]
    report, clean = [], True
    changes = git("diff", "--name-status", "--no-renames", base, commit, "--", folder, cwd=submodule)
    for line in changes.splitlines():
        status, path = line.split("\t")
        name = pathlib.PurePosixPath(path).relative_to(folder).as_posix()
        target = root / copy / vendored_path(entry, name)
        if status == "A":
            report.append(f"{copy}: upstream added {path}; take it or leave it cut")
            clean = False
        elif not target.exists():
            report.append(f"{copy}: {path} changed upstream but is cut from the copy; left out")
        elif status == "D":
            report.append(f"{copy}: upstream deleted {path}; delete or keep {target.relative_to(root).as_posix()}")
            clean = False
        else:
            clean &= merge(target, show(submodule, base, path), show(submodule, commit, path), report, root)
    entry["taken_from"] = commit
    return clean, report


def merge(target, base, theirs, report, root):
    with_base, with_theirs = target.with_name(target.name + ".base"), target.with_name(target.name + ".theirs")
    try:
        with_base.write_bytes(base)
        with_theirs.write_bytes(theirs)
        result = subprocess.run(["git", "merge-file", "-L", "vendored", "-L", "upstream before",
                                 "-L", "upstream after", str(target), str(with_base), str(with_theirs)],
                                capture_output=True)
    finally:
        with_base.unlink(missing_ok=True)
        with_theirs.unlink(missing_ok=True)
    shown = target.relative_to(root).as_posix()
    if result.returncode == 0:
        report.append(f"{shown}: merged")
        return True
    report.append(f"{shown}: {result.returncode} conflict(s), resolve the markers")
    return False


def update(entries, registry=REGISTRY, root=ROOT):
    clean = True
    for copy, entry, commit in stale(entries, root):
        merged, report = update_entry(copy, entry, commit, root)
        clean &= merged
        print("\n".join(report or [f"{copy}: upstream changed nothing it vendors"]))
    registry.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8", newline="\n")
    return 0 if clean else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=("check", "update"))
    args = parser.parse_args(argv)
    entries = load()
    return check(entries) if args.command == "check" else update(entries)


if __name__ == "__main__":
    sys.exit(main())
