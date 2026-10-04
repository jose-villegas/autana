"""Check generated Markdown bodies against their marker's SHA-256, without rendering."""
import argparse
import pathlib
import sys

from tracked import tracked_files

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "launcher/tools/render"))
from generated_blocks import blocks, verify


def check(root):
    errors = []
    owners = {}
    for name in tracked_files(root, ["*.md"]):
        try:
            text = (root / name).read_text(encoding="utf-8")
            for block in blocks(text):
                if block in owners:
                    errors.append(f"{name}#{block}: duplicate name, also in {owners[block]}")
                owners[block] = name
            errors.extend(f"{name}#{block}: generated body changed; run its generator"
                          for block in verify(text))
        except ValueError as error:
            errors.append(f"{name}: {error}")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    errors = check(args.root)
    for error in errors:
        print(error)
    print(f"generated documentation: {len(errors)} errors")
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
