"""Reads and checks a mesh's import-settings file and its scene file.

Standard library only, so a settings error is reported, and tested, without the
numeric environment the bake itself needs. Every table is closed: a key nobody
reads is an error.
"""

import math
import pathlib
import tomllib
from types import SimpleNamespace

RESERVED_LIGHTS = ("point", "spot")

# The one declaration of each light type's fields; light.py pairs each with
# the function that bakes it.
LIGHT_FIELDS = {
    "directional": {"direction": "vector", "color": "vector", "intensity": "number", "disc_degrees": "number",
                    "rays": "count"},
    "sky": {"color": "vector", "intensity": "number", "rays": "count"},
    "ambient": {"color": "vector", "intensity": "number"},
}


class SettingsError(ValueError):
    """An import or scene setting cannot describe a bake."""


def check_keys(table, required, where, optional=()):
    """Every required key is present and nothing else but the optional ones."""
    if not isinstance(table, dict):
        raise SettingsError(f"{where} must be a table")
    for name in required:
        if name not in table:
            raise SettingsError(f"{where}.{name} is required")
    for name in table:
        if name not in required and name not in optional:
            raise SettingsError(f"{where}.{name} is not a known setting")


def text(value, where):
    if not isinstance(value, str) or not value:
        raise SettingsError(f"{where} must be a non-empty string")
    return value


def strings(value, where):
    if not isinstance(value, list):
        raise SettingsError(f"{where} must be an array of strings")
    return [text(item, f"{where}[{index}]") for index, item in enumerate(value)]


def boolean(value, where):
    if not isinstance(value, bool):
        raise SettingsError(f"{where} must be true or false")
    return value


def number(value, where):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise SettingsError(f"{where} must be a number")
    return float(value)


def integer(value, where):
    if isinstance(value, bool) or not isinstance(value, int):
        raise SettingsError(f"{where} must be an integer")
    return value


def count(value, where):
    value = integer(value, where)
    if value < 1:
        raise SettingsError(f"{where} must be at least 1")
    return value


def vector(value, where):
    if not isinstance(value, list) or len(value) != 3:
        raise SettingsError(f"{where} must be a three-component array")
    return [number(component, f"{where}[{index}]") for index, component in enumerate(value)]


READERS = {"vector": vector, "number": number, "count": count}


def load_light(value, where):
    kind = value.get("type") if isinstance(value, dict) else None
    if kind in RESERVED_LIGHTS:
        raise SettingsError(f"{where}.type {kind!r} is reserved for a later bake")
    if kind not in LIGHT_FIELDS:
        raise SettingsError(f"{where}.type must be one of {sorted(LIGHT_FIELDS)}")
    fields = LIGHT_FIELDS[kind]
    check_keys(value, ("type", *fields), where)
    light = {"type": kind}
    for name, reader in fields.items():
        light[name] = READERS[reader](value[name], f"{where}.{name}")
    if "direction" in light and not any(light["direction"]):
        raise SettingsError(f"{where}.direction must not be zero")
    return light


def load_scene(path):
    """The lights, the camera region the visibility cull samples and the tonemap."""
    with open(path, "rb") as source:
        scene = tomllib.load(source)
    check_keys(scene, ("lights", "camera_region", "tonemap_white"), "scene")
    lights = scene["lights"]
    if not isinstance(lights, list):
        raise SettingsError("scene.lights must be an array")
    region = scene["camera_region"]
    check_keys(region, ("min", "max"), "scene.camera_region")
    return SimpleNamespace(
        lights=[load_light(light, f"scene.lights[{index}]") for index, light in enumerate(lights)],
        lo=vector(region["min"], "scene.camera_region.min"), hi=vector(region["max"], "scene.camera_region.max"),
        tonemap_white=number(scene["tonemap_white"], "scene.tonemap_white"))


