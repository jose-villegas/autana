#!/usr/bin/env python3
"""Board captures parsed into one shared store: the `perf-data` branch.

    python launcher/tools/perf/perf_index.py sync --host <machine>   publish this machine's captures
    python launcher/tools/perf/perf_index.py store --path DIR        check out the store branch

`sync` parses every recorded capture not yet in the store into
`captures/<board>/<YYYY-MM>/<date>-<host>.jsonl`, one line per capture, and
pushes. A row is a measurement name a capture prints: a pegged `PERF TARGET
<row>: measured N us, goal G us` line, an unpegged one (`goal not yet
pegged`), a `<row> mean Nus` line or a `frame_profile:` row.

A line holds no raw log, path or user name: board short id, the machine name
given to `--host`, build id, commit, suite, start time, image flags and each
row's goal and samples. The branch is as public as the repository. Each
machine writes its own files, so two machines never conflict, and one sync
per store runs at a time. `--store DIR` uses an existing checkout of the
branch and `--write-only` leaves the commit and push to the caller.
`--hook` is for `lock_hook`: on a `released` event only, it starts the sync
detached and returns, since a hook is stopped after seconds.

Each capture is parsed once; parsed lines are cached in the per-user autana
cache, and image flags join at read time from every index's flashes.
"""
import argparse
import contextlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

PERF = Path(__file__).resolve().parent
REPO = PERF.parents[2]
sys.path.insert(0, str(PERF))
sys.path.insert(0, str(REPO / "scripts/device"))
from device_lock import short_id  # noqa: E402
from device_report import parse_perf_targets, parse_unpegged_targets, read_capture_text, read_index, records_root  # noqa: E402
from layout_measure import timing_samples  # noqa: E402
from nightly.scoped_chain import git  # noqa: E402
import autana_config  # noqa: E402

BRANCH = "perf-data"
CAPTURES = "captures"
HOST_RE = re.compile(r"[A-Za-z][A-Za-z0-9-]{0,15}")
CAPTURE_COMMANDS = ("run-suite", "selftest")
COMMIT_DIGITS = 12
DATE_CHARS = len("YYYY-MM-DD")
SECOND_CHARS = len("YYYY-MM-DDTHH:MM:SS")
MONTH_CHARS = len("YYYY-MM")
# Bumped whenever capture_rows() reads a capture differently, so cached lines are parsed again.
PARSED_FORMAT = 2
# `git ls-remote --exit-code` status for a ref the remote does not have.
LS_REMOTE_ABSENT = 2
# Longer than any sync takes; a lock this old belongs to a sync that died.
STORE_LOCK_SECONDS = 600
LOCK_EVENT_VAR = "AUTANA_LOCK_EVENT"
RELEASED = "released"


# --- capture lines ---------------------------------------------------------

def image_flags(entries):
    """build id -> the flags its flash built with, for the flash entries that record them."""
    return {entry["build_id"]: entry["image_flags"] for entry in entries
            if entry.get("command") == "flash" and entry.get("build_id")
            and entry.get("image_flags") is not None}


def capture_rows(text):
    """row -> [goal or None, sample, ...] for every row the capture measures."""
    rows = {}
    targets, _ = parse_perf_targets(text)
    for target in targets:
        rows.setdefault(target["name"], [int(target["goal"])]).append(int(target["measured"]))
    for target in parse_unpegged_targets(text):
        rows.setdefault(target["name"], [None]).append(int(target["measured"]))
    for name, value in timing_samples(text):
        rows.setdefault(name, [None]).append(value)
    return rows


def capture_line(entry, text):
    """The store's line for one recorded capture, host and image aside, or None when it measured no row."""
    rows = capture_rows(text)
    if not rows:
        return None
    return {"board": short_id(entry["board"]), "build": entry["build_id"],
            "commit": (entry.get("commit") or "")[:COMMIT_DIGITS], "suite": entry.get("suite"),
            "date": entry["started_at"][:SECOND_CHARS], "rows": rows}


