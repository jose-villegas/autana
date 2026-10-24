#!/usr/bin/env python3
"""The shared base of the lookup tools: a local log of every lookup, its
summary, and each tool's tracked retrieval score.

    python scripts/docs/query_log.py [--days N] [--tool NAME]   summarise the log

A call is `with lookup(tool, kind, query) as call:`; see Lookup and keep_score.

Each lookup appends one JSON line to queries.jsonl in the per-user autana
cache (%LOCALAPPDATA%/autana/usage, else $XDG_CACHE_HOME or ~/.cache, then
autana/usage). Nothing leaves the machine; deleting the folder loses only
the history. The summary reads every *.jsonl in that folder, so any tool
that writes rows in this format is summarised beside docs_search.py:

    time      ISO 8601 UTC            tool   which tool asked
    kind      what was asked for      query  the question or reference
    hits      [[label, score], ...]   ms     latency, when known
    weak      true when nothing answered well
    followed  optional: rank of the result opened next, 0 for none
    chars     optional: characters printed; the summary shows them as tokens
    session   optional: AUTANA_SESSION, else the first `session_vars` variable set

Other fields are kept but not summarised. To group lookups by session, run
the session's commands with AUTANA_SESSION=<any label> in their environment
(set per process; nothing is stored), or name in the `session_vars` setting
the variables that already label one; --session NAME then shows only that one.

A failure to write the log never fails the lookup that made it.
"""
import argparse
import contextlib
import difflib
import json
import os
import sys
import time
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
import autana_config  # noqa: E402

LOG_NAME = "queries.jsonl"
EVAL_HISTORY = "eval/history.jsonl"
HITS_KEPT = 5
SESSION_VAR = "AUTANA_SESSION"
TOP_SHOWN = 15
# Rough characters per English token; only meant to compare tools by output.
CHARS_PER_TOKEN = 4
NEAREST_SHOWN = 3
NEAREST_CUTOFF = 0.8


def log_dir():
    return autana_config.user_cache_dir() / "usage"


def iso(seconds=None):
    moment = datetime.fromtimestamp(time.time() if seconds is None else seconds, timezone.utc)
    return moment.isoformat(timespec="seconds")


def session():
    """This process's session label: AUTANA_SESSION, else the first set variable
    the `session_vars` setting names, else None."""
    names = [SESSION_VAR]
    try:
        names += autana_config.load().get("session_vars", [])
    except autana_config.ConfigError:
        pass
    return next((os.environ[name] for name in names if os.environ.get(name)), None)


def record(tool, kind, query, hits=(), started=None, **fields):
    """Append one lookup; hits are (label, score) pairs, best first."""
    row = {"time": iso(), "tool": tool, "kind": kind, "query": query,
           "hits": [[label, None if score is None else round(score, 4)]
                    for label, score in list(hits)[:HITS_KEPT]]}
    label = session()
    if label:
        row["session"] = label
    if started is not None:
        row["ms"] = round((time.perf_counter() - started) * 1000)
    row.update(fields)
    append(LOG_NAME, row)


class Lookup:
    """One lookup as it runs: what it printed, what it found, and its verdict."""

    def __init__(self):
        self.hits, self.weak, self.fields, self.chars = [], False, {}, 0

    def print(self, text="", end="\n", flush=False):
        self.chars += len(text) + len(end)
        print(text, end=end, flush=flush)


@contextlib.contextmanager
def lookup(tool, kind, query):
    """Time one lookup and log it, with what it printed, when the block ends;
    a block that raises logs nothing."""
    call, started = Lookup(), time.perf_counter()
    yield call
    record(tool, kind, query, hits=call.hits, started=started, weak=call.weak,
           chars=call.chars, **call.fields)


def nearest(word, words):
    """The indexed words closest in spelling to one that matched nothing."""
    return difflib.get_close_matches(word, words, NEAREST_SHOWN, NEAREST_CUTOFF)


def scores(ranks):
    """Mean reciprocal rank over (question, rank or None) rows, and the share
    found first and in the top three."""
    count = max(len(ranks), 1)
    return {"rows": len(ranks),
            "mrr": round(sum(1 / r for _, r in ranks if r) / count, 3),
            "top1": round(sum(1 for _, r in ranks if r == 1) / count, 3),
            "top3": round(sum(1 for _, r in ranks if r and r <= 3) / count, 3)}


def eval_history():
    return log_dir() / EVAL_HISTORY


def keep_score(tool, mode, ranks, started, **fields):
    """Score one tool's evaluation, print it beside the last comparable run of
    that tool, and keep it in the history every checkout shares."""
    measured = scores(ranks)
    if set(fields) & set(measured):
        raise ValueError(f"a field may not reuse a score's name: {sorted(set(fields) & set(measured))}")
    # The history names the tool `suite`, as its earliest rows already do.
    result = {"time": iso(), "suite": tool, "mode": mode, **fields, **measured,
              "seconds": round(time.perf_counter() - started, 1)}
    print(f"{tool} {mode}: MRR {result['mrr']:.3f}, top-1 {result['top1']:.0%}, "
          f"top-3 {result['top3']:.0%} of {result['rows']} questions in {result['seconds']} s")
    # Only a run of the same tool and mode over the same set compares.
    previous = next((e for e in reversed(scores_of(tool))
                     if e.get("mode") == mode and e.get("rows") == result["rows"]), None)
    if previous:
        print(f"last {previous['time'][:10]}: MRR {previous['mrr']:.3f}, top-3 {previous['top3']:.0%}"
              f" of {previous['rows']}")
    append(EVAL_HISTORY, result)
    return result


