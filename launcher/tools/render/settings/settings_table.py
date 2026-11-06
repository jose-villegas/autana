#!/usr/bin/env python3
"""Write the render settings table in docs/render/Render-Settings.md from
launcher/main/render/settings/render_settings.h: one row per field of
render_settings_t, with the default, tier and trade its RENDER_SETTINGS_<FIELD>
define states.

    python launcher/tools/render/settings/settings_table.py [--check]

A field without a define, or a define without a known tier, is an error.
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
from generated_blocks import replace_block  # noqa: E402

HEADER = ROOT / "launcher/main/render/settings/render_settings.h"
PAGE = ROOT / "docs/render/Render-Settings.md"
BLOCK = "render-settings"
COMMAND = "python launcher/tools/render/settings/settings_table.py --check"
TIERS = ("basic", "advanced")
STRUCT = re.compile(r"typedef struct \{(.*?)\} render_settings_t;", re.S)
DEFINE = re.compile(r"^#define RENDER_SETTINGS_([A-Z_]+)\s+(\S+)\s+/\* (\w+): (.*?) \*/$", re.M)


def fields(source):
    """The struct's field names, in order."""
    found = STRUCT.search(source)
    if found is None:
        raise ValueError(f"{HEADER.name}: no render_settings_t")
    declarations = re.sub(r"/\*.*?\*/", "", found.group(1), flags=re.S)
    return [name for line in declarations.split(";") for name in re.findall(r"\w+", line)[1:]]


def render(source):
    defines = {name.lower(): rest for name, *rest in DEFINE.findall(source)}
    lines = ["| Field | Default | Tier | What it trades |", "|---|---|---|---|"]
    for field in fields(source):
        if field not in defines:
            raise ValueError(f"{HEADER.name}: {field} has no RENDER_SETTINGS_{field.upper()} with a tier comment")
        value, tier, trade = defines[field]
        if tier not in TIERS:
            raise ValueError(f"{HEADER.name}: {field}'s tier {tier!r} is not one of {', '.join(TIERS)}")
        lines.append(f"| `{field}` | `{value}` | {tier} | {trade} |")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        changed = replace_block(PAGE, BLOCK, render(HEADER.read_text(encoding="utf-8")), check=args.check,
                                command=COMMAND)
        if args.check and changed:
            raise ValueError(f"{PAGE.name}#{BLOCK} is stale: run {COMMAND.replace(' --check', '')}")
    except (OSError, ValueError) as error:
        parser.exit(1, f"settings_table: {error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
