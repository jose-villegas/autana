#!/usr/bin/env python3
"""Why is a file, a line range or a name the way it is: the pull requests behind it.

    python scripts/lookup/why.py <path>              the file (or folder) as it is now
    python scripts/lookup/why.py <path>:<a>[-<b>]    these lines
    python scripts/lookup/why.py <name>              every line that names it
    python scripts/lookup/why.py --eval              ranking score over why_eval.tsv

Options: --rev REV (HEAD), --history N (5), --budget CHARS (2000), --json,
--offline, --usage [--days N].

Every line that exists now is blamed with `git blame --first-parent`, which
lands it on the main-line commit that brought it in: a squash commit
`subject (#N)` or a merge commit `Merge pull request #N`, so one pull
request. The pull requests that own a line now come newest first; for a
name, the one that first added it joins them. A folder
counts the lines each pull request changed in it instead, since blaming every
file would be slow. Pull requests that touched it but own no line now
follow as a one-line history.

Each of the first FULL_ROWS rows adds the pull request's first paragraph
(`gh`, cached per pull request in the per-user autana cache, since a merged
pull request does not change) and the tickets its subject or `Ticket:` line
names, each with its bd status and title. --offline reads only the cache.
Nothing is guessed: the same history and cache give the same answer.
Every lookup is logged locally (scripts/docs/query_log.py).
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "docs"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
import autana_config  # noqa: E402
import query_log  # noqa: E402

TOOL = "why"
FULL_ROWS = 5
TICKETS_PER_ROW = 3
SUMMARY_CHARS = 280
REASON_CHARS = 60
# A name on more lines than this is too common to blame line by line.
NAME_LINES_BLAMED = 400
SITES_SHOWN = 3
# bd waits on other writers of the shared database; past this the notes are left out.
BD_SECONDS = 30
EVAL_DEPTH = 10
SITE_ORDER = {".h": 0, ".c": 1, ".py": 1}
EVAL_FILE = Path(__file__).with_name("why_eval.tsv")

SQUASH = re.compile(r"\s*\(#(\d+)\)$")
MERGE = re.compile(r"^Merge pull request #(\d+)\b")
# The last parenthesis of a subject names its tickets when every word in it is an id or
# a milestone: "(autana-ks4hn)", "(hacgs)", "(cvt.16 M1)", but not "(cached bakes)".
SUBJECT_TICKETS = re.compile(r"\(([^()]+)\)$")
MILESTONE = re.compile(r"^M\d+[a-z]?$")
TICKET_LINE = re.compile(r"^Ticket:\s*(.+)$", re.MULTILINE)
TICKET_ID = re.compile(r"^(?:autana-)?(?:[a-z0-9]{4,5}|[a-z0-9]{3}(?=\.))(?:\.\d+)*$")
RANGE = re.compile(r"^(.+):(\d+)(?:-(\d+))?$")


def git(root, *args, check=True):
    done = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if check and done.returncode:
        raise SystemExit(f"git {' '.join(args)}: {done.stderr.strip()}")
    return done.stdout


class Commit:
    """One main-line commit: the pull request it merged, when it has one."""

    def __init__(self, sha, date, subject, body):
        self.sha, self.date = sha, date
        squash, merge = SQUASH.search(subject), MERGE.match(subject)
        self.pr = int((squash or merge).group(1)) if squash or merge else None
        # A merge commit's subject names the branch; its body's first line is the PR title.
        self.title = (body.strip().splitlines() or [subject])[0] if merge else SQUASH.sub("", subject)
        named = SUBJECT_TICKETS.search(self.title)
        words = named.group(1).replace(",", " ").split() if named else []
        ids = all(TICKET_ID.match(w) or MILESTONE.match(w) for w in words)
        self.tickets = [w for w in words if TICKET_ID.match(w)] if ids else []


def commits(root, shas):
    """Commit by full sha, for every sha asked."""
    if not shas:
        return {}
    out = git(root, "show", "-s", "--format=%H%x1f%ad%x1f%s%x1f%b%x1e", "--date=short", *sorted(shas))
    found = {}
    for record in out.split("\x1e"):
        parts = record.strip("\n").split("\x1f")
        if len(parts) == 4:
            found[parts[0]] = Commit(*parts)
    return found


def history(root, rev, path=None, name=None):
    """Main-line commits that changed the path, or the count of the name, newest first."""
    args = ["log", "--first-parent", "--format=%H", rev]
    if name:
        args[3:3] = ["-S", name]
    if path:
        args += (["--follow"] if Path(path).suffix else []) + ["--", path]
    return [line for line in git(root, *args, check=False).splitlines() if line]


def blame(root, rev, path, ranges=()):
    """Lines owned now, per main-line sha."""
    args = ["blame", "--first-parent", "-w", "-M", "--porcelain"]
    for start, end in ranges:
        args += ["-L", f"{start},{end}"]
    out = git(root, *args, rev, "--", path, check=False)
    owned = Counter()
    for line in out.splitlines():
        head = line.split(" ")
        # A porcelain header is "<sha> <orig line> <final line> [<group size>]".
        if len(head) >= 3 and len(head[0]) == 40 and head[1].isdigit():
            owned[head[0]] += 1
    return owned


def changed(root, rev, path):
    """Lines added plus removed under a folder, per main-line sha."""
    out = git(root, "log", "--first-parent", "--format=@%H", "--numstat", rev, "--", path)
    owned, sha = Counter(), None
    for line in out.splitlines():
        if line.startswith("@"):
            sha = line[1:]
        elif line.strip():
            added, removed = line.split("\t")[:2]
            owned[sha] += int(added) + int(removed) if added.isdigit() and removed.isdigit() else 1
    return owned


def name_sites(root, rev, name):
    """(path, line) of every tracked line that names it as a whole word."""
    out = git(root, "grep", "-n", "-w", "-F", "-I", "-e", name, rev, check=False)
    sites = []
    for line in out.splitlines():
        # rev:path:line:text
        _, path, number, _ = line.split(":", 3)
        sites.append((path, int(number)))
    # Code before prose, so the declaring header leads the shown sites.
    return sorted(sites, key=lambda site: (SITE_ORDER.get(Path(site[0]).suffix, len(SITE_ORDER)), site))


def kind_of(root, rev, query):
    """('range', path, [(a, b)]), ('file', path), ('folder', path) or ('name', query)."""
    def object_type(path):
        done = subprocess.run(["git", "-C", str(root), "cat-file", "-t", f"{rev}:{path}"],
                              capture_output=True, text=True)
        return done.stdout.strip() if done.returncode == 0 else None

    query = query.replace("\\", "/")
    found = RANGE.match(query)
    if found and object_type(found.group(1)) == "blob":
        start = int(found.group(2))
        return ("range", found.group(1), [(start, int(found.group(3) or start))])
    kind = object_type(query.rstrip("/"))
    if kind == "blob":
        return ("file", query)
    if kind == "tree":
        return ("folder", query.rstrip("/"))
    return ("name", query)


def rank(root, rev, query):
    """The answer before any pull request text: target, ranked rows, history tail, sites."""
    kind = kind_of(root, rev, query)
    sites, introduced = [], None
    if kind[0] in ("file", "range"):
        owned = blame(root, rev, kind[1], kind[2] if kind[0] == "range" else ())
        touched = history(root, rev, path=kind[1])
    elif kind[0] == "folder":
        owned = changed(root, rev, kind[1])
        touched = list(owned)
    else:
        sites = name_sites(root, rev, query)
        owned = Counter()
        by_file = {}
        for path, line in sites[:NAME_LINES_BLAMED]:
            by_file.setdefault(path, []).append((line, line))
        for path, lines in by_file.items():
            owned.update(blame(root, rev, path, lines))
        touched = history(root, rev, name=query)
        introduced = touched[-1] if touched else None
    known = commits(root, set(owned) | set(touched))
    # Newest first: in the eval it found the answer sooner than most lines first.
    alive = sorted(owned, key=lambda sha: (known[sha].date, owned[sha], sha), reverse=True)
    if introduced and introduced not in alive:
        alive.append(introduced)
    tail = [sha for sha in touched if sha not in owned and sha != introduced]
    return {"kind": kind[0], "target": query, "lines": sum(owned.values()),
            "sites": sites, "rows": [(known[s], owned.get(s, 0)) for s in alive],
            "history": [known[s] for s in tail]}


def pr_cache():
    return autana_config.user_cache_dir() / "why" / "pr"


def pr_text(root, number, offline):
    """{title, body} of a pull request from the cache, else gh (then cached); None when unknown."""
    cached = pr_cache() / f"{number}.json"
    try:
        return json.loads(cached.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        pass
    if offline:
        return None
    try:
        done = subprocess.run([shutil.which("gh") or "gh", "pr", "view", str(number), "--json", "title,body,state"],
                              cwd=root, capture_output=True, text=True, encoding="utf-8", timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if done.returncode:
        return None
    text = json.loads(done.stdout)
    if text.get("state") == "MERGED":
        try:
            cached.parent.mkdir(parents=True, exist_ok=True)
            cached.write_text(json.dumps(text), encoding="utf-8")
        except OSError:
            pass
    return text


def summary(body):
    """The body's first paragraph of prose, cut at a sentence end within SUMMARY_CHARS."""
    for paragraph in re.split(r"\n\s*\n", body or ""):
        text = " ".join(paragraph.split())
        if not text or text[0] in "#|<>-*" or text.startswith("```") or TICKET_LINE.match(text):
            continue
        if len(text) <= SUMMARY_CHARS:
            return text
        cut = text[:SUMMARY_CHARS]
        stop = max(cut.rfind(". "), cut.rfind("; "))
        return (cut[:stop + 1] if stop > SUMMARY_CHARS // 2 else cut.rstrip() + " ...")
    return ""


def tickets(ids):
    """id -> {id, status, reason, title} for the ids bd knows, in one bd call."""
    ids = list(dict.fromkeys(ids))
    if not ids:
        return {}
    try:
        done = subprocess.run([shutil.which("bd") or "bd", "--readonly", "show", *ids, "--json"],
                              capture_output=True, text=True, encoding="utf-8", errors="replace",
                              timeout=BD_SECONDS)
        issues = json.loads(done.stdout) if done.stdout.strip() else []
    except (OSError, ValueError, subprocess.TimeoutExpired):
        issues = []
    found = {}
    for issue in issues:
        note = {"id": issue["id"], "status": issue.get("status", ""),
                "reason": (issue.get("close_reason") or "").strip(), "title": issue.get("title", "")}
        found[issue["id"]] = found[issue["id"].removeprefix("autana-")] = note
    return found


def answer(root, rev, query, offline=False, with_tickets=True):
    found = rank(root, rev, query)
    rows = []
    for i, (commit, lines) in enumerate(found["rows"]):
        row = {"pr": commit.pr, "commit": commit.sha[:9], "date": commit.date, "lines": lines,
               "title": commit.title, "tickets": commit.tickets}
        if i < FULL_ROWS and commit.pr:
            text = pr_text(root, commit.pr, offline)
            if text:
                row["summary"] = summary(text.get("body"))
                named = TICKET_LINE.search(text.get("body") or "")
                if named:
                    row["tickets"] += [w for w in named.group(1).replace(",", " ").split()
                                       if TICKET_ID.match(w) and w not in row["tickets"]]
        rows.append(row)
    if with_tickets:
        ids = [t for row in rows[:FULL_ROWS] for t in row["tickets"][:TICKETS_PER_ROW]]
        known = tickets(ids)
        for row in rows[:FULL_ROWS]:
            row["ticket_notes"] = [known[t] for t in row["tickets"][:TICKETS_PER_ROW] if t in known]
    return {"kind": found["kind"], "target": query, "lines": found["lines"],
            "sites": first_per_file(found["sites"])[:SITES_SHOWN],
            "site_count": len(found["sites"]), "rows": rows,
            "history": [{"pr": c.pr, "commit": c.sha[:9], "date": c.date, "title": c.title}
                        for c in found["history"]]}


def first_per_file(sites):
    first = {}
    for path, line in sites:
        first.setdefault(path, (path, line))
    return list(first.values())


def label(row):
    return f"#{row['pr']}" if row["pr"] else row["commit"]


def render(reply, budget, history_rows):
    """The answer as text lines, cut to budget characters."""
    target, rows = reply["target"], reply["rows"]
    owning = "changed" if reply["kind"] == "folder" else "own"
    noun = "lines changed" if reply["kind"] == "folder" else "lines"
    head = f"{target}  {len(rows)} PRs {owning} these {reply['lines']} {noun}"
    if reply["kind"] == "name":
        head += f" ({reply['site_count']} lines name it)"
    lines = [head]
    lines += [f"  {path}:{line}" for path, line in reply["sites"]]
    width = len(str(reply["lines"]))
    shown = 0
    for i, row in enumerate(rows):
        block = [f"{label(row)} {row['date']}  {row['lines']:>{width}}/{reply['lines']}  {row['title']}"]
        if i < FULL_ROWS:
            if row.get("summary"):
                block.append(f"    {row['summary']}")
            for note in row.get("ticket_notes", []):
                reason = f" ({note['reason'][:REASON_CHARS]})" if note["reason"] else ""
                block.append(f"    {note['id']}  {note['status']}{reason}  {note['title']}")
        if sum(len(x) + 1 for x in lines + block) > budget:
            break
        lines += block
        shown += 1
    tail = [f"{label(h)} {h['date']} {h['title']}" for h in reply["history"][:history_rows]]
    if tail and shown == len(rows):
        line = "history: " + "; ".join(tail)
        if sum(len(x) + 1 for x in lines) + len(line) <= budget:
            lines.append(line)
    hidden = len(rows) - shown + max(len(reply["history"]) - (history_rows if shown == len(rows) else 0), 0)
    if hidden > 0:
        lines.append(f"+{hidden} more (--budget, --history or --json)")
    return lines


def load_eval():
    rows = []
    for line in EVAL_FILE.read_text(encoding="utf-8").splitlines():
        if line.strip() and not line.startswith("# "):
            query, expected, asked = line.split("\t")
            rows.append((query, expected, asked))
    return rows


def eval_rank(reply, expected):
    """1-based row of the expected pull request (#N) or ticket, None when absent."""
    ordered = reply["rows"] + reply["history"]
    ticket = expected.removeprefix("autana-")
    for i, row in enumerate(ordered[:EVAL_DEPTH], 1):
        if expected.startswith("#") and row["pr"] == int(expected[1:]):
            return i
        if not expected.startswith("#") and ticket in {t.removeprefix("autana-") for t in row.get("tickets", [])}:
            return i
    return None


def run_eval(root, rev):
    started = time.perf_counter()
    ranks = []
    for query, expected, asked in load_eval():
        # Answered on the main line as it stood when the question was asked.
        then = git(root, "rev-list", "-1", "--first-parent", f"--before={asked}", rev).strip()
        reply = answer(root, then, query, offline=True, with_tickets=False)
        found = eval_rank(reply, expected)
        print(f"{found or '-':>2}  {expected:<14} {query}")
        ranks.append((query, found))
    return query_log.keep_score(TOOL, "offline", ranks, started)


def repo_root():
    """The checkout the command runs in, so any clone answers for its own history."""
    out = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    if out.returncode:
        raise SystemExit("why.py runs inside a git checkout")
    return Path(out.stdout.strip())


def main(argv=None, root=None):
    parser = argparse.ArgumentParser(prog="why", description=__doc__.split("\n")[0])
    parser.add_argument("query", nargs="?")
    parser.add_argument("--rev", default="HEAD")
    parser.add_argument("--history", type=int, default=5)
    parser.add_argument("--budget", type=int, default=2000)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--eval", action="store_true")
    parser.add_argument("--usage", action="store_true")
    parser.add_argument("--days", type=int, default=14)
    args = parser.parse_args(argv)
    # PR text carries any Unicode; a Windows console or pipe may not.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if args.usage:
        return query_log.main(["--days", str(args.days), "--tool", TOOL])
    root = root or repo_root()
    if args.eval:
        run_eval(root, args.rev)
        return 0
    if not args.query:
        parser.error("give a path, path:line or name")
    with query_log.lookup(TOOL, "why", args.query) as call:
        reply = answer(root, args.rev, args.query, offline=args.offline)
        call.hits = [(label(r), r["lines"]) for r in reply["rows"]]
        call.weak = not reply["rows"] and not reply["history"]
        if args.json:
            call.print(json.dumps(reply, indent=1))
        elif call.weak:
            names = git(root, "ls-files", "--", ".").splitlines()
            near = query_log.nearest(Path(args.query).name, sorted({Path(n).name for n in names}))
            call.print(f"{args.query}: no line or commit on {args.rev} matches"
                       + (f"; nearest files: {', '.join(near)}" if near else ""))
        else:
            for line in render(reply, args.budget, args.history):
                call.print(line)
    return 0 if reply["rows"] or reply["history"] else 1


if __name__ == "__main__":
    sys.exit(main())