def scores_of(tool):
    """Every kept score of one tool, oldest first; unreadable lines are skipped."""
    return [row for row in rows_in(eval_history()) if row.get("suite") == tool]


def append(name, row):
    """One JSON line onto log_dir()/name; a write that fails is dropped, never raised."""
    try:
        file = log_dir() / name
        file.parent.mkdir(parents=True, exist_ok=True)
        with open(file, "a", encoding="utf-8") as stream:
            stream.write(json.dumps(row) + "\n")
    except OSError:
        pass


def rows_in(file):
    """The JSON objects of a log file, one per line; a missing file or a bad line is skipped."""
    try:
        with open(file, encoding="utf-8", errors="replace") as stream:
            lines = stream.readlines()
    except OSError:
        return []
    rows = []
    for line in lines:
        try:
            row = json.loads(line)
        except ValueError:
            continue
        if isinstance(row, dict):
            rows.append(row)
    return rows


def read(folder=None):
    return [row for file in sorted(Path(folder or log_dir()).glob("*.jsonl"))
            for row in rows_in(file) if row.get("tool")]


def since(rows, days):
    if not days:
        return rows
    cut = (datetime.now(timezone.utc) - timedelta(days=days)).isoformat(timespec="seconds")
    return [r for r in rows if (r.get("time") or "") >= cut]


def percent(part, whole):
    return f"{100 * part / whole:.0f}%" if whole else "-"


def summary(rows):
    """Plain-text report: use per tool and kind, follow-through, the commonest
    questions, and the missed ones of every kind."""
    if not rows:
        return "no lookups logged yet"
    lines = []
    first, last = min(r.get("time") or "~" for r in rows), max(r.get("time") or "" for r in rows)
    sessions = {r["session"] for r in rows if r.get("session")}
    lines.append(f"{len(rows)} lookups, {first[:10]} to {last[:10]}"
                 + (f", {len(sessions)} sessions" if sessions else ""))
    by_tool = defaultdict(list)
    for row in rows:
        by_tool[row["tool"]].append(row)
    lines.append("")
    lines.append(f"{'tool':<10} {'kind':<9} {'count':>6} {'weak':>6} {'opened':>7} {'median ms':>10}"
                 f" {'tokens':>8}")
    for tool in sorted(by_tool):
        kinds = Counter(r.get("kind", "?") for r in by_tool[tool])
        for kind, count in kinds.most_common():
            group = [r for r in by_tool[tool] if r.get("kind", "?") == kind]
            weak = sum(1 for r in group if r.get("weak"))
            judged = [r for r in group if "followed" in r]
            opened = sum(1 for r in judged if r["followed"])
            times = sorted(r["ms"] for r in group if isinstance(r.get("ms"), (int, float)))
            median = str(times[len(times) // 2]) if times else "-"
            sized = [r["chars"] for r in group if isinstance(r.get("chars"), int)]
            tokens = str(sum(sized) // CHARS_PER_TOKEN) if sized else "-"
            lines.append(f"{tool:<10} {kind:<9} {count:>6} {percent(weak, count):>6} "
                         f"{percent(opened, len(judged)):>7} {median:>10} {tokens:>8}")
    asked = Counter((r["tool"], r.get("kind", "?"), r["query"].lower()) for r in rows if r.get("query"))
    repeated = [(key, count) for key, count in asked.most_common(TOP_SHOWN) if count > 1]
    if repeated:
        lines += ["", "most asked:"]
        lines += [f"  {count:>3}  {tool} {kind}: {query[:90]}" for (tool, kind, query), count in repeated]
    missed = Counter((r["tool"], r.get("kind", "?"), r["query"].lower())
                     for r in rows if r.get("weak") and r.get("query"))
    if missed:
        lines += ["", "missed (weak or no result):"]
        lines += [f"  {count:>3}  {tool} {kind}: {query[:90]}"
                  for (tool, kind, query), count in missed.most_common(TOP_SHOWN)]
    return "\n".join(lines)


def main(argv=None):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(prog="query_log", description=__doc__.split("\n")[0])
    parser.add_argument("--days", type=int, default=0, help="only the last N days")
    parser.add_argument("--tool", help="only this tool's rows")
    parser.add_argument("--session", help="only this session's rows (the AUTANA_SESSION they ran under)")
    args = parser.parse_args(argv)
    rows = since(read(), args.days)
    if args.tool:
        rows = [r for r in rows if r["tool"] == args.tool]
    if args.session:
        rows = [r for r in rows if r.get("session") == args.session]
    print(f"log: {log_dir()}")
    print(summary(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
