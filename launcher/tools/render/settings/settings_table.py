#!/usr/bin/env python3
"""Write the render settings table in docs/render/Render-Settings.md: one row
per field of render_settings_t, with the tier and trade its comment in
launcher/main/render/settings/render_settings.h states and the default
default.render.toml sets.

    python launcher/tools/render/settings/settings_table.py [--check]

A tier not in TIERS is an error.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from doc_tables import markdown  # noqa: E402
from generated_blocks import block_main  # noqa: E402
import render_asset  # noqa: E402

PAGE = ROOT / "docs/render/Render-Settings.md"
BLOCK = "render-settings"
COMMAND = "python launcher/tools/render/settings/settings_table.py --check"
TIERS = ("basic", "advanced")


def render(source=None, values=None):
    """The table of `source` (render_settings.h's text) with `values` (default.render.toml's)."""
    found = render_asset.fields(source)
    values = render_asset.load(render_asset.DEFAULT) if values is None else values
    rows = []
    for field, (_, tier, trade) in found.items():
        if tier not in TIERS:
            raise ValueError(f"{render_asset.HEADER.name}: {field}'s tier {tier!r} is not one of {', '.join(TIERS)}")
        rows.append((f"`{field}`", f"`{str(values[field]).lower()}`", tier, trade))
    return markdown(["Field", "Default", "Tier", "What it trades"], rows).rstrip()


def main(argv=None):
    return block_main(PAGE, BLOCK, COMMAND, render, __doc__, argv)


if __name__ == "__main__":
    sys.exit(main())
