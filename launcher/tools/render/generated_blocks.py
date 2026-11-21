"""Replace named Markdown blocks, retaining the document's line endings.

A block's opening marker either records the SHA-256 of its body, which
catches a hand edit to a block nothing regenerates, or names the command,
given by its generator, that regenerates and compares it (`check: COMMAND`).
The second form changes only when the command does, so two branches that
each regenerate the block merge without conflicting on its marker line.
"""
import argparse
import sys
import hashlib
import pathlib
import posixpath
import re

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "scripts/gates"))
from tracked import tracked_files
from check_doc_index import SCHEME
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from doc_tables import LOOK_COLUMNS, SCORE_COLUMNS, table_rows

MARKER = re.compile(r"^<!-- (/?generated): ([a-z0-9][a-z0-9-]*)"
                    r"(?: sha256=([0-9a-f]{64})| check: (\S.*?))? -->$", re.M)
TOKEN = re.compile(r"^<!--\s*/?generated:", re.M)
# A table does not know which page owns its block, so its relative links are
# written as from docs/<folder>/page.md and re-based to the real owner.
TABLE_BASE = "docs/page"
LINK = re.compile(r"(\]\()([^)\s#][^)\s]*)(\))")
# A refreshed table's mean dE76 against a reference (the MEAN_DE columns) may
# not pass both of these over its published value: a render drawn at the wrong
# pose scores five or more times worse (#1597: 5.5 -> 34.6), while a re-bake
# moves it by a few tenths.
DE_JUMP_RATIO = 2.0
DE_JUMP_MIN = 5.0
MEAN_DE = (SCORE_COLUMNS[0], *LOOK_COLUMNS[:2])


def digest(body):
    return hashlib.sha256(body.replace("\r\n", "\n").encode("utf-8")).hexdigest()


def blocks(text):
    text = text.replace("\r\n", "\n")
    markers = list(MARKER.finditer(text))
    if len(markers) != len(TOKEN.findall(text)):
        raise ValueError("malformed generated marker")
    found = {}
    start = None
    for marker in markers:
        kind, name, checksum, command = marker.groups()
        if kind == "generated":
            if start is not None or name in found:
                raise ValueError(f"nested or duplicate generated block: {name}")
            start = marker
        else:
            if start is None or name != start.group(2) or checksum or command:
                raise ValueError(f"unmatched generated end: {name}")
            found[name] = (start.start(), marker.end(), text[start.end():marker.start()], *start.group(3, 4))
            start = None
    if start is not None:
        raise ValueError(f"missing generated end: {start.group(2)}")
    return found


def verify(text):
    """Names of the blocks whose recorded digest does not match their body;
    a block that names its check command is the command's to verify."""
    return [name for name, (_, _, body, checksum, command) in blocks(text).items()
            if command is None and checksum != digest(body)]


def check_commands(text):
    """The check command each block names, by block name."""
    return {name: command for name, (*_, command) in blocks(text).items() if command}


def replace_block(path, name, body, check=False, command=None):
    """Write `body` into block `name`; True when that changes the file.
    A generator CI reruns passes the `command` that checks it, which the
    marker names in place of the body's digest."""
    path = pathlib.Path(path)
    raw = path.read_bytes()
    newline = "\r\n" if b"\r\n" in raw else "\n"
    original = raw.decode("utf-8")
    text = original.replace("\r\n", "\n")
    spans = blocks(text)
    if name not in spans:
        raise ValueError(f"{path}: missing generated block {name}")
    start, end, *_ = spans[name]
    body = "\n" + body.replace("\r\n", "\n").strip("\n") + "\n"
    stamp = f"check: {command}" if command else f"sha256={digest(body)}"
    replacement = f"<!-- generated: {name} {stamp} -->{body}<!-- /generated: {name} -->"
    offsets = [0, *(match.end() for match in re.finditer(r"\r\n|.", original, re.S))]
    updated = (original[:offsets[start]] + replacement.replace("\n", newline) + original[offsets[end]:]).encode("utf-8")
    changed = updated != raw
    if changed and not check:
        path.write_bytes(updated)
    return changed


def replace_blocks(path, bodies, check=False, command=None):
    """replace_block for each name and body of `bodies`; True when any changes the file."""
    return any([replace_block(path, name, body, check, command) for name, body in bodies.items()])


