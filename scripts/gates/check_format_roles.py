#!/usr/bin/env python3
"""Fail when an entry-format packet diagram's boxes are not filled by their
fields' roles; `--write` puts the right `themeCSS` in each diagram's config.
The packet grammar has no per-block style, so the CSS picks each box by its
row (an SVG group after one empty one) and its place in the row; mermaid drops
a themeCSS holding `>`, so the selectors use descendants.

    python scripts/gates/check_format_roles.py [--write] [PAGE ...]

Reads every docs/assets/formats/*.md page, or the pages named. A field's role
comes from its label alone, by the first rule that matches (the legend is in
docs/assets/formats/README.md, whose legend strip is filled by the role each
box names):

| Role | Rule |
|---|---|
| padding | the label is `0`, `reserved` or `0 (reserved)` |
| identity | it starts with `magic` or `version`, or is `crc32` |
| offset | it ends in `_off` or is `offset` |
| count or size | it ends in `count`, `_size` or is `size`, or is one plural word |
| code | the page's header names two or more of its values, `<PREFIX>_<LABEL>_<NAME>` |
| value | anything else |

The header is the first `launcher/main/**.h` the page links to.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PAGES = ROOT / "docs/assets/formats"
# Light fills under dark labels, so a box reads on either theme's background.
FILLS = {"identity": "fill: #8fb3ee", "count": "fill: #93d6b0", "offset": "fill: #f4a885", "code": "fill: #c7aaee",
         "value": "fill: #e3e3e3", "padding": "fill: #ffffff; stroke-dasharray: 4 3"}
LABEL_CSS = ".packetLabel { fill: #1b1b1b !important; }"
FIRST_ROW_GROUP = 2  # the SVG's first group is empty
LEGEND = {"identity": "identity", "count or size": "count", "offset": "offset", "code": "code", "value": "value",
          "padding": "padding"}
MARKERS = ("⬜", "🟦", "🟧", "🟩", "🟪")  # the squares fields carried before their boxes were filled
DEFAULT_BITS_PER_ROW = 32
BITS = re.compile(r"bitsPerRow: (\d+)")
CSS_LINE = re.compile(r'^  themeCSS: ".*"\n', re.M)
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
    """The label without a square it carried before."""
    for marker in MARKERS:
        if label.startswith(marker + " "):
            return label[len(marker) + 1:]
    return label


def important(declarations):
    """`declarations`, each ahead of the theme's own fill."""
    return " ".join(f"{part.strip()} !important;" for part in declarations.split(";"))


def theme_css(body, roles):
    """The CSS filling each box of a packet diagram by its field's role."""
    bits = int((BITS.search(body) or [None, DEFAULT_BITS_PER_ROW])[1])
    rows = {}
    for match, field_role in zip(FIELD.finditer(body), roles):
        start, _, end = match["at"].partition("-")
        start, end = int(start), int(end or start)
        for row in range(start // bits, end // bits + 1):
            rows.setdefault(row, []).append(field_role)
    rules = [f"g:nth-of-type({row + FIRST_ROW_GROUP}) rect:nth-of-type({place + 1}) {{ {important(FILLS[field_role])} }}"
             for row, in_row in sorted(rows.items()) for place, field_role in enumerate(in_row)]
    return " ".join(rules + [LABEL_CSS])


def filled(page):
    """The page with every packet diagram's boxes filled by role, and the
    diagrams whose fill or labels were wrong."""
    text = page.read_text(encoding="utf-8")
    defines = codes(page, text)
    legend = page.name == "README.md"
    wrong = []

    def block(match):
        body = match.group(1)
        if "packet-beta" not in body:
            return match.group(0)
        new = FIELD.sub(lambda field: f'{field["at"]}: "{unmarked(field["label"])}"', body)
        labels = [field["label"] for field in FIELD.finditer(new)]
        roles = [LEGEND[label] if legend else role(label, defines) for label in labels]
        new = CSS_LINE.sub("", new)
        if "\nconfig:\n" not in "\n" + new:
            raise ValueError(f"{page}: a packet diagram without a config block")
        new = new.replace("config:\n", f'config:\n  themeCSS: "{theme_css(new, roles)}"\n', 1)
        if new != body:
            title = re.search(r"^title (.*)$", body, re.M)
            wrong.append(f"{page.relative_to(ROOT).as_posix()}: {title[1] if title else 'a packet diagram'}")
        return f"```mermaid\n{new}```"

    return BLOCK.sub(block, text), wrong


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--write", action="store_true", help="write each diagram's themeCSS")
    parser.add_argument("pages", nargs="*", type=pathlib.Path)
    args = parser.parse_args()
    pages = [page.resolve() for page in args.pages] or sorted(PAGES.glob("*.md"))
    wrong = []
    for page in pages:
        text, found = filled(page)
        wrong += found
        if args.write and found:
            page.write_text(text, encoding="utf-8", newline="\n")
    if not args.write:
        print("\n".join(wrong))
    print(f"{len(wrong)} format diagram(s) whose boxes are not filled by role")
    return 1 if wrong and not args.write else 0


if __name__ == "__main__":
    sys.exit(main())
