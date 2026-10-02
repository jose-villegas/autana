"""Reads and checks a mesh's import-settings file and a scene file of objects.

Standard library only, so a settings error is reported, and tested, without the
numeric environment the bake itself needs. Every table is closed: a key nobody
reads is an error.
"""

import math
import pathlib
import re
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


def identifier(value, where):
    """A C identifier, since the scene table names symbols by it."""
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value):
        raise SettingsError(f"{where} must be a C identifier")
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


def nonnegative_count(value, where):
    value = integer(value, where)
    if value < 0:
        raise SettingsError(f"{where} must not be negative")
    return value


def colour_rgb(value, where):
    """A colour as 0xRRGGBB."""
    value = integer(value, where)
    if not 0 <= value <= 0xFFFFFF:
        raise SettingsError(f"{where} must be 0xRRGGBB")
    return value


def vector(value, where):
    if not isinstance(value, list) or len(value) != 3:
        raise SettingsError(f"{where} must be a three-component array")
    return [number(component, f"{where}[{index}]") for index, component in enumerate(value)]


READERS = {"vector": vector, "number": number, "count": count}


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
    check_keys(process, (), "process", optional=("seed", "alpha_mask", "visibility", "thin", "light", "simplify"))
    steps = SimpleNamespace(seed=0, alpha_keep=None, visibility=None, thin=None, light=None, simplify=None)
    if "alpha_mask" in process:
        check_keys(process["alpha_mask"], ("keep_alpha",), "process.alpha_mask")
        steps.alpha_keep = number(process["alpha_mask"]["keep_alpha"], "process.alpha_mask.keep_alpha")
    if "visibility" in process:
        check_keys(process["visibility"], ("rounds",), "process.visibility")
        steps.visibility = SimpleNamespace(rounds=count(process["visibility"]["rounds"], "process.visibility.rounds"))
    if "thin" in process:
        check_keys(process["thin"], ("material", "keep"), "process.thin")
        steps.thin = SimpleNamespace(material=text(process["thin"]["material"], "process.thin.material"),
                                     keep=number(process["thin"]["keep"], "process.thin.keep"))
    if "light" in process:
        table = process["light"]
        check_keys(table, ("ray_offset", "colour_merge_step"), "process.light", optional=("flat_sky_rays", "indirect"))
        steps.light = SimpleNamespace(
            ray_offset=number(table["ray_offset"], "process.light.ray_offset"),
            colour_merge_step=count(table["colour_merge_step"], "process.light.colour_merge_step"),
            flat_sky_rays=count(table["flat_sky_rays"], "process.light.flat_sky_rays") if "flat_sky_rays" in table else None,
            indirect=None)
        if "indirect" in table:
            indirect = table["indirect"]
            check_keys(indirect, ("bounces", "rays", "cache_samples"), "process.light.indirect")
            steps.light.indirect = SimpleNamespace(
                bounces=nonnegative_count(indirect["bounces"], "process.light.indirect.bounces"),
                rays=count(indirect["rays"], "process.light.indirect.rays"),
                cache_samples=count(indirect["cache_samples"], "process.light.indirect.cache_samples"))
    if "simplify" in process:
        table = process["simplify"]
        check_keys(table, ("dense_edge", "props", "props_share", "seal_seams"), "process.simplify")
        steps.simplify = SimpleNamespace(
            dense_edge=number(table["dense_edge"], "process.simplify.dense_edge"),
            props=set(strings(table["props"], "process.simplify.props")),
            props_share=number(table["props_share"], "process.simplify.props_share"),
            seal_seams=boolean(table["seal_seams"], "process.simplify.seal_seams"))
    if "seed" in process:
        if not (steps.visibility or steps.thin or steps.light):
            raise SettingsError("process.seed needs a step that draws random rays: visibility, thin or light")
        steps.seed = integer(process["seed"], "process.seed")
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
    directory = text(output["directory"], "output.directory")
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
        shapes = {}
        for variant in variants:
            shape = (variant.triangles, repr(variant.face_samples))
            if shape in shapes:
                raise SettingsError(f"variants {shapes[shape]!r} and {variant.name!r} would produce the same mesh")
            shapes[shape] = variant.name
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
        path=path, source=source, out_dir=(path.parent / directory).resolve(), mesh_dir=path.parent,
        position_scale=count(output["position_scale"], "output.position_scale") if "position_scale" in output else None,
        double_sided=set(strings(materials.get("double_sided", []), "materials.double_sided")), seed=steps.seed,
        alpha_keep=steps.alpha_keep, visibility=steps.visibility, thin=steps.thin, light=steps.light,
        simplify=steps.simplify, scene_dependent=bool(steps.light or steps.visibility), named=("variants" in values),
        variants=variants)