def block_owners(root):
    """Document paths by generated block name, rejecting duplicate owners."""
    listing = tracked_files(root, ("*.md",))
    owners = {}
    for name in listing:
        for block in blocks((root / name).read_text(encoding="utf-8")):
            if block in owners:
                raise ValueError(f"generated name has multiple owners: {block}")
            owners[block] = name
    return owners


def block_body(root, name):
    """Read the body of a generated block from its owning document."""
    return blocks((root / block_owners(root)[name]).read_text(encoding="utf-8"))[name][2]


def rebase_links(body, page):
    """`body` with each relative link moved from TABLE_BASE to `page`'s folder."""
    folder = posixpath.dirname(page)

    def move(match):
        target = match.group(2)
        if SCHEME.match(target) or target.startswith("/"):
            return match.group(0)
        path, _, fragment = target.partition("#")
        moved = posixpath.relpath(posixpath.normpath(posixpath.join(TABLE_BASE, path)), folder or ".")
        return match.group(1) + moved + ("#" + fragment if fragment else "") + match.group(3)

    return LINK.sub(move, body)


def mean_de_cells(body):
    """{(table, first cell, column): cell} of each MEAN_DE column of each
    table row of `body`; `table` counts the tables, so ranks in one do not
    meet another's."""
    cells, table, previous = {}, -1, None
    for _line, headers, row in table_rows(body):
        if headers is not previous:
            table, previous = table + 1, headers
        cells.update({(table, row[0], name): cell for name, cell in zip(headers, row) if name in MEAN_DE})
    return cells


def reference_jumps(published, refreshed):
    """The rows of `refreshed` whose MEAN_DE against a reference passed
    DE_JUMP_RATIO times and DE_JUMP_MIN above the value `published` holds."""
    before, after = mean_de_cells(published), mean_de_cells(refreshed)
    jumps = []
    for key, cell in after.items():
        try:
            old, new = float(before[key]), float(cell)
        except (KeyError, ValueError):
            continue
        if new > old * DE_JUMP_RATIO and new - old > DE_JUMP_MIN:
            jumps.append(f"{key[1]}: {key[2]} {before[key]} -> {cell}")
    return jumps


def apply_tables(root, tables, check=False, accept_jumps=False):
    """Write each table of `tables` into the block its name owns; True when one
    changes. A jump reference_jumps finds is refused unless `accept_jumps`."""
    owners = block_owners(root)
    changed = False
    for table in sorted(tables.glob("*.md")):
        name = table.stem
        if name not in owners:
            raise ValueError(f"no document owns generated table {name}")
        doc = owners[name]
        body = rebase_links(table.read_text(encoding="utf-8"), doc)
        jumps = [] if accept_jumps else reference_jumps(blocks((root / doc).read_text(encoding="utf-8"))[name][2], body)
        if jumps:
            raise ValueError("".join(f"{doc}#{name} {jump}\n" for jump in jumps)
                             + "a figure scored against its reference far worse than published: a render at the wrong "
                               "pose, or a meant change. If meant, regenerate in the change that causes it and run "
                               "generated_blocks.py --tables DIR --accept-reference-jump")
        different = replace_block(root / doc, name, body, check)
        print(f"{'changed' if different else 'same'} {doc}#{name}")
        changed |= different
    return changed


def block_main(page, name, command, body, description, argv=None):
    """A generator's command line: write `body()` into block `name` of `page`,
    or with --check fail when the block is stale. `command` is the --check run
    the marker names."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        if replace_block(page, name, body(), check=args.check, command=command) and args.check:
            raise ValueError(f"{page.name}#{name} is stale: run {command.replace(' --check', '')}")
    except (OSError, ValueError) as error:
        parser.exit(1, f"{pathlib.Path(sys.argv[0]).stem}: {error}\n")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--tables", type=pathlib.Path)
    parser.add_argument("--doc", type=pathlib.Path)
    parser.add_argument("--name")
    parser.add_argument("--body", type=pathlib.Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--accept-reference-jump", action="store_true",
                        help="with --tables, write a Mean dE76 that passes DE_JUMP_RATIO and DE_JUMP_MIN over its block")
    args = parser.parse_args()
    try:
        if args.tables:
            changed = apply_tables(args.root, args.tables, args.check, args.accept_reference_jump)
        else:
            changed = replace_block(args.doc, args.name, args.body.read_text(encoding="utf-8"), args.check)
        return int(args.check and changed)
    except (ValueError, OSError) as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
