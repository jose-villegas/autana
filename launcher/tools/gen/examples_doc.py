"""Embed whole C examples and header declarations in their owning Markdown blocks.

    python launcher/tools/gen/examples_doc.py [--check]

The discovery and naming rule is documented in docs/tools/Generated-Files.md.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
from generated_blocks import block_owners, blocks, check_commands, replace_block
from tracked import tracked_files
from idf_vocabulary import prototypes

CHECK = "python launcher/tools/gen/examples_doc.py --check"
FENCE = re.compile(r"^\s*(?:(?:[-+*]|[0-9]+[.)])\s+)?(`{3,}|~{3,})(.*)$")
C_LANGUAGES = {"c", "h", "cpp"}


def block_name(area, stem, kind):
    return f"{area}-{stem}-{kind}".replace("_", "-")


def header_declarations(root):
    """Index prototypes by function, with one entry per tracked launcher header."""
    declarations = {}
    for header in tracked_files(root, ("*.h",)):
        if not header.startswith("launcher/"):
            continue
        source = (root / header).read_text(encoding="utf-8")
        found = {}
        for name, start, end in prototypes(source):
            found.setdefault(name, source[start:end].strip())
        for name, text in found.items():
            declarations.setdefault(name, []).append((header, text))
    return declarations


def declaration(declarations, function):
    """Copy the unique tracked header prototype, preserving its layout."""
    found = declarations.get(function, [])
    if len(found) != 1:
        raise ValueError(f"{function}: expected exactly one declaring header; found {len(found)}"
                         + (": " + ", ".join(header for header, _ in found) if found else ""))
    return found[0][1]


def write_examples(root, check=False):
    owners = block_owners(root)
    examples = {}
    for name in tracked_files(root):
        path = pathlib.PurePosixPath(name)
        for folder in path.parts[:-1]:
            if not folder.endswith("_examples"):
                continue
            area = folder[:-len("_examples")]
            if path.suffix != ".c":
                continue
            block = block_name(area, path.stem, "example")
            if block in examples:
                raise ValueError(f"{block}: multiple files: {examples[block]}, {name}")
            examples[block] = name
    headers = header_declarations(root)
    declarations = {block: declaration(headers, block[len("declaration-"):].replace("-", "_"))
                    for block in owners if block.startswith("declaration-") and block not in examples}
    for block, doc in owners.items():
        if check_commands((root / doc).read_text(encoding="utf-8")).get(block) == CHECK:
            if block not in examples and block not in declarations:
                raise ValueError(f"{doc}#{block}: no example file")
    for block, name in examples.items():
        if block not in owners:
            raise ValueError(f"no document owns generated block {block} for {name}")
    changed = False
    bodies = {block: (root / name).read_text(encoding="utf-8").rstrip("\n")
              for block, name in examples.items()}
    bodies.update(declarations)
    for block, source in sorted(bodies.items()):
        body = "```c\n" + source + "\n```"
        different = replace_block(root / owners[block], block, body, check, CHECK)
        print(f"{'changed' if different else 'same'} {owners[block]}#{block}")
        changed |= different
    return changed


def unmarked_fences(text):
    """Line numbers of handwritten C fences without the fragment word."""
    text = text.replace("\r\n", "\n")
    generated = [(start, end) for start, end, *_ in blocks(text).values()]
    opened = None
    offset = 0
    for number, line in enumerate(text.splitlines(keepends=True), 1):
        match = FENCE.match(line)
        if match:
            fence, info = match.groups()
            if opened:
                if fence[0] == opened[0] and len(fence) >= len(opened) and not info.strip():
                    opened = None
            else:
                opened = fence
                words = info.split()
                if (words and words[0] in C_LANGUAGES and "fragment" not in words[1:]
                        and not any(start <= offset < end for start, end in generated)):
                    yield number
        offset += len(line)


def check_fences(root):
    """Check every tracked Markdown document, including outside docs/."""
    failed = False
    for name in tracked_files(root, ("*.md",)):
        for number in unmarked_fences((root / name).read_text(encoding="utf-8")):
            print(f"{name}:{number}: embed an example file, a declaration block, "
                  "or mark it ```c fragment")
            failed = True
    return failed


def main(argv=None, root=ROOT):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        changed = write_examples(root, args.check)
        failed = check_fences(root) if args.check else False
        return int(args.check and (changed or failed))
    except (ValueError, OSError) as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
