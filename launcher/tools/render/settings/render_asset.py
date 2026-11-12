#!/usr/bin/env python3
"""A NAME.render.toml: render settings, baked into a pack's TABL entry
RENDER_SETTINGS_ENTRY, one row per field, and the source of
RENDER_SETTINGS_DEFAULT.

    python tools/render/settings/render_asset.py main/render/settings/render_settings_default.h

The keys are render_settings_t's fields (launcher/main/render/settings/
render_settings.h), a field of a struct field under that struct's table, as
`[lod]` for `lod`. default.render.toml, in the same folder as that header,
sets every one; another file lists only the values it changes, and the rest
come from the default. An unknown key or a value of another type than its
field is an error. A table row holds a float in 1/2^RENDER_SETTINGS_FRAC_BITS
steps, so it must be a multiple of one, and a bool or an int as itself.

Run as above, it writes the header that holds RENDER_SETTINGS_DEFAULT.
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
FRAC_BITS = read_define(HEADER, "RENDER_SETTINGS_FRAC_BITS")
STRUCTS = re.compile(r"typedef struct \{(.*?)\} (\w+);", re.S)
FIELD = re.compile(r"^\s*(\w+) (\w+);\s*/\*(.*?)\*/$", re.M)
TIER = re.compile(r"^ (\w+): (.*) $")
TYPES = {"bool": (bool,), "int": (int,), "float": (int, float)}
COMMAND = "python tools/render/settings/render_asset.py main/render/settings/render_settings_default.h"


def fields(source=None):
    """{field: (C type, tier, trade)} of render_settings_t in order, a struct field's own
    as `field.member`. Each is declared on its own line with its tier comment."""
    source = HEADER.read_text(encoding="utf-8") if source is None else source
    structs = {}
    for body, name in STRUCTS.findall(source):
        rows = FIELD.findall(body.strip())
        if len(rows) != len([line for line in body.strip().splitlines() if line.strip()]):
            raise ValueError(f"{HEADER.name}: declare each field on its own line with a /* tier: trade */ comment")
        structs[name] = rows

    def expand(name, prefix):
        found = {}
        for kind, field, comment in structs[name]:
            if kind in structs:
                found.update(expand(kind, f"{prefix}{field}."))
                continue
            tier = TIER.match(comment)
            if kind not in TYPES or tier is None:
                raise ValueError(f"{HEADER.name}: {prefix}{field} needs a bool, int or float and /* tier: trade */")
            found[prefix + field] = (kind, *tier.groups())
        return found

    if "render_settings_t" not in structs:
        raise ValueError(f"{HEADER.name}: no render_settings_t")
    return expand("render_settings_t", "")


def flatten(values, prefix=""):
    """{key: value} of a TOML table, a nested table's keys as `table.key`."""
    flat = {}
    for key, value in values.items():
        flat.update(flatten(value, f"{prefix}{key}.") if isinstance(value, dict) else {prefix + key: value})
    return flat


def read(path):
    """The validated keys of one settings file, flattened."""
    path = pathlib.Path(path)
    try:
        declared = tomllib.loads(path.read_text(encoding="utf-8"))
        if path.resolve() == DEFAULT.resolve():
            declared.pop("ranges", None)
        values = flatten(declared)
    except tomllib.TOMLDecodeError as error:
        raise ValueError(f"{path}: {error}") from None
    known = fields()
    unknown = values.keys() - known.keys()
    if unknown:
        raise ValueError(f"{path}: {', '.join(sorted(unknown))} is not a render setting")
    for key, value in values.items():
        kind = known[key][0]
        if type(value) not in TYPES[kind]:
            raise ValueError(f"{path}: {key} must be a TOML {kind}")
    return values


def load(path):
    """Every field's value: `path`'s keys over default.render.toml's, in struct order."""
    default = read(DEFAULT)
    missing = fields().keys() - default.keys()
    if missing:
        raise ValueError(f"{DEFAULT}: sets no {', '.join(sorted(missing))}")
    values = default | read(path)
    return {key: values[key] for key in fields()}


def literal(kind, value, suffix=""):
    """`value` as C source of its field's type, `suffix` after a float."""
    return str(value).lower() if kind == "bool" else repr(float(value)) + suffix if kind == "float" else str(value)


def nested(values, kinds, prefix=""):
    """The C designated initialiser body of the flat `values`, a struct field's as a nested one."""
    parts = []
    for head in dict.fromkeys(key[len(prefix):].partition(".")[0] for key in values if key.startswith(prefix)):
        key = prefix + head
        parts.append(f".{head} = " + (literal(kinds[key], values[key], "F") if key in values
                                      else "{" + nested(values, kinds, key + ".") + "}"))
    return ", ".join(parts)


def bake(path):
    """The RENDER_SETTINGS_ENTRY bytes of a settings file."""
    rows = {}
    kinds = fields()
    for key, value in load(path).items():
        scale = 1 << FRAC_BITS if kinds[key][0] == "float" else 1
        steps = float(value) * scale
        if steps != int(steps) or not table_asset.VALUE_MIN <= steps <= table_asset.VALUE_MAX:
            raise ValueError(f"{key} = {value}: not a multiple of 1/{scale} "
                             f"from {table_asset.VALUE_MIN / scale:g} to {table_asset.VALUE_MAX / scale:g}")
        rows[key] = [int(steps)]
    return table_asset.encode(rows, FRAC_BITS)


def field_ranges():
    """The default file's optional bounds, keyed by a field's member path."""
    ranges = tomllib.loads(DEFAULT.read_text(encoding="utf-8")).get("ranges", {})
    for field, bounds in ranges.items():
        if field not in fields() or not isinstance(bounds, dict):
            raise ValueError(f"{DEFAULT}: unknown range field {field}")
        if bounds.keys() - {"min", "max", "min_exclusive", "max_exclusive"}:
            raise ValueError(f"{DEFAULT}: {field} has an unknown range bound")
    return ranges


def range_predicate(kind, bounds):
    """A field's C validity expression over its typed `value`; no bounds accepts any value."""
    checks = []
    for edge, comparison in (("min", ">"), ("max", "<")):
        if edge not in bounds:
            continue
        bound = bounds[edge]
        if isinstance(bound, str):
            if not re.fullmatch(r"[A-Z][A-Z0-9_]*", bound):
                raise ValueError(f"invalid C range constant {bound!r}")
            limit = bound
        else:
            limit = literal(kind, bound, "F")
        operator = comparison + ("" if bounds.get(f"{edge}_exclusive", False) else "=")
        checks.append(f"value {operator} {limit}")
    return " && ".join(checks) or "true"


def header():
    """render_settings_default.h's text."""
    values, known = load(DEFAULT), fields()
    kinds = {key: kind for key, (kind, _, _) in known.items()}
    ranges = field_ranges()
    declarations = " ".join(f"X({key}, {kinds[key]}, {range_predicate(kinds[key], ranges.get(key, {}))})" for key in values)
    return (f"/* GENERATED FILE - do not edit.\n *     {COMMAND}\n */\n#pragma once\n\n"
            f"#define RENDER_SETTINGS_DEFAULT ((render_settings_t){{{nested(values, kinds)}}})\n"
            "/* X(field, type, valid) in struct order; valid tests the typed field as `value`. */\n"
            f"#define RENDER_SETTINGS_FIELDS(X) {declarations}\n"
            f"#define RENDER_SETTINGS_FIELD_COUNT {len(values)}\n")


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    pathlib.Path(argv[0]).write_text(header(), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
