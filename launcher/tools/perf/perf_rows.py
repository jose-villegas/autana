#!/usr/bin/env python3
"""One perf row's recent history, one line per board.

    python launcher/tools/perf/perf_rows.py "gas cascade"            every board, latest captures
    python launcher/tools/perf/perf_rows.py "gas cascade" -b 2124 -n 10   one line per capture
    python launcher/tools/perf/perf_rows.py "gas cascade" --runs     each capture's samples
    python launcher/tools/perf/perf_rows.py sync --host Legion       publish this machine's captures
    python launcher/tools/perf/perf_rows.py --eval                   score the row picker on its fixture

A row is a measurement name a capture prints: a pegged `PERF TARGET <row>:
measured N us, goal G us` line, or an unpegged one (`goal not yet pegged`,
`<row> mean Nus`, a `frame_profile:` row). Every query word must appear in
the name; a query naming several rows lists them with their capture counts.

A capture is one build on one board: every run of that build id pools into
its worst and median. Boards are never pooled or compared; each line holds
only its own board's series, and its noise is that board's own spread within
a build.

Rows come from the shared store, the `perf-data` branch of this repository
(`captures/<board>/<YYYY-MM>/<date>-<host>.jsonl`, one line per capture),
plus this machine's records not yet in it. `sync` parses new local captures
into the store and pushes it; `--store DIR` uses an existing checkout of the
branch and `--write-only` leaves the commit and push to the caller. A line holds no raw
log, path or user name: board short id, the machine name given to `--host`,
build id, commit, suite, date, image flags and each row's goal and samples.

Perf-scoped and layout-seeded images are hidden unless `--scoped` (which
shows only them); captures from before images recorded their flags show `?`.
"""
import argparse
import json
import re
import statistics
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

PERF = Path(__file__).resolve().parent
REPO = PERF.parents[2]
sys.path.insert(0, str(PERF))
sys.path.insert(0, str(REPO / "scripts/docs"))
import capture  # noqa: E402,F401  (puts scripts/device on the path)
from device_lock import short_id  # noqa: E402
from device_report import parse_perf_targets, read_capture_text, read_index, records_root  # noqa: E402
from layout_measure import timing_samples  # noqa: E402
import autana_config  # noqa: E402
import query_log  # noqa: E402

TOOL = "perf"
BRANCH = "perf-data"
CAPTURES = "captures"
# perf_budget.h's perf_goal() line for a row with no goal yet.
UNPEGGED_RE = re.compile(r"PERF TARGET (?P<name>.+?): measured (?P<measured>\d+) us, goal not yet pegged")
HOST_RE = re.compile(r"[A-Za-z][A-Za-z0-9-]{0,15}")
CAPTURE_COMMANDS = ("run-suite", "selftest")
SCOPE_FLAG = "--perf-scope"
SEED_FLAG = "--layout-seed"
COMMIT_DIGITS = 12
DATE_CHARS = len("YYYY-MM-DD")
SECOND_CHARS = len("YYYY-MM-DDTHH:MM:SS")
MONTH_CHARS = len("YYYY-MM")
DEFAULT_CAPTURES = 5
# A move past this between two captures of one board is flagged.
MOVED_PERCENT = 1.0
STALE_DAYS = 3
DEFAULT_BUDGET_TOKENS = 400
ROWS_LISTED = 10
# Bumped whenever capture_rows() reads a capture differently, so cached lines are parsed again.
PARSED_FORMAT = 1


# --- capture lines ---------------------------------------------------------

def image_flags(entries):
    """build id -> (perf scoped, layout seed) from the flash entries that name their image flags."""
    images = {}
    for entry in entries:
        flags = entry.get("image_flags")
        if entry.get("command") != "flash" or not entry.get("build_id") or flags is None:
            continue
        seed = int(flags[flags.index(SEED_FLAG) + 1]) if SEED_FLAG in flags else 0
        images[entry["build_id"]] = (SCOPE_FLAG in flags, seed)
    return images


