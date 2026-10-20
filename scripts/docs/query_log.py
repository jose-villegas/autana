#!/usr/bin/env python3
"""A local log of documentation lookups, and its summary.

    python scripts/docs/query_log.py [--days N] [--tool NAME]   summarise the log

Each lookup appends one JSON line to queries.jsonl in the per-user autana
cache (%LOCALAPPDATA%/autana/usage, or ~/.cache/autana/usage): the time, the
tool, the question, its best results with their scores, how long it took,
whether meaning-based ranking was on, and whether the answer was weak. The
summary reads every *.jsonl in that folder, so rows recovered from elsewhere
can sit beside the live ones. Nothing leaves the machine; deleting the folder
loses only the history.

A failure to write the log never fails the lookup that made it.
"""
import argparse
import json
import sys
import time
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
import autana_config  # noqa: E402

LOG_NAME = "queries.jsonl"
HITS_KEPT = 5
TOP_SHOWN = 15


def log_dir():
    return autana_config.user_cache_dir() / "usage"


def iso(seconds=None):
    moment = datetime.fromtimestamp(time.time() if seconds is None else seconds, timezone.utc)
    return moment.isoformat(timespec="seconds")


def record(tool, kind, query, hits=(), started=None, **fields):
    """Append one lookup; hits are (label, score) pairs, best first."""
    row = {"time": iso(), "tool": tool, "kind": kind, "query": query,
           "hits": [[label, None if score is None else round(score, 4)]
                    for label, score in list(hits)[:HITS_KEPT]]}
    if started is not None:
        row["ms"] = round((time.perf_counter() - started) * 1000)
    row.update(fields)
    append(LOG_NAME, row)


def append(name, row):
    """One JSON line onto log_dir()/name; a write that fails is dropped, never raised."""
    try:
        folder = log_dir()
        folder.mkdir(parents=True, exist_ok=True)
        with open(folder / name, "a", encoding="utf-8") as stream:
            stream.write(json.dumps(row, ensure_ascii=False) + "\n")
    except OSError:
        pass


def read(folder=None):
    rows = []
    for file in sorted(Path(folder or log_dir()).glob("*.jsonl")):
        with open(file, encoding="utf-8", errors="replace") as stream:
            for line in stream:
                try:
                    row = json.loads(line)
                except ValueError:
                    continue
                if isinstance(row, dict) and row.get("tool"):
                    rows.append(row)
    return rows


def since(rows, days):
    if not days:
        return rows
    cut = (datetime.now(timezone.utc) - timedelta(days=days)).isoformat(timespec="seconds")
    return [r for r in rows if (r.get("time") or "") >= cut]


def percent(part, whole):
    return f"{100 * part / whole:.0f}%" if whole else "-"


def summary(rows):
    """Plain-text report: use per tool and kind, follow-through, the commonest
    and the missed questions, and the documents most often refused whole."""
    if not rows:
        return "no lookups logged yet"
    lines = []
    first, last = min(r.get("time") or "~" for r in rows), max(r.get("time") or "" for r in rows)
    lines.append(f"{len(rows)} lookups, {first[:10]} to {last[:10]}")
    by_tool = defaultdict(list)
    for row in rows:
        by_tool[row["tool"]].append(row)
    lines.append("")
    lines.append(f"{'tool':<10} {'kind':<9} {'count':>6} {'weak':>6} {'opened':>7} {'median ms':>10}")
    for tool in sorted(by_tool):
        kinds = Counter(r.get("kind", "?") for r in by_tool[tool])
        for kind, count in kinds.most_common():
            group = [r for r in by_tool[tool] if r.get("kind", "?") == kind]
            weak = sum(1 for r in group if r.get("weak"))
            judged = [r for r in group if "followed" in r]
            opened = sum(1 for r in judged if r["followed"])
            times = sorted(r["ms"] for r in group if isinstance(r.get("ms"), (int, float)))
            median = str(times[len(times) // 2]) if times else "-"
            lines.append(f"{tool:<10} {kind:<9} {count:>6} {percent(weak, count):>6} "
                         f"{percent(opened, len(judged)):>7} {median:>10}")
    searches = [r for r in rows if r.get("kind") in ("search", "ask") and r.get("query")]
    if searches:
        lines += ["", "most asked:"]
        for query, count in Counter((r["tool"], r["query"].lower()) for r in searches).most_common(TOP_SHOWN):
            if count > 1:
                lines.append(f"  {count:>3}  {query[0]}: {query[1][:90]}")
        missed = Counter((r["tool"], r["query"].lower()) for r in searches if r.get("weak"))
        if missed:
            lines += ["", "missed (weak or no result):"]
            lines += [f"  {count:>3}  {tool}: {query[:90]}" for (tool, query), count in missed.most_common(TOP_SHOWN)]
    refused = Counter(r["query"] for r in rows if r.get("kind") == "refused")
    if refused:
        bypassed = Counter(r["query"] for r in rows if r.get("kind") == "bypass")
        lines += ["", "refused whole-file reads (then read whole anyway):"]
        lines += [f"  {count:>3}  {path}  ({bypassed.get(path, 0)})" for path, count in refused.most_common(TOP_SHOWN)]
    return "\n".join(lines)


def main(argv=None):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(prog="query_log", description=__doc__.split("\n")[0])
    parser.add_argument("--days", type=int, default=0, help="only the last N days")
    parser.add_argument("--tool", help="only this tool's rows")
    args = parser.parse_args(argv)
    rows = since(read(), args.days)
    if args.tool:
        rows = [r for r in rows if r["tool"] == args.tool]
    print(f"log: {log_dir()}")
    print(summary(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
