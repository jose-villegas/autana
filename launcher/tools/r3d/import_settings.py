"""Reads and checks a mesh's import-settings file and a scene file that places meshes.

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


def load_variant(value, process, where):
    check_keys(value, ("name",), where, optional=("triangles", "face_samples"))
    variant = SimpleNamespace(name=text(value["name"], f"{where}.name"), triangles=None, face_samples=None)
    if process.simplify:
        if "triangles" not in value:
            raise SettingsError(f"{where}.triangles is required when process.simplify is present")
        variant.triangles = count(value["triangles"], f"{where}.triangles")
    elif "triangles" in value:
        raise SettingsError(f"{where}.triangles needs process.simplify")
    if "face_samples" in value:
        if not process.light:
            raise SettingsError(f"{where}.face_samples needs process.light")
        variant.face_samples = face_sample_options(value["face_samples"], f"{where}.face_samples")
    return variant


def load_process(process):
    """The opt-in steps: a step runs when its table is present."""
    check_keys(process, (), "process", optional=("seed", "alpha_mask", "visibility", "light", "simplify"))
    steps = SimpleNamespace(seed=integer(process.get("seed", 0), "process.seed"), alpha_keep=None, visibility=None,
                            light=None, simplify=None)
    if "alpha_mask" in process:
        check_keys(process["alpha_mask"], ("keep_alpha",), "process.alpha_mask")
        steps.alpha_keep = number(process["alpha_mask"]["keep_alpha"], "process.alpha_mask.keep_alpha")
    if "visibility" in process:
        table = process["visibility"]
        check_keys(table, ("rounds",), "process.visibility", optional=("thin",))
        steps.visibility = SimpleNamespace(rounds=count(table["rounds"], "process.visibility.rounds"),
                                           thin_material=None, thin_keep=None)
        if "thin" in table:
            check_keys(table["thin"], ("material", "keep"), "process.visibility.thin")
            steps.visibility.thin_material = text(table["thin"]["material"], "process.visibility.thin.material")
            steps.visibility.thin_keep = number(table["thin"]["keep"], "process.visibility.thin.keep")
    if "light" in process:
        table = process["light"]
        check_keys(table, ("ray_offset", "colour_merge_step"), "process.light", optional=("flat_sky_rays",))
        steps.light = SimpleNamespace(
            ray_offset=number(table["ray_offset"], "process.light.ray_offset"),
            colour_merge_step=count(table["colour_merge_step"], "process.light.colour_merge_step"),
            flat_sky_rays=count(table["flat_sky_rays"], "process.light.flat_sky_rays") if "flat_sky_rays" in table else None)
    if "simplify" in process:
        table = process["simplify"]
        check_keys(table, ("dense_edge", "props", "props_share", "seal_seams"), "process.simplify")
        steps.simplify = SimpleNamespace(
            dense_edge=number(table["dense_edge"], "process.simplify.dense_edge"),
            props=set(strings(table["props"], "process.simplify.props")),
            props_share=number(table["props_share"], "process.simplify.props_share"),
            seal_seams=boolean(table["seal_seams"], "process.simplify.seal_seams"))
    return steps


def load_import_settings(path):
    """One mesh asset: its source, output and opt-in processing. With only
    those, the mesh is imported as authored."""
    path = pathlib.Path(path).resolve()
    with open(path, "rb") as source:
        values = tomllib.load(source)
    check_keys(values, ("source", "output"), "settings", optional=("materials", "process", "variants"))
    source = values["source"]
    check_keys(source, ("url", "sha256", "path", "cache", "credit"), "source")
    for name in source:
        text(source[name], f"source.{name}")
    output = values["output"]
    check_keys(output, ("directory",), "output", optional=("name", "position_scale"))
    materials = values.get("materials", {})
    check_keys(materials, (), "materials", optional=("double_sided",))
    steps = load_process(values.get("process", {}))
    if "variants" in values:
        variants = values["variants"]
        if not isinstance(variants, list) or not variants:
            raise SettingsError("variants must be a non-empty array of tables")
        if "name" in output:
            raise SettingsError("output.name is for an import without variants; each variant names its own mesh")
        variants = [load_variant(variant, steps, f"variants[{index}]") for index, variant in enumerate(variants)]
        names = [variant.name for variant in variants]
        if len(set(names)) != len(names):
            raise SettingsError("variants names must be unique")
    else:
        if steps.simplify:
            raise SettingsError("process.simplify needs variants, each with its triangles budget")
        variants = [SimpleNamespace(name=text(output.get("name"), "output.name"), triangles=None, face_samples=None)]
    flat = any(variant.face_samples for variant in variants)
    if flat and steps.light.flat_sky_rays is None:
        raise SettingsError("process.light.flat_sky_rays is required when a variant has face_samples")
    if steps.light and not flat and steps.light.flat_sky_rays is not None:
        raise SettingsError("process.light.flat_sky_rays applies to a variant with face_samples only")
    return SimpleNamespace(
        path=path, source=source, out_dir=(path.parent / output["directory"]).resolve(),
        position_scale=count(output["position_scale"], "output.position_scale") if "position_scale" in output else None,
        double_sided=set(strings(materials.get("double_sided", []), "materials.double_sided")), seed=steps.seed,
        alpha_keep=steps.alpha_keep, visibility=steps.visibility, light=steps.light, simplify=steps.simplify,
        named=("variants" in values), variants=variants)


def load_renderer(value, base, where):
    check_keys(value, ("mesh",), where, optional=("variant", "position", "rotation", "scale"))
    path = (base / text(value["mesh"], f"{where}.mesh")).resolve()
    if not path.is_file():
        raise SettingsError(f"{where}.mesh {value['mesh']!r} is not a file")
    settings = load_import_settings(path)
    if settings.named:
        wanted = text(value.get("variant"), f"{where}.variant") if "variant" in value else None
        if wanted is None:
            raise SettingsError(f"{where}.variant is required: {value['mesh']!r} has variants")
        match = [variant for variant in settings.variants if variant.name == wanted]
        if not match:
            raise SettingsError(f"{where}.variant {wanted!r} is not in {value['mesh']!r}")
        variant = match[0]
    elif "variant" in value:
        raise SettingsError(f"{where}.variant: {value['mesh']!r} has no variants")
    else:
        variant = settings.variants[0]
    renderer = SimpleNamespace(
        settings=settings, variant=variant,
        position=vector(value["position"], f"{where}.position") if "position" in value else [0.0, 0.0, 0.0],
        rotation=vector(value["rotation"], f"{where}.rotation") if "rotation" in value else [0.0, 0.0, 0.0],
        scale=vector(value["scale"], f"{where}.scale") if "scale" in value else [1.0, 1.0, 1.0])
    identity = renderer.position == [0.0] * 3 and renderer.rotation == [0.0] * 3 and renderer.scale == [1.0] * 3
    if settings.light and not identity:
        raise SettingsError(f"{where}: a mesh with process.light is baked where it sits, so its transform must be identity")
    return renderer


def load_scene(path):
    """A scenario: the meshes it places, its lights, camera region, spawn and exposure."""
    path = pathlib.Path(path).resolve()
    with open(path, "rb") as source:
        values = tomllib.load(source)
    check_keys(values, ("mesh_renderers",), "scene", optional=("lights", "camera_region", "spawn", "exposure"))
    renderers = values["mesh_renderers"]
    if not isinstance(renderers, list) or not renderers:
        raise SettingsError("scene.mesh_renderers must be a non-empty array of tables")
    renderers = [load_renderer(renderer, path.parent, f"scene.mesh_renderers[{index}]")
                 for index, renderer in enumerate(renderers)]
    names = [renderer.variant.name for renderer in renderers]
    if len(set(names)) != len(names):
        raise SettingsError("scene.mesh_renderers bake a mesh name twice")
    lights = values.get("lights", [])
    if not isinstance(lights, list):
        raise SettingsError("scene.lights must be an array")
    region = values.get("camera_region")
    if region is not None:
        check_keys(region, ("min", "max"), "scene.camera_region")
        region = (vector(region["min"], "scene.camera_region.min"), vector(region["max"], "scene.camera_region.max"))
    spawn = values.get("spawn", {})
    check_keys(spawn, (), "scene.spawn", optional=("position", "rotation"))
    scene = SimpleNamespace(
        path=path, renderers=renderers, region=region,
        lights=[load_light(light, f"scene.lights[{index}]") for index, light in enumerate(lights)],
        exposure=number(values["exposure"], "scene.exposure") if "exposure" in values else None,
        spawn=SimpleNamespace(position=vector(spawn["position"], "scene.spawn.position") if "position" in spawn else [0.0] * 3,
                              rotation=vector(spawn["rotation"], "scene.spawn.rotation") if "rotation" in spawn else [0.0] * 3))
    for renderer in renderers:
        if renderer.settings.light and not scene.lights:
            raise SettingsError("scene.lights is required: a mesh places with process.light")
        if renderer.settings.light and scene.exposure is None:
            raise SettingsError("scene.exposure is required: a mesh places with process.light")
        if renderer.settings.visibility and region is None:
            raise SettingsError("scene.camera_region is required: a mesh places with process.visibility")
    return scene
