"""Discover scene animation bindings from the C component and field tables."""
import pathlib
import re

from build.math_constants import blank_comments

MAIN = pathlib.Path(__file__).resolve().parents[2] / "main"
SCENE_FIELDS_EXTENSION = "AUTANA_scene_fields"
VALUE_TYPES = {name: index for index, name in enumerate(
    ("ANIM_VALUE_FLOAT", "ANIM_VALUE_VEC2", "ANIM_VALUE_VEC3", "ANIM_VALUE_QUAT", "ANIM_VALUE_COLOUR"))}
VALUE_FLOAT, VALUE_VEC2, VALUE_VEC3, VALUE_QUAT, VALUE_COLOUR = VALUE_TYPES.values()
WIDTHS = (1, 2, 3, 4, 3)

ASSET_TYPE_DEFINE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+ASSET_TYPE\s*\(\s*'(.)'\s*,\s*'(.)'\s*,\s*'(.)'\s*,\s*'(.)'\s*\)",
    re.MULTILINE)
COMPONENT_TABLE = re.compile(r"\banim_component_fields_t\s+\w+\s*=\s*\{\s*(\w+)\s*,\s*(\w+)\s*,")
FIELD_TABLE = re.compile(r"\banim_field_t\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\}", re.DOTALL)
FIELD_DECL = re.compile(r"\bANIM_FIELD\s*\(\s*[^,]+,\s*(\w+)\s*,\s*(\w+)\s*\)")


def discover(root=MAIN):
    """Return field names mapped to their (component 4cc, anim_value_t) pairs."""
    tags = {}
    for path in sorted(root.rglob("*.[ch]")):
        source = path.read_text(encoding="utf-8")
        if "ASSET_TYPE" in source:
            for name, *chars in ASSET_TYPE_DEFINE.findall(blank_comments(source)):
                tag = int.from_bytes("".join(chars).encode("ascii"), "little")
                if tags.setdefault(name, tag) != tag:
                    raise ValueError(f"{path}: conflicting definitions of {name}")
    fields = {}
    for path in sorted(root.rglob("*.c")):
        source = path.read_text(encoding="utf-8")
        if "anim_component_fields_t" not in source:
            continue
        source = blank_comments(source)
        tables = dict(FIELD_TABLE.findall(source))
        for define, table in COMPONENT_TABLE.findall(source):
            if define not in tags or table not in tables:
                raise ValueError(f"{path}: cannot resolve animation component {define} with field table {table}")
            tag = tags[define]
            code = tag.to_bytes(4, "little").decode("ascii")
            for field, value_type in FIELD_DECL.findall(tables[table]):
                if value_type not in VALUE_TYPES:
                    raise ValueError(f"{path}: {field} has unknown animation value type {value_type}")
                if field in fields and fields[field][0] != tag:
                    other = fields[field][0].to_bytes(4, "little").decode("ascii")
                    raise ValueError(f"{path}: field {field!r} is declared in {code} and {other}")
                fields[field] = (tag, VALUE_TYPES[value_type])
    return fields


FIELDS = discover()
COMPONENTS = frozenset(tag for tag, _ in FIELDS.values())
TRANSFORM_PATHS = {"position": "translation", "rotation": "rotation", "scale": "scale"}
