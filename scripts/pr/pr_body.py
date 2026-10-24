"""Scaffold and check a pull request description.

  pr_body.py new [--kind change|move|perf|docs] [--ticket ID] [--base REF] [--perf DIR]
  pr_body.py check BODY_FILE

`new` prints .github/pull_request_template.md with its block slot replaced by
the kind's table and its Ticket line filled. Everything the branch already
shows is derived: the changed parts from the diff against the merge base of
--base (default origin/main), renames, line counts, document sizes and, for
perf, the rows of perf_compare's comparison.json in --perf DIR. The ticket is
--ticket, else the first segment of the branch name that the backlog (`bd`)
knows. What the diff cannot say is left as a `<!-- fill -->` marker.

`check` passes when no marker is left, the Ticket line passes
scripts/gates/check-pr-ticket.sh, and every mermaid block renders under
scripts/gates/check-mermaid.mjs.
"""
import argparse
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
TEMPLATE = REPO / '.github' / 'pull_request_template.md'
TICKET_GATE = REPO / 'scripts' / 'gates' / 'check-pr-ticket.sh'
MERMAID_GATE = REPO / 'scripts' / 'gates' / 'check-mermaid.mjs'
KINDS = ('change', 'move', 'perf', 'docs')
# Above this many rows a table stops being read at a glance, so changed files
# are grouped by ever shorter directory prefixes until the rows fit.
MAX_ROWS = 8
FILL = '<!-- fill -->'
MARKER = re.compile(r'<!-- (fill|block)\b')
BLOCK_LINE = re.compile(r'^<!-- block\b.*-->$', re.M)
TICKET_LINE = re.compile(r'^Ticket:.*$', re.M)
TICKET_ID = re.compile(r'^[a-z0-9]+(\.[0-9]+)*$')
FENCE = '```mermaid'


def git(*args):
    return subprocess.run(['git', *args], cwd=REPO, check=True, capture_output=True,
                          text=True, encoding='utf-8').stdout


def changed_files(base):
    """(added, deleted, old path, new path) per file between the merge base and HEAD."""
    merge_base = git('merge-base', base, 'HEAD').strip()
    fields = git('diff', '--numstat', '-z', '-M', merge_base, 'HEAD').split('\0')
    files = []
    index = 0
    while index < len(fields) - 1:
        added, deleted, path = fields[index].split('\t')
        index += 1
        if path:
            old = new = path
        else:
            old, new = fields[index], fields[index + 1]
            index += 2
        files.append((0 if added == '-' else int(added), 0 if deleted == '-' else int(deleted), old, new))
    return merge_base, files


def prefix(path, depth):
    """The row a path falls in when rows reach `depth` directories: a directory, or the file."""
    parts = PurePosixPath(path).parts
    return parts[:depth] if len(parts) > depth else parts


def group(paths):
    """Map each row label to its paths: top-level directories first, then the
    largest row is split into its subdirectories while the rows fit MAX_ROWS."""
    rows = {}
    for path in paths:
        rows.setdefault((prefix(path, 1), 1), []).append(path)
    while True:
        open_rows = [key for key in rows
                     if any(len(PurePosixPath(path).parts) > key[1] for path in rows[key])]
        if not open_rows:
            break
        # Rows tied for largest split together, so equal parts get equal detail.
        largest = max(len(rows[key]) for key in open_rows)
        split = [key for key in open_rows if len(rows[key]) == largest]
        children = {}
        for key in split:
            for path in rows[key]:
                children.setdefault((prefix(path, key[1] + 1), key[1] + 1), []).append(path)
        if len(rows) - len(split) + len(children) > MAX_ROWS:
            break
        for key in split:
            del rows[key]
        rows.update(children)
    labels = {}
    for (parts, _), members in sorted(rows.items()):
        if len(members) == 1:
            labels[f'`{members[0]}`'] = members
        else:
            labels[f'`{"/".join(parts)}/` ({len(members)} files)'] = members
    return labels


def lines_row(files):
    added = sum(f[0] for f in files)
    deleted = sum(f[1] for f in files)
    return f'**Lines:** +{added} / −{deleted} (net {added - deleted:+d})'


def change_table(files):
    rows = ['| Piece | Before | After |', '|---|---|---|']
    rows += [f'| {label} | {FILL} | {FILL} |' for label in group([f[3] for f in files])]
    return rows


def move_table(files):
    moved = [f for f in files if f[2] != f[3]]
    pairs = {}
    for _, _, old, new in moved:
        key = (str(PurePosixPath(old).parent), str(PurePosixPath(new).parent))
        pairs.setdefault(key, []).append((old, new))
    rows = ['| From | To |', '|---|---|']
    for (old_dir, new_dir), members in sorted(pairs.items()):
        if len(members) == 1:
            rows.append(f'| `{members[0][0]}` | `{members[0][1]}` |')
        else:
            rows.append(f'| `{old_dir}/` ({len(members)} files) | `{new_dir}/` |')
    rest = [f for f in files if f[2] == f[3]]
    return rows + ([''] + change_table(rest) if rest else [])