def rotation_matrix(degrees):
    """Rows of the rotation for Euler angles [pitch, yaw, roll] in degrees:
    right-handed, applied roll about z first, then pitch about x, then yaw
    about y."""
    pitch, yaw, roll = (math.radians(angle) for angle in degrees)
    sx, cx, sy, cy, sz, cz = math.sin(pitch), math.cos(pitch), math.sin(yaw), math.cos(yaw), math.sin(roll), math.cos(roll)
    rx = ((1, 0, 0), (0, cx, -sx), (0, sx, cx))
    ry = ((cy, 0, sy), (0, 1, 0), (-sy, 0, cy))
    rz = ((cz, -sz, 0), (sz, cz, 0), (0, 0, 1))

    def product(a, b):
        return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))

    return product(ry, product(rx, rz))


def placement_matrix(rotation, scale):
    """Rotation times scale, the 3x3 a placed mesh's model units go through,
    as rows. The one place the rotation convention is written down."""
    matrix = rotation_matrix(rotation)
    return tuple(tuple(matrix[i][j] * scale[j] for j in range(3)) for i in range(3))


def rotate(degrees, vector_):
    matrix = rotation_matrix(degrees)
    return [sum(matrix[i][j] * vector_[j] for j in range(3)) for i in range(3)]


def load_environment_light(kind, table, where):
    """Sky and ambient are scene settings, not objects."""
    fields = LIGHT_FIELDS[kind]
    check_keys(table, tuple(fields), where)
    light = {"type": kind}
    for name, reader in fields.items():
        light[name] = READERS[reader](table[name], f"{where}.{name}")
    return light


def load_light(component, rotation, where):
    """A directional light; the direction toward it is the object's +Y axis, turned by its rotation."""
    kind = component.get("type") if isinstance(component, dict) else None
    if kind in RESERVED_LIGHTS:
        raise SettingsError(f"{where}.type {kind!r} is reserved for a later bake")
    if kind != "directional":
        raise SettingsError(f"{where}.type must be directional: sky and ambient are scene settings")
    fields = {name: reader for name, reader in LIGHT_FIELDS[kind].items() if name != "direction"}
    check_keys(component, ("type", *fields), where)
    light = {"type": kind, "direction": rotate(rotation, [0.0, 1.0, 0.0])}
    for name, reader in fields.items():
        light[name] = READERS[reader](component[name], f"{where}.{name}")
    return light


def load_camera(component, where):
    check_keys(component, ("half_fov_short_tan", "near_z"), where, optional=("region", "path", "background"))
    camera = SimpleNamespace(half_fov_short_tan=number(component["half_fov_short_tan"], f"{where}.half_fov_short_tan"),
                             near_z=number(component["near_z"], f"{where}.near_z"), region=None, path=None,
                             background=colour_rgb(component.get("background", 0), f"{where}.background"))
    if "region" in component:
        region = component["region"]
        check_keys(region, ("min", "max"), f"{where}.region")
        camera.region = (vector(region["min"], f"{where}.region.min"), vector(region["max"], f"{where}.region.max"))
    if "path" in component:
        path = component["path"]
        check_keys(path, ("tracks", "node"), f"{where}.path")
        camera.path = SimpleNamespace(tracks=identifier(path["tracks"], f"{where}.path.tracks"),
                                      node=identifier(path["node"], f"{where}.path.node"))
    return camera


def load_renderer(component, base, where):
    check_keys(component, ("mesh",), where, optional=("variant",))
    path = (base / text(component["mesh"], f"{where}.mesh")).resolve()
    if not path.is_file():
        raise SettingsError(f"{where}.mesh {component['mesh']!r} is not a file")
    settings = load_import_settings(path)
    if settings.named:
        wanted = text(component.get("variant"), f"{where}.variant") if "variant" in component else None
        if wanted is None:
            raise SettingsError(f"{where}.variant is required: {component['mesh']!r} has variants")
        match = [variant for variant in settings.variants if variant.name == wanted]
        if not match:
            raise SettingsError(f"{where}.variant {wanted!r} is not in {component['mesh']!r}")
        variant = match[0]
    elif "variant" in component:
        raise SettingsError(f"{where}.variant: {component['mesh']!r} has no variants")
    else:
        variant = settings.variants[0]
    return SimpleNamespace(settings=settings, variant=variant)


