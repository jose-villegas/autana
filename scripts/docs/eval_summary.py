#!/usr/bin/env python3
"""Report a lookup tool's latest retrieval evaluation and require its expected mode.

    python3 scripts/docs/eval_summary.py [--tool NAME] hybrid|lexical

NAME is the tool whose last kept score is read (docs when omitted).

Appends the result and metrics to GITHUB_STEP_SUMMARY for nightly CI.
"""
import argparse
import json
import os
import sys
from pathlib import Path

import query_log


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--tool", default="docs")
    parser.add_argument("mode", choices=("hybrid", "lexical"))
    args = parser.parse_args(argv)
    try:
        lines = query_log.eval_history().read_text(encoding="utf-8").splitlines()
        line = [entry for entry in lines if json.loads(entry).get("suite") == args.tool][-1]
        result = json.loads(line)
        metrics = (f"{result['mode']}: MRR {result['mrr']:.3f}, top-1 {result['top1']:.0%}, "
                   f"top-3 {result['top3']:.0%}, {result['rows']} rows")
        print(line)
        with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as summary:
            summary.write(f"{metrics}\n\n```json\n{line}\n```\n\n")
    except (OSError, ValueError, IndexError, KeyError, TypeError) as error:
        print(f"Cannot report the {args.tool} evaluation: {error}", file=sys.stderr)
        return 1
    if result["mode"] != args.mode:
        print(f"{args.tool} evaluation: expected {args.mode}, got {result['mode']}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
