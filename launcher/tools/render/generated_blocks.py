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
import re

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "scripts/gates"))
from tracked import tracked_files

MARKER = re.compile(r"^<!-- (/?generated): ([a-z0-9][a-z0-9-]*)"
                    r"(?: sha256=([0-9a-f]{64})| check: (\S.*?))? -->$", re.M)
TOKEN = re.compile(r"^<!--\s*/?generated:", re.M)


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


def apply_tables(root, tables, check=False):
    listing = tracked_files(root, ("*.md",))
    owners = {}
    for name in listing:
        for block in blocks((root / name).read_text(encoding="utf-8")):
            if block in owners:
                raise ValueError(f"generated name has multiple owners: {block}")
            owners[block] = name
    changed = False
    for table in sorted(tables.glob("*.md")):
        name = table.stem
        if name not in owners:
            raise ValueError(f"no document owns generated table {name}")
        doc = owners[name]
        different = replace_block(root / doc, name, table.read_text(encoding="utf-8"), check)
        print(f"{'changed' if different else 'same'} {doc}#{name}")
        changed |= different
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--tables", type=pathlib.Path)
    parser.add_argument("--doc", type=pathlib.Path)
    parser.add_argument("--name")
    parser.add_argument("--body", type=pathlib.Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        if args.tables:
            changed = apply_tables(args.root, args.tables, args.check)
        else:
            changed = replace_block(args.doc, args.name, args.body.read_text(encoding="utf-8"), args.check)
        return int(args.check and changed)
    except (ValueError, OSError) as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
