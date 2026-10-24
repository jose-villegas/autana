"""Embed whole C examples in their owning Markdown blocks.

    python launcher/tools/gen/examples_doc.py [--check]

The discovery and naming rule is documented in docs/tools/Generated-Files.md.
"""
import argparse
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
from generated_blocks import block_owners, check_commands, replace_block
from tracked import tracked_files

CHECK = "python launcher/tools/gen/examples_doc.py --check"


def block_name(area, stem, kind):
    return f"{area}-{stem}-{kind}".replace("_", "-")


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
    for block, doc in owners.items():
        if check_commands((root / doc).read_text(encoding="utf-8")).get(block) == CHECK:
            if block not in examples:
                raise ValueError(f"{doc}#{block}: no example file")
    for block, name in examples.items():
        if block not in owners:
            raise ValueError(f"no document owns generated block {block} for {name}")
    changed = False
    for block, name in sorted(examples.items()):
        body = "```c\n" + (root / name).read_text(encoding="utf-8").rstrip("\n") + "\n```"
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