def capture_rows(text):
    """row -> [goal or None, sample, ...] for every row the capture measures."""
    rows = {}
    targets, _ = parse_perf_targets(text)
    for target in targets:
        rows.setdefault(target["name"], [int(target["goal"])]).append(int(target["measured"]))
    for match in UNPEGGED_RE.finditer(text):
        rows.setdefault(match["name"], [None]).append(int(match["measured"]))
    for name, value in timing_samples(text):
        rows.setdefault(name, [None]).append(value)
    return rows


def capture_line(entry, images, text):
    """The store's line for one recorded capture, host aside, or None when it measured no row."""
    rows = capture_rows(text)
    if not rows:
        return None
    scope, seed = images.get(entry["build_id"], (None, None))
    return {"board": short_id(entry["board"]), "build": entry["build_id"],
            "commit": (entry.get("commit") or "")[:COMMIT_DIGITS], "suite": entry.get("suite"),
            "date": entry["started_at"][:SECOND_CHARS], "scope": scope, "seed": seed, "rows": rows}


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
    its line, or None for one that measured no row, is kept in a per-user cache."""
    cache, parsed = parsed_cache(), {}
    try:
        for text in cache.read_text(encoding="utf-8").splitlines():
            key, line = json.loads(text)
            parsed[tuple(key)] = line
    except (OSError, ValueError):
        pass
    fresh, wanted = [], set()
    for index in index_paths:
        entries = read_index(index)
        images = image_flags(entries)
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
            parsed[entry_key(entry)] = capture_line(entry, images, text)
            fresh.append([list(entry_key(entry)), parsed[entry_key(entry)]])
    if fresh:
        try:
            cache.parent.mkdir(parents=True, exist_ok=True)
            with open(cache, "a", encoding="utf-8") as stream:
                stream.writelines(json.dumps(item, separators=(",", ":")) + "\n" for item in fresh)
        except OSError:
            pass
    return [parsed[key] for key in wanted if parsed.get(key)]


# --- the store --------------------------------------------------------------

def git(folder, *args, check=True):
    return subprocess.run(["git", "-C", str(folder), *args], check=check, text=True,
                          stdin=subprocess.DEVNULL, capture_output=True).stdout.strip()


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
    return subprocess.run(["git", "-C", str(store), "ls-remote", "--exit-code", "origin", "refs/heads/" + BRANCH],
                          stdin=subprocess.DEVNULL, capture_output=True).returncode == 0


def open_store(store):
    """A checkout of the store branch at `store`, cloned from this repository's origin when absent."""
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


def sync(args):
    if not HOST_RE.fullmatch(args.host or ""):
        raise SystemExit("sync: --host takes this machine's short name (letters, digits, '-')")
    store = open_store(args.store or default_store())
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
        git(store, "-c", "user.name=perf_rows", "-c", "user.email=perf_rows@localhost", "commit", "-q",
            "-m", f"perf: {len(lines)} captures from {args.host}")
        pull(store)
        git(store, "push", "-q", "origin", "HEAD:" + BRANCH)
    print(f"sync: {len(lines)} captures in {len(touched)} files" + ("" if args.write_only else ", pushed"))
    return 0


def index_paths(args):
    folders = [Path(folder) for folder in args.records] if args.records else [records_root()]
    return [folder / "index.jsonl" for folder in folders]


# --- the answer -------------------------------------------------------------

class Capture:
    """One build on one board: every sample of one row from that build's runs."""

    def __init__(self, line):
        self.board, self.build, self.date = line["board"], line["build"], line["date"]
        self.scope, self.seed = line.get("scope"), line.get("seed")
        self.goal, self.samples, self.runs = None, [], 0

    def add(self, values):
        self.goal = values[0] if values[0] is not None else self.goal
        self.samples += values[1:]
        self.runs += 1

    @property
    def worst(self):
        return max(self.samples)

    @property
    def median(self):
        return round(statistics.median(self.samples))

    @property
    def margin(self):
        """Percent headroom of the worst sample under the goal; negative is over it."""
        return None if not self.goal else (self.goal - self.worst) * 100 / self.goal

    @property
    def spread(self):
        return (self.worst - min(self.samples)) * 100 / self.median if len(self.samples) > 1 else None