COMPONENTS = ("mesh_renderer", "light", "camera")


def load_object(value, base, where):
    check_keys(value, ("name",), where, optional=("position", "rotation", "scale", *COMPONENTS))
    present = [name for name in COMPONENTS if name in value]
    if len(present) != 1:
        raise SettingsError(f"{where} must have exactly one component: {', '.join(COMPONENTS)}")
    kind = present[0]
    obj = SimpleNamespace(
        name=text(value["name"], f"{where}.name"), kind=kind,
        position=vector(value["position"], f"{where}.position") if "position" in value else [0.0, 0.0, 0.0],
        rotation=vector(value["rotation"], f"{where}.rotation") if "rotation" in value else [0.0, 0.0, 0.0],
        scale=vector(value["scale"], f"{where}.scale") if "scale" in value else [1.0, 1.0, 1.0])
    if not all(axis > 0 for axis in obj.scale):
        raise SettingsError(f"{where}.scale must be positive on every axis")
    obj.matrix = placement_matrix(obj.rotation, obj.scale)
    obj.identity = obj.position == [0.0] * 3 and obj.rotation == [0.0] * 3 and obj.scale == [1.0] * 3
    spot = f"{where}.{kind}"
    if kind in ("mesh_renderer", "camera"):
        identifier(obj.name, f"{where}.name")  # the scene table names a symbol after it
    if kind == "mesh_renderer":
        obj.component = load_renderer(value[kind], base, spot)
    elif kind == "light":
        obj.component = load_light(value[kind], obj.rotation, spot)
    else:
        obj.component = load_camera(value[kind], spot)
    return obj


def load_scene(path):
    """A scenario: objects (each a transform and one component), the sky and
    ambient settings, and the tone map the lit meshes use."""
    path = pathlib.Path(path).resolve()
    with open(path, "rb") as source:
        values = tomllib.load(source)
    check_keys(values, ("objects",), "scene", optional=("tonemap_white", "sky", "ambient"))
    objects = values["objects"]
    if not isinstance(objects, list) or not objects:
        raise SettingsError("scene.objects must be a non-empty array of tables")
    objects = [load_object(item, path.parent, f"scene.objects[{index}]") for index, item in enumerate(objects)]
    names = [item.name for item in objects]
    if len(set(names)) != len(names):
        raise SettingsError("scene.objects names must be unique")
    renderers = [item for item in objects if item.kind == "mesh_renderer"]
    cameras = [item for item in objects if item.kind == "camera"]
    if not renderers:
        raise SettingsError("scene.objects needs a mesh_renderer")
    if len(cameras) > 1:
        raise SettingsError("scene.objects may have one camera")
    baked = [item.component.variant.name for item in renderers]
    if len(set(baked)) != len(baked):
        raise SettingsError("scene.objects place a mesh name twice")
    lights = [item.component for item in objects if item.kind == "light"]
    for name in ("sky", "ambient"):
        if name in values:
            lights.append(load_environment_light(name, values[name], f"scene.{name}"))
    region = cameras[0].component.region if cameras else None
    scene = SimpleNamespace(
        path=path, objects=objects, renderers=[SimpleNamespace(settings=item.component.settings, variant=item.component.variant,
                                                               object=item) for item in renderers],
        camera=cameras[0] if cameras else None, region=region, lights=lights,
        tonemap_white=number(values["tonemap_white"], "scene.tonemap_white") if "tonemap_white" in values else None)
    lit = any(item.component.settings.light for item in renderers)
    culled = any(item.component.settings.visibility for item in renderers)
    for name, present, needed in (("lights", bool(lights), lit), ("tonemap_white", scene.tonemap_white is not None, lit),
                                  ("camera region", region is not None, culled)):
        if needed and not present:
            raise SettingsError(f"scene {name} is required: a placed mesh has a step that reads it")
        if present and not needed:
            raise SettingsError(f"scene {name} is read by no placed mesh")
    for item in renderers:
        if item.component.settings.scene_dependent and not item.identity:
            raise SettingsError(f"scene.objects {item.name!r}: a mesh with a scene-dependent step is baked where it "
                                "sits, so its transform must be identity")
    return scene