def size(rev, path):
    shown = subprocess.run(['git', 'show', f'{rev}:{path}'], cwd=REPO, capture_output=True,
                           text=True, encoding='utf-8')
    if shown.returncode:
        return '—'
    return f'{len(shown.stdout.splitlines())} lines, {len(shown.stdout.split()):,} words'


def docs_table(merge_base, files):
    rows = ['| File | Before | After |', '|---|---|---|']
    for _, _, old, new in files:
        if new.endswith('.md'):
            label = f'`{new}`' if old == new else f'`{new}` (was `{old}`)'
            rows.append(f'| {label} | {size(merge_base, old)} | {size("HEAD", new)} |')
    rest = [f for f in files if not f[3].endswith('.md')]
    return rows + ([''] + change_table(rest) if rest else [])


def number(value):
    return 'n/a' if value is None else f'{value:.4g}'


def perf_table(perf_dir):
    comparison = json.loads((Path(perf_dir) / 'comparison.json').read_text(encoding='utf-8'))
    rows = [f'| Row | {comparison["label_a"]} µs | {comparison["label_b"]} µs | Change | Verdict |',
            '|---|---:|---:|---:|---|']
    for name, row in sorted(comparison['rows'].items()):
        ratio = row.get('ratio')
        change = 'n/a' if not ratio else f'{100 * (ratio - 1):+.1f}%'
        rows.append(f'| `{name}` | {number(row.get("a"))} | {number(row.get("b"))} | {change} | {row["verdict"]} |')
    if comparison.get('incomplete'):
        rows += ['', '**Incomplete comparison:** see its summary.md.']
    return rows + ['', '| Goal | Old | New |', '|---|---:|---:|', f'| {FILL} | {FILL} | {FILL} |']


def ticket_from_branch():
    branch = git('rev-parse', '--abbrev-ref', 'HEAD').strip()
    bd = shutil.which('bd')
    if bd is None:
        return None
    for segment in re.split(r'[/-]', branch):
        if not TICKET_ID.match(segment):
            continue
        shown = subprocess.run([bd, 'show', segment, '--json'], capture_output=True, text=True,
                               encoding='utf-8')
        if shown.returncode:
            continue
        try:
            issues = json.loads(shown.stdout)
        except json.JSONDecodeError:
            continue
        issue = issues[0] if isinstance(issues, list) and issues else issues
        if isinstance(issue, dict) and issue.get('id') in (segment, f'autana-{segment}'):
            return segment
    return None


def scaffold(kind, ticket, base, perf_dir):
    merge_base, files = changed_files(base)
    if kind == 'move':
        block = move_table(files)
    elif kind == 'docs':
        block = docs_table(merge_base, files)
    elif kind == 'perf':
        block = perf_table(perf_dir) + [''] + change_table(files)
    else:
        block = change_table(files)
    block += ['', lines_row(files)]
    body = TEMPLATE.read_text(encoding='utf-8')
    body = BLOCK_LINE.sub(lambda _: '\n'.join(block), body, count=1)
    return TICKET_LINE.sub(lambda _: f'Ticket: {ticket}', body, count=1)


def run_gate(command, body, label):
    program = shutil.which(command[0])
    if program is None:
        return [f'{label}: `{command[0]}` is not on PATH, so this check cannot run']
    result = subprocess.run([program, *command[1:]], input=body, capture_output=True, text=True, encoding='utf-8')
    if result.returncode == 0:
        return []
    return [f'{label}: {(result.stderr or result.stdout).strip()}']


def check(body):
    """Problems with a description, empty when it is ready."""
    problems = [f'line {number}: unfilled marker: {line.strip()}'
                for number, line in enumerate(body.splitlines(), 1) if MARKER.search(line)]
    problems += run_gate(['sh', str(TICKET_GATE)], body, 'ticket')
    if FENCE in body:
        problems += run_gate(['node', str(MERMAID_GATE), '--stdin', 'PR description'], body, 'mermaid')
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    commands = parser.add_subparsers(dest='command', required=True)
    new = commands.add_parser('new', help='print a scaffolded description')
    new.add_argument('--kind', choices=KINDS, default='change')
    new.add_argument('--ticket', help='backlog id; default: from the branch name')
    new.add_argument('--base', default='origin/main')
    new.add_argument('--perf', metavar='DIR', help="perf_compare's --out directory (kind perf)")
    checked = commands.add_parser('check', help='fail on markers, a bad Ticket line or a broken diagram')
    checked.add_argument('body', type=Path)
    args = parser.parse_args(argv)

    if args.command == 'check':
        problems = check(args.body.read_text(encoding='utf-8'))
        for problem in problems:
            print(problem, file=sys.stderr)
        return 1 if problems else 0

    if args.kind == 'perf' and not args.perf:
        parser.error('--kind perf needs --perf DIR')
    ticket = args.ticket or ticket_from_branch()
    if not ticket:
        parser.error('no backlog id found in the branch name; pass --ticket')
    ticket = ticket.removeprefix('autana-')
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stdout.write(scaffold(args.kind, ticket, args.base, args.perf))
    return 0


if __name__ == '__main__':
    sys.exit(main())
