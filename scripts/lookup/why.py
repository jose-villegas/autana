#!/usr/bin/env python3
"""Why is a file, a line range or a name the way it is: the pull requests behind it.

    python scripts/lookup/why.py <path>              the file (or folder) as it is now
    python scripts/lookup/why.py <path>:<a>[-<b>]    these lines
    python scripts/lookup/why.py <name>              every line that names it
    python scripts/lookup/why.py --eval              ranking score over why_eval.tsv

Options: --rev REV, --history N, --budget CHARS, --json, --offline,
--usage [--days N].

Each line is blamed to the main-line commit, so the pull request, that
brought it in; the first rows add the pull request's first paragraph (gh,
cached) and its tickets (bd). docs/tools/Why.md has the sources and order.
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
# bd waits on other writers of the shared database, gh on the network; past
# these the notes or the description are left out.
BD_SECONDS = 30
GH_SECONDS = 30
EVAL_DEPTH = 10
SITE_ORDER = {".h": 0, ".c": 1, ".py": 1}
EVAL_FILE = Path(__file__).with_name("why_eval.tsv")

SQUASH = re.compile(r"\s*\(#(\d+)\)$")
MERGE = re.compile(r"^Merge pull request #(\d+)\b")
# The last parenthesis of a subject names its tickets when every word in it is an id or
# a milestone: "(autana-ks4hn)", "(hacgs)", "(cvt.16 M1)", but not "(cached bakes)".
SUBJECT_TICKETS = re.compile(r"\(([^()]+)\)$")
MILESTONE = re.compile(r"^M\d+[a-z]?$")
# Subjects are not gated, so their ids are held to bd's shape: 4-5 characters, or 3 and a child number.
TICKET_ID = re.compile(r"^(?:autana-)?(?:[a-z0-9]{4,5}|[a-z0-9]{3}(?=\.))(?:\.\d+)*$")
# A description's one ticket, in the grammar scripts/gates/check-pr-ticket.sh enforces
# (a body typed on GitHub keeps CRLF line ends).
TICKET_LINE = re.compile(r"^Ticket:[ \t]*((?:autana-)?[a-z0-9]+(?:\.[0-9]+)*)[ \t\r]*$", re.MULTILINE)
RANGE = re.compile(r"^(.+):(\d+)(?:-(\d+))?$")


def git(root, *args, check=True):
    done = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True,
                          encoding="utf-8", errors="replace")
    if check and done.returncode:
        raise SystemExit(f"git {' '.join(args)}: {done.stderr.strip()}")
    return done.stdout


class Commit:
    """One main-line commit: the pull request it merged, when it has one."""

    def __init__(self, sha, seconds, date, subject, body):
        self.sha, self.seconds, self.date = sha, int(seconds), date
        squash, merge = SQUASH.search(subject), MERGE.match(subject)
        self.pr = int((squash or merge).group(1)) if squash or merge else None
        # A merge commit's subject names the branch; its body's first line is the PR title.
        self.title = (body.strip().splitlines() or [subject])[0] if merge else SQUASH.sub("", subject)
        named = SUBJECT_TICKETS.search(self.title)
        words = named.group(1).replace(",", " ").split() if named else []
        ids = all(TICKET_ID.match(w) or MILESTONE.match(w) for w in words)
        self.tickets = [w for w in words if TICKET_ID.match(w)] if ids else []

    def row(self):
        return {"pr": self.pr, "commit": self.sha[:9], "date": self.date, "title": self.title,
                "tickets": list(self.tickets)}


def commits(root, shas):
    """Commit by full sha, for every sha asked."""
    if not shas:
        return {}
    out = git(root, "show", "-s", "--format=%H%x1f%ct%x1f%ad%x1f%s%x1f%b%x1e", "--date=short", *sorted(shas))
    found = {}
    for record in out.split("\x1e"):
        parts = record.strip("\n").split("\x1f")
        if len(parts) == 5:
            found[parts[0]] = Commit(*parts)
    return found


def history(root, rev, *selector):
    """Main-line commits, newest first, that a git log selector picks:
    ("--follow", "--", path), ("-L<a>,<b>:<path>",) or ("-S", name)."""
    options, paths = (selector[:-2], selector[-2:]) if "--" in selector else (selector, ())
    out = git(root, "log", "--first-parent", "-s", "--format=%H", *options, rev, *paths, check=False)
    return [line for line in out.splitlines() if line]


def blame(root, rev, path, ranges=()):
    """Lines owned now, per main-line sha."""
    args = ["blame", "--first-parent", "-w", "-M", "--porcelain"]
    for start, end in ranges:
        args += ["-L", f"{start},{end}"]
    out = git(root, *args, rev, "--", path, check=False)
    owned = Counter()
    for line in out.splitlines():
        head = line.split(" ")
        # A porcelain header is "<sha> <orig line> <final line> [<group size>]"; a
        # shallow clone marks its boundary commit "^<sha>".
        sha = head[0].lstrip("^")
        if len(head) >= 3 and len(sha) == 40 and head[1].isdigit():
            owned[sha] += 1
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
    out = git(root, "grep", "-z", "-n", "-w", "-F", "-I", "-e", name, rev, check=False)
    sites = []
    for line in out.splitlines():
        # -z: rev:path NUL line NUL text, so a ':' in a path cannot split it.
        where, number, _ = line.split("\0", 2)
        sites.append((where.removeprefix(f"{rev}:"), int(number)))
    # Code before prose, so the declaring header leads the shown sites.
    return sorted(sites, key=lambda site: (SITE_ORDER.get(Path(site[0]).suffix, len(SITE_ORDER)), site))


def kind_of(root, rev, query, prefix=""):
    """('range', path, (a, b)), ('file', path), ('folder', path) or ('name', query).
    A path is tried from the folder the command runs in (prefix), then from the root."""
    def resolve(path):
        for whole in dict.fromkeys((prefix + path, path)):
            kind = git(root, "cat-file", "-t", f"{rev}:{whole}", check=False).strip()
            if kind:
                return kind, whole
        return None, path

    query = query.replace("\\", "/")
    found = RANGE.match(query)
    if found:
        kind, path = resolve(found.group(1))
        if kind == "blob":
            start = int(found.group(2))
            return ("range", path, (start, int(found.group(3) or start)))
    kind, path = resolve(query.rstrip("/"))
    if kind == "blob":
        return ("file", path)
    if kind == "tree":
        return ("folder", path)
    return ("name", query)


def rank(root, rev, query, prefix=""):
    """The answer before any pull request text: target, ranked rows, history tail, sites."""
    kind = kind_of(root, rev, query, prefix)
    sites, introduced = [], None
    if kind[0] == "file":
        owned = blame(root, rev, kind[1])
        touched = history(root, rev, "--follow", "--", kind[1])
    elif kind[0] == "range":
        owned = blame(root, rev, kind[1], [kind[2]])
        touched = history(root, rev, f"-L{kind[2][0]},{kind[2][1]}:{kind[1]}")
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
        touched = history(root, rev, "-S", query)
        introduced = touched[-1] if touched else None
    known = commits(root, set(owned) | set(touched))
    # Newest first: the change a reader asks about is usually a recent one.
    alive = sorted(owned, key=lambda sha: (known[sha].seconds, sha), reverse=True)
    if introduced and introduced not in alive:
        alive.append(introduced)
    tail = [sha for sha in touched if sha not in owned and sha != introduced]
    return {"kind": kind[0], "target": query, "lines": sum(owned.values()),
            "sites": sites, "rows": [(known[s], owned.get(s, 0)) for s in alive],
            "history": [known[s] for s in tail]}


def pr_cache():
    return autana_config.user_cache_dir() / "why" / "pr"


def repo_key(root):
    """The origin remote's owner and name, so clones of different repositories keep apart."""
    url = re.sub(r"\.git$", "", git(root, "remote", "get-url", "origin", check=False).strip())
    return "-".join(url.replace(":", "/").split("/")[-2:]) or "local"


