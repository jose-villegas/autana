"""Discover scene animation bindings from the C component and field tables."""
import functools
import pathlib
import re

from build.c_enum import read_define, read_enum, tag_text
from build.math_constants import INTEGER_DEFINE, blank_comments

MAIN = pathlib.Path(__file__).resolve().parents[2] / "main"
TRACK_HEADER = MAIN / "anim/anim_track.h"
TRACK_FORMAT_HEADER = MAIN / "anim/anim_tracks_format.h"
LENS_HEADER = MAIN / "scene/camera/lens.h"
SCENE_FIELDS_EXTENSION = "AUTANA_scene_fields"
VALUE_TYPES = read_enum(TRACK_HEADER, "anim_value_t")
VALUE_FLOAT = VALUE_TYPES["ANIM_VALUE_FLOAT"]
VALUE_VEC2 = VALUE_TYPES["ANIM_VALUE_VEC2"]
VALUE_VEC3 = VALUE_TYPES["ANIM_VALUE_VEC3"]
VALUE_QUAT = VALUE_TYPES["ANIM_VALUE_QUAT"]
VALUE_COLOUR = VALUE_TYPES["ANIM_VALUE_COLOUR"]
WIDTHS = read_define(TRACK_HEADER, "ANIM_VALUE_WIDTHS")
CAMERA = read_define(LENS_HEADER, "ANIM_COMPONENT_CAMERA")
TRANSFORM = read_define(TRACK_FORMAT_HEADER, "ANIM_COMPONENT_TRANSFORM")

COMPONENT_DEFINITION = r"\banim_component_fields_t\s+\w+\s*="
COMPONENT_TABLE = re.compile(COMPONENT_DEFINITION + r"\s*\{\s*(\w+)\s*,\s*(\w+)\s*,")
FIELD_TABLE = re.compile(r"\banim_field_t\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\}", re.DOTALL)
FIELD_MACROS = {"ANIM_FIELD": False, "ANIM_FIELD_STEP": True}
FIELD_CALL = r"\b(" + "|".join(FIELD_MACROS) + r")\s*\("
FIELD_DECL = re.compile(FIELD_CALL + r"\s*\w+\s*,\s*(\w+)\s*,\s*(\w+)\s*\)")
TRANSFORM_FIELDS = {"position": "translation", "rotation": "rotation", "scale": "scale"}


def discover(root=MAIN):
    """Map (component tag, field name) to (value type, step only) from C."""
    tags, sources = {}, {}
    for path in sorted(root.rglob("*.[ch]")):
        source = path.read_text(encoding="utf-8")
        if "ASSET_TYPE" in source or "anim_component_fields_t" in source or "ANIM_FIELD" in source:
            sources[path] = blank_comments(source)
    for path, source in sources.items():
        for definition in INTEGER_DEFINE.finditer(source):
            name, value = definition.groups()
            if re.match(r"ASSET_TYPE\s*\(", value):
                tag = read_define(path, name)
                if tags.setdefault(name, tag) != tag:
                    raise ValueError(f"{path}: conflicting definitions of {name}")
    fields = {}
    for path, source in sources.items():
        if path.suffix != ".c":
            continue
        components = COMPONENT_TABLE.findall(source)
        if len(components) != len(re.findall(COMPONENT_DEFINITION, source)):
            raise ValueError(f"{path}: unsupported animation component initializer")
        tables = dict(FIELD_TABLE.findall(source))
        matched = {}
        for define, table in components:
            if define not in tags or table not in tables:
                raise ValueError(f"{path}: cannot resolve animation component {define} with field table {table}")
            tag = tags[define]
            declarations = FIELD_DECL.findall(tables[table])
            if len(declarations) != len(re.findall(FIELD_CALL, tables[table])):
                raise ValueError(f"{path}: unsupported animation field declaration in {table}")
            matched[table] = len(declarations)
            for macro, field, value_type in declarations:
                if value_type not in VALUE_TYPES:
                    raise ValueError(f"{path}: {field} has unknown animation value type {value_type}")
                if (tag, field) in fields:
                    code = tag_text(tag)
                    raise ValueError(f"{path}: duplicate animation field {code}.{field}")
                fields[tag, field] = (VALUE_TYPES[value_type], FIELD_MACROS[macro])
        if sum(matched.values()) != len(re.findall(FIELD_CALL, source)):
            raise ValueError(f"{path}: animation field declaration outside a resolved component table")
    return fields


@functools.cache
def fields():
    """The scene field table for this invocation of the tool."""
    return discover(MAIN)


def bake_inputs():
    """The component tags and field declarations used by baked track bindings."""
    return CAMERA, TRANSFORM, sorted(fields().items())
