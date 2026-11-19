"""A NAME.render.toml as its author wrote it: its tables, flattened to `table.key`, with no check against
render_settings_t. render_asset.py checks and bakes them; a tool that needs one default value reads it here,
without the table encoder or the settings header in its closure.
"""
import pathlib
import tomllib

DEFAULT = pathlib.Path(__file__).resolve().parents[3] / "main/render/settings/default.render.toml"
# default.render.toml's table of field ranges, which sets no value.
RANGES = "ranges"


def flatten(values, prefix=""):
    """{key: value} of a TOML table, a nested table's keys as `table.key`."""
    flat = {}
    for key, value in values.items():
        flat.update(flatten(value, f"{prefix}{key}.") if isinstance(value, dict) else {prefix + key: value})
    return flat


def tables(path):
    """The file's TOML tables."""
    return tomllib.loads(pathlib.Path(path).read_text(encoding="utf-8"))


def default_value(key):
    """What default.render.toml sets `key` to."""
    declared = tables(DEFAULT)
    declared.pop(RANGES, None)
    return flatten(declared)[key]