def face_sample_options(value, where):
    """(samples, min, max, area): a count per face, or "auto" from min to max
    with one sample per `area` of face (None for the mesh's median face)."""
    if not isinstance(value, dict) or len(value) != 1:
        raise SettingsError(f"{where} must contain exactly fixed or auto")
    if "fixed" in value:
        fixed = count(value["fixed"], f"{where}.fixed")
        return fixed, 1, fixed, None
    if "auto" not in value:
        raise SettingsError(f"{where} must contain exactly fixed or auto")
    auto = value["auto"]
    check_keys(auto, ("min", "max", "area"), f"{where}.auto")
    minimum, maximum = count(auto["min"], f"{where}.auto.min"), count(auto["max"], f"{where}.auto.max")
    if minimum > maximum:
        raise SettingsError(f"{where}.auto min must be no greater than max")
    area = auto["area"]
    if area != "median" and number(area, f"{where}.auto.area") <= 0:
        raise SettingsError(f"{where}.auto.area must be positive or 'median'")
    return "auto", minimum, maximum, None if area == "median" else float(area)


def load_variant(value, where):
    check_keys(value, ("name", "flat", "triangles"), where, optional=("face_samples",))
    flat = boolean(value["flat"], f"{where}.flat")
    variant = SimpleNamespace(name=text(value["name"], f"{where}.name"), flat=flat, face_samples=None,
                              triangles=count(value["triangles"], f"{where}.triangles"))
    if flat:
        if "face_samples" not in value:
            raise SettingsError(f"{where}.face_samples is required for a flat variant")
        variant.face_samples = face_sample_options(value["face_samples"], f"{where}.face_samples")
    elif "face_samples" in value:
        raise SettingsError(f"{where}.face_samples applies to a flat variant only")
    return variant


def load_import_settings(path):
    path = pathlib.Path(path).resolve()
    with open(path, "rb") as source:
        values = tomllib.load(source)
    check_keys(values, ("scene", "source", "output", "materials", "options", "variants"), "settings")
    source = values["source"]
    check_keys(source, ("url", "sha256", "path", "cache", "credit"), "source")
    for name in source:
        text(source[name], f"source.{name}")
    check_keys(values["output"], ("directory",), "output")
    materials = values["materials"]
    check_keys(materials, ("double_sided", "leaf_material", "props"), "materials")
    options = values["options"]
    check_keys(options, ("mask_keep_alpha", "visibility_rounds", "leaf_keep", "seed", "ray_offset", "position_scale",
                         "colour_merge_step", "dense_edge", "props_share", "seal_seams"), "options",
               optional=("flat_sky_rays",))
    variants = values["variants"]
    if not isinstance(variants, list) or not variants:
        raise SettingsError("variants must be a non-empty array of tables")
    variants = [load_variant(variant, f"variants[{index}]") for index, variant in enumerate(variants)]
    names = [variant.name for variant in variants]
    if len(set(names)) != len(names):
        raise SettingsError("variants names must be unique")
    flat = any(variant.flat for variant in variants)
    if flat and "flat_sky_rays" not in options:
        raise SettingsError("options.flat_sky_rays is required when a variant is flat")
    if not flat and "flat_sky_rays" in options:
        raise SettingsError("options.flat_sky_rays applies to a flat variant only")
    scene = load_scene(path.parent / text(values["scene"], "scene"))
    return SimpleNamespace(
        path=path, source=source, out_dir=(path.parent / values["output"]["directory"]).resolve(), scene=scene,
        double_sided=set(strings(materials["double_sided"], "materials.double_sided")),
        props=set(strings(materials["props"], "materials.props")),
        leaf_material=text(materials["leaf_material"], "materials.leaf_material"),
        mask_keep_alpha=number(options["mask_keep_alpha"], "options.mask_keep_alpha"),
        visibility_rounds=count(options["visibility_rounds"], "options.visibility_rounds"),
        leaf_keep=number(options["leaf_keep"], "options.leaf_keep"), seed=integer(options["seed"], "options.seed"),
        ray_offset=number(options["ray_offset"], "options.ray_offset"),
        position_scale=count(options["position_scale"], "options.position_scale"),
        colour_merge_step=count(options["colour_merge_step"], "options.colour_merge_step"),
        flat_sky_rays=count(options["flat_sky_rays"], "options.flat_sky_rays") if flat else None,
        dense_edge=number(options["dense_edge"], "options.dense_edge"),
        props_share=number(options["props_share"], "options.props_share"),
        seal_seams=boolean(options["seal_seams"], "options.seal_seams"), variants=variants)