def recorded(entry):
    return (entry.get("command") in CAPTURE_COMMANDS and entry.get("reason") == "complete"
            and entry.get("board") and entry.get("build_id") and entry.get("started_at"))


def line_key(line):
    return line["board"], line["date"], line["suite"]


def entry_key(entry):
    return short_id(entry["board"]), entry["started_at"][:SECOND_CHARS], entry.get("suite")


def parsed_cache():
    return autana_config.user_cache_dir() / "perf-rows" / f"parsed-{PARSED_FORMAT}.jsonl"


def local_lines(index_paths):
    """Lines for this machine's recorded captures. Each capture is parsed once:
    its line, or None for one that measured no row, is kept in a per-user cache.
    Image flags join afterwards from every index's flashes, so no capture keeps a stale `?`."""
    cache, parsed = parsed_cache(), {}
    try:
        for text in cache.read_text(encoding="utf-8").splitlines():
            key, line = json.loads(text)
            parsed[tuple(key)] = line
    except (OSError, ValueError):
        pass
    fresh, wanted, images = [], set(), {}
    for index in index_paths:
        entries = read_index(index)
        images.update(image_flags(entries))
        for entry in entries:
            if not recorded(entry):
                continue
            wanted.add(entry_key(entry))
            if entry_key(entry) in parsed:
                continue
            try:
                text = read_capture_text(entry["capture_path"])
            except OSError:
                continue
            parsed[entry_key(entry)] = capture_line(entry, text)
            fresh.append([list(entry_key(entry)), parsed[entry_key(entry)]])
    if fresh:
        try:
            cache.parent.mkdir(parents=True, exist_ok=True)
            with open(cache, "a", encoding="utf-8") as stream:
                stream.writelines(json.dumps(item, separators=(",", ":")) + "\n" for item in fresh)
        except OSError:
            pass
    return [dict(parsed[key], image=images.get(parsed[key]["build"])) for key in wanted if parsed.get(key)]


# --- the store --------------------------------------------------------------

def default_store():
    return autana_config.user_cache_dir() / BRANCH


def store_lines(store):
    lines = []
    for path in sorted((Path(store) / CAPTURES).glob("*/*/*.jsonl")):
        for text in path.read_text(encoding="utf-8").splitlines():
            try:
                lines.append(json.loads(text))
            except ValueError:
                continue
    return lines


def store_path(store, line):
    return (Path(store) / CAPTURES / line["board"] / line["date"][:MONTH_CHARS]
            / f"{line['date'][:DATE_CHARS]}-{line['host']}.jsonl")


def write_lines(store, lines):
    """Append lines to their per-board, per-day, per-host files; returns the files touched."""
    touched = set()
    for line in sorted(lines, key=lambda l: l["date"]):
        path = store_path(store, line)
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "a", encoding="utf-8", newline="\n") as stream:
            stream.write(json.dumps(line, separators=(",", ":"), sort_keys=True) + "\n")
        touched.add(path)
    return touched


def remote_has_branch(store):
    try:
        git(store, "ls-remote", "--exit-code", "origin", "refs/heads/" + BRANCH)
    except subprocess.CalledProcessError as error:
        if error.returncode == LS_REMOTE_ABSENT:
            return False
        raise
    return True


def open_store(store):
    """A checkout of the store branch at `store`, cloned from this repository's origin when absent.
    Credentials come from git's own configuration, e.g. GIT_CONFIG_COUNT in a CI step."""
    store = Path(store)
    if (store / ".git").exists():
        return store
    store.mkdir(parents=True, exist_ok=True)
    git(store, "init", "-q")
    git(store, "remote", "add", "origin", git(REPO, "remote", "get-url", "origin"))
    if remote_has_branch(store):
        git(store, "fetch", "-q", "origin", BRANCH)
        git(store, "checkout", "-q", "-b", BRANCH, "FETCH_HEAD")
    else:
        git(store, "checkout", "-q", "--orphan", BRANCH)
    return store


