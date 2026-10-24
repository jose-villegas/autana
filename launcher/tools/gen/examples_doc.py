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
from generated_blocks import block_owners, check_commands, replace_block
from tracked import tracked_files
from c_lexer import tokens
from idf_vocabulary import DECLARATION, DIRECTIVE, FLOW, WORD

CHECK = "python launcher/tools/gen/examples_doc.py --check"


def block_name(area, stem, kind):
    return f"{area}-{stem}-{kind}".replace("_", "-")


def declaration(root, function):
    """Copy the unique tracked header prototype, preserving its layout."""
    found = {}
    for name in tracked_files(root, ("*.h",)):
        if not name.startswith("launcher/"):
            continue
        source = (root / name).read_text(encoding="utf-8")
        code = list(source)
        for _, start, end in tokens(source):
            code[start:end] = ["\n" if ch == "\n" else " " for ch in source[start:end]]
        masked = "".join(code)
        masked = DIRECTIVE.sub(
            lambda match: "".join("\n" if ch == "\n" else " " for ch in match[0]), masked)
        for match in DECLARATION.finditer(masked):
            prefix, name_in_header = match.groups()
            if name_in_header != function or not FLOW.isdisjoint(WORD.findall(prefix)):
                continue
            tail = re.match(r"\([^;{}]*\)\s*;", masked[match.end() - 1:])
            if tail:
                end = match.end() - 1 + tail.end()
                found[name] = source[match.start():end].strip()
                break

    if len(found) != 1:
        raise ValueError(f"{function}: expected exactly one declaring header; found {len(found)}"
                         + (": " + ", ".join(found) if found else ""))
    return next(iter(found.values()))


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
    declarations = {block: declaration(root, block[len("declaration-"):].replace("-", "_"))
                    for block in owners if block.startswith("declaration-")}
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


def main(argv=None, root=ROOT):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        return int(write_examples(root, args.check) and args.check)
    except (ValueError, OSError) as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