def scoped(line):
    return bool(line.get("scope")) or bool(line.get("seed"))


def matching(name, words):
    lowered = name.lower()
    return all(word in lowered for word in words)


def row_names(lines, words):
    counts = {}
    for line in lines:
        for name in line["rows"]:
            if matching(name, words):
                counts[name] = counts.get(name, 0) + 1
    return counts


def pick_row(counts, query):
    """The one row a query names: the only match, or the match spelled exactly as asked."""
    if len(counts) == 1:
        return next(iter(counts))
    exact = [name for name in counts if name.lower() == query.strip().lower()]
    return exact[0] if exact else None


def series(lines, row, board=None):
    """board -> captures of `row`, newest first."""
    boards = {}
    for line in sorted(lines, key=lambda l: l["date"]):
        if row not in line["rows"] or (board and line["board"] != board):
            continue
        captures = boards.setdefault(line["board"], {})
        captures.setdefault(line["build"], Capture(line)).add(line["rows"][row])
    return {name: sorted(builds.values(), key=lambda c: c.date, reverse=True)
            for name, builds in sorted(boards.items())}


def percent(value):
    return f"{value:+.1f}%"


def flags(captures, today):
    latest, previous = captures[0], captures[1] if len(captures) > 1 else None
    found = []
    if latest.goal is None:
        found.append("unpegged")
    elif latest.worst > latest.goal:
        found.append("OVER GOAL")
    if previous:
        moved = (latest.worst - previous.worst) * 100 / previous.worst
        if abs(moved) > MOVED_PERCENT:
            found.append("moved " + percent(moved))
        if latest.goal != previous.goal and previous.goal is not None:
            found.append("re-pegged")
    age = (today - datetime.fromisoformat(latest.date)).days
    if age > STALE_DAYS:
        found.append(f"stale {age}d")
    return found


def board_line(board, captures, today):
    latest = captures[0]
    parts = [f"{board:<5}", f"goal {latest.goal}" if latest.goal else "goal -",
             f"worst {latest.worst}"]
    if latest.margin is not None:
        parts.append(percent(latest.margin))
    before = [capture.worst for capture in captures[1:]]
    if before:
        typical = statistics.median(before)
        parts.append(f"trend {percent((latest.worst - typical) * 100 / typical)} vs {len(before)} before")
    spreads = [capture.spread for capture in captures if capture.spread is not None]
    if spreads:
        parts.append(f"noise {statistics.median(spreads):.1f}%")
    return "  ".join(parts + flags(captures, today))


def capture_line_text(capture, runs):
    scope = "?" if capture.scope is None else ("scoped" if capture.scope else "full")
    seed = "" if not capture.seed else f" seed {capture.seed}"
    margin = "" if capture.margin is None else "  " + percent(capture.margin)
    text = (f"  {capture.build}  {capture.date[:16].replace('T', ' ')}  runs {capture.runs}  "
            f"worst {capture.worst}  median {capture.median}{margin}  goal {capture.goal or '-'}  "
            f"{scope}{seed}")
    return text + ("\n    " + " ".join(map(str, capture.samples)) if runs else "")


def as_json(row, boards):
    return {"row": row, "boards": {board: [
        {"build": c.build, "date": c.date, "runs": c.runs, "goal": c.goal, "worst": c.worst,
         "median": c.median, "margin": c.margin, "samples": c.samples, "scope": c.scope, "seed": c.seed}
        for c in captures] for board, captures in boards.items()}}


class Budget:
    """Prints through the lookup until the token budget is spent, then says what was left out."""

    def __init__(self, call, tokens):
        self.call, self.limit, self.dropped = call, tokens * query_log.CHARS_PER_TOKEN, 0

    def print(self, text):
        if self.call.chars + len(text) + 1 > self.limit:
            self.dropped += 1
        else:
            self.call.print(text)

    def close(self):
        if self.dropped:
            self.call.print(f"... {self.dropped} more lines past --budget")


