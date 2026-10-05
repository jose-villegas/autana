"""Tiny budget table command for revision-source acquisition tests."""
import argparse
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('capture', type=Path)
parser.add_argument('table', type=Path)
parser.add_argument('--source', type=Path, required=True)
args = parser.parse_args()
budgets = dict(line.split() for line in args.source.read_text().splitlines())
rows = ['| Test | Budget (us) | Measured (us) | Headroom | Status |',
        '|---|---:|---:|---:|---|']
measured = None
for line in args.capture.read_text().splitlines():
    timing = re.search(r'row (\d+) us', line)
    if timing:
        measured = int(timing[1])
    result = re.search(r':\d+:(\w+):PASS$', line)
    if result:
        name = result[1]
        rows.append(f'| `{name}` | {budgets[name]} | {measured} | ? | PASS |')
        measured = None
args.table.write_text('\n'.join(rows) + '\n')