def pull(store):
    if remote_has_branch(store):
        git(store, "pull", "-q", "--no-rebase", "--no-edit", "origin", BRANCH)


@contextlib.contextmanager
def store_lock(store):
    """One sync per store at a time; a lock older than STORE_LOCK_SECONDS is a dead sync's and is taken over."""
    lock = Path(store) / ".git" / "perf_index.lock"
    try:
        if time.time() - lock.stat().st_mtime > STORE_LOCK_SECONDS:
            lock.unlink()
    except OSError:
        pass
    try:
        os.close(os.open(lock, os.O_CREAT | os.O_EXCL))
    except FileExistsError:
        yield False
        return
    try:
        yield True
    finally:
        lock.unlink(missing_ok=True)


def hook(argv):
    """`lock_hook` entry: on `released` only, start the sync detached, since a hook is killed after seconds."""
    if os.environ.get(LOCK_EVENT_VAR) != RELEASED:
        return 0
    command = [sys.executable, str(Path(__file__).resolve())] + [word for word in argv if word != "--hook"]
    detached = ({"creationflags": subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP}
                if os.name == "nt" else {"start_new_session": True})
    subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, close_fds=True, **detached)
    return 0


def sync(args):
    if not HOST_RE.fullmatch(args.host or ""):
        raise SystemExit("sync: --host takes this machine's short name (letters, digits, '-')")
    store = open_store(args.store or default_store())
    with store_lock(store) as held:
        if not held:
            print("sync: another sync holds this store")
            return 0
        return sync_held(args, store)


def sync_held(args, store):
    if not args.write_only:
        pull(store)
    known = {line_key(line) for line in store_lines(store)}
    lines = [dict(line, host=args.host) for line in local_lines(index_paths(args))
             if line_key(line) not in known]
    if args.dry_run:
        print(f"store {store} ({git(store, 'remote', 'get-url', 'origin')}): {len(lines)} new captures")
        return 0
    touched = write_lines(store, lines)
    if not touched:
        print("sync: nothing new")
        return 0
    if not args.write_only:
        git(store, "add", CAPTURES)
        git(store, "-c", "user.name=perf_index", "-c", "user.email=perf_index@localhost", "commit", "-q",
            "-m", f"perf: {len(lines)} captures from {args.host}")
        pull(store)
        git(store, "push", "-q", "origin", "HEAD:" + BRANCH)
    print(f"sync: {len(lines)} captures in {len(touched)} files" + ("" if args.write_only else ", pushed"))
    return 0


def index_paths(args):
    folders = [Path(folder) for folder in args.records] if args.records else [records_root()]
    return [folder / "index.jsonl" for folder in folders]


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    parser = argparse.ArgumentParser(prog="perf_index", description=__doc__.split("\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    store = commands.add_parser("store", help="check the store branch out")
    store.add_argument("--path", required=True, help="folder for the checkout")
    sync_parser = commands.add_parser("sync", help="publish this machine's new captures")
    sync_parser.add_argument("--store", help="checkout of the perf-data branch (default: a clone in the user cache)")
    sync_parser.add_argument("--records", action="append",
                             help="records folder holding index.jsonl (repeatable; default: this checkout's)")
    sync_parser.add_argument("--host", required=True, help="this machine's short name")
    sync_parser.add_argument("--write-only", action="store_true",
                             help="write the files only; the caller commits and pushes")
    sync_parser.add_argument("--dry-run", action="store_true", help="count new captures, write nothing")
    sync_parser.add_argument("--hook", action="store_true", help="for lock_hook: on `released` only, sync detached")
    args = parser.parse_args(argv)
    if args.command == "store":
        open_store(args.path)
        return 0
    if args.hook:
        return hook(argv)
    return sync(args)


if __name__ == "__main__":
    sys.exit(main())
