#!/usr/bin/env python3
"""Fail when a field in an entry-format packet diagram does not start with the
colour marker of its role; `--write` puts the right marker in place. Packet
diagrams cannot colour a single field, so the marker is a coloured square.

    python scripts/gates/check_format_roles.py [--write] [PAGE ...]

Reads every docs/assets/formats/*.md page, or the pages named. A field's role
comes from its label alone, by the first rule that matches (the legend is in
docs/assets/formats/README.md):

| Role | Rule |
|---|---|
| padding | the label is `0`, `reserved` or `0 (reserved)` |
| identity | it starts with `magic` or `version`, or is `crc32` |
| offset | it ends in `_off` or is `offset` |
| count or size | it ends in `count`, `_size` or is `size`, or is one plural word |
| code | the page's header names two or more of its values, `<PREFIX>_<LABEL>_<NAME>` |
| value | anything else; it has no marker |

The header is the first `launcher/main/**.h` the page links to.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PAGES = ROOT / "docs/assets/formats"
MARKERS = {"padding": "⬜", "identity": "🟦", "offset": "🟧", "count": "🟩", "code": "🟪"}  # a value has none
BLOCK = re.compile(r"```mermaid\n(.*?)```", re.S)
FIELD = re.compile(r'^(?P<at>\d+(?:-\d+)?): "(?P<label>.*)"$', re.M)
HEADER = re.compile(r"\]\(((?:\.\./)+launcher/main/[^)]+\.h)\)")
DEFINE = re.compile(r"^#define\s+([A-Z0-9_]+)", re.M)
PADDING = {"0", "reserved", "0 (reserved)"}
PLURAL = re.compile(r"^[a-z]+s$")
CODE_VALUES = 2  # named values that make a field a code: an enum has two at least


def codes(page, text):
    """The define names of the page's header, or none when it links to none."""
    match = HEADER.search(text)
    if not match:
        return []
    return DEFINE.findall((page.parent / match.group(1)).resolve().read_text(encoding="utf-8"))


def role(label, defines):
    """The role of a field from its label, by the module docstring's rules."""
    if label in PADDING:
        return "padding"
    if label.startswith(("magic", "version")) or label == "crc32":
        return "identity"
    if label.endswith("_off") or label == "offset":
        return "offset"
    if label.endswith(("count", "_size")) or label == "size" or PLURAL.match(label):
        return "count"
    named = re.compile(rf"_{label.upper()}_[A-Z0-9_]+$")
    if re.fullmatch(r"[a-z_]+", label) and sum(bool(named.search(name)) for name in defines) >= CODE_VALUES:
        return "code"
    return "value"


def unmarked(label):
    """The label without its marker."""
    for marker in MARKERS.values():
        if label.startswith(marker + " "):
            return label[len(marker) + 1:]
    return label


def marked(page):
    """The page with every packet field marked by its role, and the fields
    whose marker was wrong or missing."""
    text = page.read_text(encoding="utf-8")
    defines = codes(page, text)
    wrong = []

    def field(match):
        label = unmarked(match["label"])
        marker = MARKERS.get(role(label, defines))
        want = f"{marker} {label}" if marker else label
        if want != match["label"]:
            wrong.append(f"{page.relative_to(ROOT).as_posix()}: {match['at']} \"{match['label']}\" wants \"{want}\"")
        return f'{match["at"]}: "{want}"'

    def block(match):
        body = match.group(1)
        return match.group(0) if "packet-beta" not in body else f"```mermaid\n{FIELD.sub(field, body)}```"

    return BLOCK.sub(block, text), wrong


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--write", action="store_true", help="put the right marker on every field")
    parser.add_argument("pages", nargs="*", type=pathlib.Path)
    args = parser.parse_args()
    pages = [page.resolve() for page in args.pages] or sorted(PAGES.glob("*.md"))
    wrong = []
    for page in pages:
        text, found = marked(page)
        wrong += found
        if args.write and found:
            page.write_text(text, encoding="utf-8", newline="\n")
    if not args.write:
        print("\n".join(wrong))
    print(f"{len(wrong)} format field(s) without their role's marker")
    return 1 if wrong and not args.write else 0


if __name__ == "__main__":
    sys.exit(main())
