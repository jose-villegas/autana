#!/usr/bin/env python3
"""Report the latest docs retrieval evaluation and require its expected mode.

    python3 scripts/docs/eval_summary.py hybrid|lexical

Appends the result and metrics to GITHUB_STEP_SUMMARY for nightly CI.
"""
import argparse
import json
import os
import sys
from pathlib import Path

import docs_search


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("mode", choices=("hybrid", "lexical"))
    args = parser.parse_args(argv)
    try:
        line = docs_search.eval_history().read_text(encoding="utf-8").splitlines()[-1]
        result = json.loads(line)
        metrics = (f"{result['mode']}: MRR {result['mrr']:.3f}, top-1 {result['top1']:.0%}, "
                   f"top-3 {result['top3']:.0%}, {result['rows']} rows")
        print(line)
        with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as summary:
            summary.write(f"{metrics}\n\n```json\n{line}\n```\n\n")
    except (OSError, ValueError, IndexError, KeyError, TypeError) as error:
        print(f"Cannot report docs evaluation: {error}", file=sys.stderr)
        return 1
    if result["mode"] != args.mode:
        print(f"Docs evaluation: expected {args.mode}, got {result['mode']}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