def answer(args, lines, out, today):
    words = args.query.lower().split()
    counts = row_names(lines, words)
    row = pick_row(counts, args.query)
    if not counts:
        known = sorted({word for line in lines for name in line["rows"] for word in name.lower().split()})
        near = sorted({n for word in words for n in query_log.nearest(word, known)})
        out.print(f"no row matches {args.query!r}" + (f"; nearest words: {', '.join(near)}" if near else ""))
        return None, {}
    if row is None:
        out.print(f"{len(counts)} rows match; name one (captures in brackets):")
        for name, count in sorted(counts.items(), key=lambda pair: -pair[1])[:ROWS_LISTED]:
            out.print(f"  {name} ({count})")
        return None, {}
    board = short_id(args.board) if args.board else None
    boards = {name: captures[:args.captures] for name, captures in series(lines, row, board).items()}
    if args.json:
        out.print(json.dumps(as_json(row, boards), separators=(",", ":")))
        return row, boards
    out.print(f"{row}  (us; margin = goal vs latest worst; noise = that board's own spread within a build)")
    for name, captures in boards.items():
        out.print(board_line(name, captures, today))
        if board or args.runs:
            for capture in captures:
                out.print(capture_line_text(capture, args.runs))
    return row, boards


def query(args):
    store = Path(args.store) if args.store else default_store()
    if args.fetch and (store / ".git").exists():
        pull(store)
    lines = store_lines(store) if store.exists() else []
    known = {line_key(line) for line in lines}
    lines += [line for line in local_lines(index_paths(args)) if line_key(line) not in known]
    lines = [line for line in lines if scoped(line) == args.scoped]
    with query_log.lookup(TOOL, "row", args.query) as call:
        out = Budget(call, args.budget)
        row, boards = answer(args, lines, out, datetime.now())
        out.close()
        call.hits = [(f"{board} {row}", None) for board in boards]
        call.weak = not boards
    return 0 if boards else 1


def evaluate(args):
    """Score the row picker over the fixture questions: rank 1 when the expected row is the answer."""
    started = time.perf_counter()
    fixture = PERF / "tests/fixtures/perf_rows"
    lines = store_lines(fixture / "store")
    ranks = []
    for text in (fixture / "eval.tsv").read_text(encoding="utf-8").splitlines():
        question, expected = text.split("\t")
        counts = row_names(lines, question.lower().split())
        picked = pick_row(counts, question)
        listed = sorted(counts, key=lambda name: -counts[name])
        rank = 1 if picked == expected else (listed.index(expected) + 1 if expected in listed else None)
        ranks.append((question, rank))
    query_log.keep_score(TOOL, "lexical", ranks, started)
    return 0


def main(argv=None):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    argv = sys.argv[1:] if argv is None else argv
    parser = argparse.ArgumentParser(prog="perf_rows", description=__doc__.split("\n")[0])
    parser.add_argument("--store", help="checkout of the perf-data branch (default: a clone in the user cache)")
    parser.add_argument("--records", action="append",
                        help="records folder holding index.jsonl (repeatable; default: this checkout's)")
    if argv[:1] == ["sync"]:
        parser.add_argument("sync")
        parser.add_argument("--host", required=True, help="this machine's short name, e.g. Legion")
        parser.add_argument("--write-only", action="store_true",
                            help="write the files only; the caller commits and pushes")
        parser.add_argument("--dry-run", action="store_true", help="count new captures, write nothing")
        return sync(parser.parse_args(argv))
    if argv[:1] == ["--eval"]:
        return evaluate(parser.parse_args(argv[1:]))
    parser.add_argument("query", help="words of the row's name")
    parser.add_argument("-b", "--board", help="one board's short id or serial: one line per capture")
    parser.add_argument("-n", "--captures", type=int, default=DEFAULT_CAPTURES, help="captures per board")
    parser.add_argument("--runs", action="store_true", help="each capture's samples")
    parser.add_argument("--scoped", action="store_true", help="only perf-scoped or layout-seeded images")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--fetch", action="store_true", help="pull the store before answering")
    parser.add_argument("--budget", type=int, default=DEFAULT_BUDGET_TOKENS, help="output cap in tokens")
    return query(parser.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
