#!/usr/bin/env python3
"""Render CRC benchmark rows using the shared package benchmark capture parser."""
import argparse
from pathlib import Path
import sys

import math_bench_table as bench
from generated_blocks import replace_block

ROOT = bench.ROOT
CAPTURE = Path("docs/packages/data/hash_bench_capture.txt")
PLACEHOLDER = "# Board capture pending: run autana suite hash_perf --flash."
CANDIDATES = ("nibble", "slice4", "rom")
SIZES = (64, 4096, 65536)
MEMORIES = ("ram", "flash")
COMMAND = "python launcher/tools/perf/hash_bench_table.py --check"


def rows():
    return [(f"crc32_update_{candidate}_{size}_{memory}", f"crc32_update_{candidate}",
             f"{size}_{memory}", 1, "hash")
            for candidate in CANDIDATES for size in SIZES for memory in MEMORIES]


def render(capture):
    build, measured = bench.parse_capture(capture, rows(), prefix="HASHBENCH", placeholder=PLACEHOLDER)
    target = bench.parse_target(capture.replace("HASHTARGET", "MATHTARGET"))
    if measured and not target:
        raise ValueError("hash measurements require a hardware HASHTARGET record")
    lines = [f"Build: `{build or '-'}`; target: {target or '-' }.", "",
             "| Candidate | Bytes | Source | Warm cycles | Cold cycles | Instructions |",
             "|---|---|---|---|---|---|"]
    for key, label, kind, _, _ in rows():
        size, memory = kind.split("_")
        values = measured.get(key)
        cells = [f"{value:.1f}" for value in values] if values else ["-", "-", "-"]
        lines.append("| " + " | ".join([f"`{label}`", size, memory, *cells]) + " |")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        body = render((ROOT / CAPTURE).read_text())
        changed = replace_block(ROOT / "docs/packages/Hash-Benchmark.md", "hash-benchmark", body,
                                check=args.check, command=COMMAND)
        if args.check and changed:
            raise ValueError("hash benchmark table is stale")
    except (OSError, ValueError) as error:
        parser.exit(1, f"hash_bench_table: {error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