def pr_text(root, number, offline):
    """{title, body} of a pull request from the cache, else gh (then cached); None when unknown."""
    cached = pr_cache() / repo_key(root) / f"{number}.json"
    try:
        return json.loads(cached.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        pass
    if offline:
        return None
    try:
        done = subprocess.run([shutil.which("gh") or "gh", "pr", "view", str(number), "--json", "title,body,state"],
                              cwd=root, capture_output=True, text=True, encoding="utf-8", timeout=GH_SECONDS)
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


def answer(root, rev, query, offline=False, with_tickets=True, bodies=True, prefix=""):
    """Rows, history and sites; bodies=False reads no pull request text at all."""
    found = rank(root, rev, query, prefix)
    rows = []
    for i, (commit, lines) in enumerate(found["rows"]):
        row = {**commit.row(), "lines": lines}
        text = pr_text(root, commit.pr, offline) if bodies and i < FULL_ROWS and commit.pr else None
        if text:
            row["summary"] = summary(text.get("body"))
            named = TICKET_LINE.search(text.get("body") or "")
            if named and named.group(1) not in row["tickets"]:
                row["tickets"].append(named.group(1))
        rows.append(row)
    if with_tickets:
        ids = [t for row in rows[:FULL_ROWS] for t in row["tickets"][:TICKETS_PER_ROW]]
        known = tickets(ids)
        for row in rows[:FULL_ROWS]:
            row["ticket_notes"] = [known[t] for t in row["tickets"][:TICKETS_PER_ROW] if t in known]
    return {"kind": found["kind"], "target": query, "lines": found["lines"],
            "sites": first_per_file(found["sites"])[:SITES_SHOWN],
            "site_count": len(found["sites"]), "rows": rows,
            "history": [c.row() for c in found["history"]]}


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
                reason = f" ({note['reason'][:REASON_CHARS].rstrip()})" if note["reason"] else ""
                block.append(f"    {note['id']}  {note['status']}{reason}  {note['title']}")
        if sum(len(x) + 1 for x in lines + block) > budget:
            break
        lines += block
        shown += 1
    older_shown = 0
    if shown == len(rows) and reply["history"]:
        tail = [f"{label(h)} {h['date']} {h['title']}" for h in reply["history"][:history_rows]]
        line = "history: " + "; ".join(tail)
        if sum(len(x) + 1 for x in lines) + len(line) <= budget:
            lines.append(line)
            older_shown = len(tail)
    left = [f"{n} {what}" for n, what in ((len(rows) - shown, "more PRs"),
                                         (len(reply["history"]) - older_shown, "older")) if n]
    if left:
        lines.append(f"+{' and '.join(left)} (--budget, --history or --json)")
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
        if not expected.startswith("#") and ticket in {t.removeprefix("autana-") for t in row["tickets"]}:
            return i
    return None


def run_eval(root, rev):
    started = time.perf_counter()
    ranks = []
    for query, expected, asked in load_eval():
        # Answered on the main line as it stood when the question was asked, from
        # git alone, so every machine scores the same history the same.
        then = git(root, "rev-list", "-1", "--first-parent", f"--before={asked}", rev).strip()
        reply = answer(root, then, query, with_tickets=False, bodies=False)
        found = eval_rank(reply, expected)
        print(f"{found or '-':>2}  {expected:<14} {query}")
        ranks.append((query, found))
    return query_log.keep_score(TOOL, "git", ranks, started)


def repo_root():
    """The checkout the command runs in, so any clone answers for its own history."""
    top = git(Path.cwd(), "rev-parse", "--show-toplevel", check=False).strip()
    if not top:
        raise SystemExit("why.py runs inside a git checkout")
    return Path(top)


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
    parser.add_argument("--days", type=int, default=0)
    args = parser.parse_args(argv)
    # PR text carries any Unicode; a Windows console or pipe may not.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if args.usage:
        return query_log.main(["--days", str(args.days), "--tool", TOOL])
    prefix = "" if root else git(Path.cwd(), "rev-parse", "--show-prefix", check=False).strip()
    root = root or repo_root()
    if args.eval:
        run_eval(root, args.rev)
        return 0
    if not args.query:
        parser.error("give a path, path:line or name")
    with query_log.lookup(TOOL, "why", args.query) as call:
        reply = answer(root, args.rev, args.query, offline=args.offline, prefix=prefix)
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
