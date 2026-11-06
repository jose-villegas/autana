#!/usr/bin/env python3
"""A NAME.render.toml: render settings, baked into a pack's TABL entry
RENDER_SETTINGS_ENTRY, one row per field, and the source of
RENDER_SETTINGS_DEFAULT.

    python tools/render/settings/render_asset.py main/render/settings/render_settings_default.h

The keys are render_settings_t's fields (launcher/main/render/settings/
render_settings.h). default.render.toml beside it sets every one; another file
lists only the values it changes, and the rest come from the default. An
unknown key or a value of another type than its field is an error. Run as
above, it writes the header that holds RENDER_SETTINGS_DEFAULT.
"""
import pathlib
import re
import sys
import tomllib

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
from asset import table_asset  # noqa: E402
from build.c_header import read_define  # noqa: E402

SETTINGS = TOOLS.parent / "main/render/settings"
HEADER = SETTINGS / "render_settings.h"
DEFAULT = SETTINGS / "default.render.toml"
SUFFIX = ".render.toml"
TYPE = table_asset.TYPE
ENTRY = read_define(HEADER, "RENDER_SETTINGS_ENTRY")
STRUCT = re.compile(r"typedef struct \{(.*?)\} render_settings_t;", re.S)
FIELD = re.compile(r"^\s*(\w+) (\w+); +/\* (\w+): (.*?) \*/$", re.M)
TYPES = {"bool": bool}
COMMAND = "python tools/render/settings/render_asset.py main/render/settings/render_settings_default.h"


def fields(source=None):
    """{field: (C type, tier, trade)} of render_settings_t in order, each declared
    on its own line with its tier comment."""
    source = HEADER.read_text(encoding="utf-8") if source is None else source
    found = STRUCT.search(source)
    if found is None:
        raise ValueError(f"{HEADER.name}: no render_settings_t")
    body = found.group(1).strip()
    rows = FIELD.findall(body)
    if len(rows) != len([line for line in body.splitlines() if line.strip()]):
        raise ValueError(f"{HEADER.name}: declare each field on its own line with a /* tier: trade */ comment")
    return {name: (kind, tier, trade) for kind, name, tier, trade in rows}


def read(path):
    """The validated keys of one settings file."""
    path = pathlib.Path(path)
    try:
        values = tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as error:
        raise ValueError(f"{path}: {error}") from None
    known = fields()
    unknown = values.keys() - known.keys()
    if unknown:
        raise ValueError(f"{path}: {', '.join(sorted(unknown))} is not a render setting")
    for key, value in values.items():
        if type(value) is not TYPES[known[key][0]]:
            raise ValueError(f"{path}: {key} must be a TOML {TYPES[known[key][0]].__name__}")
    return values


def load(path):
    """Every field's value: `path`'s keys over default.render.toml's, in struct order."""
    default = read(DEFAULT)
    missing = fields().keys() - default.keys()
    if missing:
        raise ValueError(f"{DEFAULT}: sets no {', '.join(sorted(missing))}")
    values = default | read(path)
    return {key: values[key] for key in fields()}


def bake(path):
    """The RENDER_SETTINGS_ENTRY bytes of a settings file."""
    return table_asset.encode({key: [int(value)] for key, value in load(path).items()}, 0)


def header():
    """render_settings_default.h's text."""
    values = load(DEFAULT)
    initial = ", ".join(f".{key} = {str(value).lower()}" for key, value in values.items())
    return (f"/* GENERATED FILE - do not edit.\n *     {COMMAND}\n */\n#pragma once\n\n"
            f"#define RENDER_SETTINGS_DEFAULT ((render_settings_t){{{initial}}})\n"
            "/* X(field) for each field of render_settings_t, in order. */\n"
            f"#define RENDER_SETTINGS_FIELDS(X) {' '.join(f'X({key})' for key in values)}\n")


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    pathlib.Path(argv[0]).write_text(header(), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
