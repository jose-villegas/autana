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


FIT_KEYS = ("budget", "train_every_ms", "held_out_every_ms", "coverage_every_ms", "steps", "batch", "laplacian",
            "normal_weight", "sha256", "recipe_sha256")


def load_fit(value, variant, where):
    """The recipe of a variant the appearance fit makes offline from the mesh
    the import bakes at `triangles`: the budget it prunes to, the camera-path
    poses it trains on (every `train_every_ms`, less the multiples of
    `held_out_every_ms`), the denser poses its pruning counts over, its
    optimiser settings, the SHA-256 of the mesh it made, and the SHA-256 of
    the recipe it was made from (fitted_variant.recipe_digest)."""
    check_keys(value, FIT_KEYS, where)
    if variant.triangles is None:
        raise SettingsError(f"{where} needs the variant's triangles, the budget its start is simplified to")
    fit = SimpleNamespace(**{key: value[key] for key in FIT_KEYS})
    for key in ("budget", "train_every_ms", "held_out_every_ms", "coverage_every_ms", "steps", "batch"):
        setattr(fit, key, count(value[key], f"{where}.{key}"))
    fit.laplacian = number(value["laplacian"], f"{where}.laplacian")
    fit.normal_weight = number(value["normal_weight"], f"{where}.normal_weight")
    fit.sha256 = text(value["sha256"], f"{where}.sha256")
    fit.recipe_sha256 = text(value["recipe_sha256"], f"{where}.recipe_sha256")
    if fit.budget > variant.triangles:
        raise SettingsError(f"{where}.budget cannot exceed the variant's triangles")
    return fit


def load_variant(value, steps, where):
    if isinstance(value, dict) and "face_samples" in value:
        raise SettingsError("face_samples moved to shading = { flat = ... }")
    check_keys(value, ("name",), where, optional=("triangles", "shading", "visibility", "fit"))
    variant = SimpleNamespace(name=text(value["name"], f"{where}.name"), triangles=None, shading="smooth", face_samples=None,
                              visibility=None, fit=None)
    if steps.simplify:
        if "triangles" not in value:
            raise SettingsError(f"{where}.triangles is required when geometry.simplify is present")
        variant.triangles = count(value["triangles"], f"{where}.triangles")
    elif "triangles" in value:
        raise SettingsError(f"{where}.triangles needs geometry.simplify")
    if "shading" in value:
        shading = value["shading"]
        if shading == "smooth":
            pass
        elif isinstance(shading, dict):
            check_keys(shading, ("flat",), f"{where}.shading")
            if not steps.light:
                raise SettingsError(f"{where}.shading needs lighting.light")
            variant.shading = "flat"
            variant.face_samples = face_sample_options(shading["flat"], f"{where}.shading.flat")
        else:
            raise SettingsError(f"{where}.shading must be smooth or {{ flat = ... }}")
    if "visibility" in value:
        variant.visibility = load_visibility(value["visibility"], f"{where}.visibility")
    if "fit" in value:
        if variant.face_samples or not steps.light:
            raise SettingsError(f"{where}.fit needs a smooth variant of a lit import")
        variant.fit = load_fit(value["fit"], variant, f"{where}.fit")
    return variant


VISIBILITY_SOURCES = ("camera_region", "camera_path")


def load_visibility(table, where="visibility"):
    """Where the camera can be, so what it can see. `camera_region` keeps
    what any point of the scene camera's region box sees (`rounds` random
    tries per triangle); `camera_path` keeps what any pose of the scene
    camera's path sees at `size` pixels, sampled every `every_ms`, with
    `samples` squared rays per pixel and the view widened by `margin`
    pixels on each side. A variant's own table overrides the import's."""
    source = text(table.get("source", "camera_region"), f"{where}.source")
    if source not in VISIBILITY_SOURCES:
        raise SettingsError(f"{where}.source must be one of {', '.join(VISIBILITY_SOURCES)}")
    if source == "camera_region":
        check_keys(table, ("rounds",), where, optional=("source",))
        return SimpleNamespace(source=source, rounds=count(table["rounds"], f"{where}.rounds"))
    check_keys(table, ("every_ms", "size"), where, optional=("source", "samples", "margin"))
    size = table["size"]
    if not isinstance(size, list) or len(size) != 2:
        raise SettingsError(f"{where}.size must be [width, height]")
    margin = integer(table.get("margin", 0), f"{where}.margin")
    if margin < 0:
        raise SettingsError(f"{where}.margin cannot be negative")
    return SimpleNamespace(source=source, every_ms=count(table["every_ms"], f"{where}.every_ms"),
                           size=(count(size[0], f"{where}.size"), count(size[1], f"{where}.size")),
                           samples=count(table.get("samples", 3), f"{where}.samples"), margin=margin)


LEGACY_PROCESS_HOMES = {
    "alpha_mask": "[geometry] alpha_mask",
    "visibility": "[visibility]",
    "thin": "[geometry] thin",
    "light": "[lighting] light",
    "simplify": "[geometry] simplify",
}


def load_process(process):
    """The seed shared by every random import step."""
    if isinstance(process, dict):
        for name, home in LEGACY_PROCESS_HOMES.items():
            if name in process:
                raise SettingsError(f"[process.{name}] moved to {home}")
    check_keys(process, (), "process", optional=("seed",))
    return SimpleNamespace(seed=integer(process["seed"], "process.seed") if "seed" in process else 0)


def load_geometry(table):
    """The optional steps that decide which source triangles remain."""
    check_keys(table, (), "geometry", optional=("alpha_mask", "thin", "simplify"))
    geometry = SimpleNamespace(alpha_keep=None, thin=None, simplify=None)
    if "alpha_mask" in table:
        alpha_mask = table["alpha_mask"]
        check_keys(alpha_mask, ("keep_alpha",), "geometry.alpha_mask")
        geometry.alpha_keep = number(alpha_mask["keep_alpha"], "geometry.alpha_mask.keep_alpha")
    if "thin" in table:
        thin = table["thin"]
        check_keys(thin, ("material", "keep"), "geometry.thin")
        geometry.thin = SimpleNamespace(material=text(thin["material"], "geometry.thin.material"),
                                        keep=number(thin["keep"], "geometry.thin.keep"))
    if "simplify" in table:
        simplify = table["simplify"]
        check_keys(simplify, ("dense_edge", "props", "props_share", "seal_seams"), "geometry.simplify")
        geometry.simplify = SimpleNamespace(
            dense_edge=number(simplify["dense_edge"], "geometry.simplify.dense_edge"),
            props=set(strings(simplify["props"], "geometry.simplify.props")),
            props_share=number(simplify["props_share"], "geometry.simplify.props_share"),
            seal_seams=boolean(simplify["seal_seams"], "geometry.simplify.seal_seams"))
    return geometry


def load_lighting(table):
    """The optional step that bakes the scene's light into the mesh."""
    check_keys(table, (), "lighting", optional=("light",))
    if "light" not in table:
        return None
    light = table["light"]
    check_keys(light, ("ray_offset", "colour_merge_step"), "lighting.light", optional=("flat_sky_rays",))
    return SimpleNamespace(
        ray_offset=number(light["ray_offset"], "lighting.light.ray_offset"),
        colour_merge_step=count(light["colour_merge_step"], "lighting.light.colour_merge_step"),
        flat_sky_rays=count(light["flat_sky_rays"], "lighting.light.flat_sky_rays") if "flat_sky_rays" in light else None)


def load_import_settings(path):
    """One mesh asset: its source, output and opt-in processing. With only
    those, the mesh is imported as authored."""
    path = pathlib.Path(path).resolve()
    with open(path, "rb") as source:
        values = tomllib.load(source)
    check_keys(values, ("source", "output"), "settings", optional=("materials", "process", "geometry", "visibility", "lighting", "variants"))
    source = values["source"]
    check_keys(source, ("url", "sha256", "path", "cache", "credit"), "source")
    for name in source:
        text(source[name], f"source.{name}")
    output = values["output"]
    check_keys(output, ("directory",), "output", optional=("name", "position_scale"))
    directory = text(output["directory"], "output.directory")
    materials = values.get("materials", {})
    check_keys(materials, (), "materials", optional=("double_sided",))
    process = load_process(values.get("process", {}))
    geometry = load_geometry(values.get("geometry", {}))
    visibility = load_visibility(values["visibility"]) if "visibility" in values else None
    light = load_lighting(values.get("lighting", {}))
    steps = SimpleNamespace(seed=process.seed, alpha_keep=geometry.alpha_keep, visibility=visibility, thin=geometry.thin,
                            light=light, simplify=geometry.simplify)
    if "seed" in values.get("process", {}) and not (steps.visibility or steps.thin or steps.light):
        raise SettingsError("process.seed needs a step that draws random rays: visibility, thin or light")
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
            shape = (variant.triangles, repr(variant.face_samples), repr(variant.visibility), repr(variant.fit))
            if shape in shapes:
                raise SettingsError(f"variants {shapes[shape]!r} and {variant.name!r} would produce the same mesh")
            shapes[shape] = variant.name
    else:
        if steps.simplify:
            raise SettingsError("geometry.simplify needs variants, each with its triangles budget")
        variants = [SimpleNamespace(name=text(output.get("name"), "output.name"), triangles=None, face_samples=None,
                                    shading="smooth", visibility=None, fit=None)]
    flat = any(variant.face_samples for variant in variants)
    if flat and not steps.light:
        raise SettingsError("shading = { flat = ... } needs lighting.light")
    if flat and steps.light.flat_sky_rays is None:
        raise SettingsError("lighting.light.flat_sky_rays is required when a variant has shading = { flat = ... }")
    if steps.light and not flat and steps.light.flat_sky_rays is not None:
        raise SettingsError("lighting.light.flat_sky_rays applies to a variant with shading = { flat = ... } only")
    return SimpleNamespace(
        path=path, source=source, out_dir=(path.parent / directory).resolve(), mesh_dir=path.parent,
        position_scale=count(output["position_scale"], "output.position_scale") if "position_scale" in output else None,
        double_sided=set(strings(materials.get("double_sided", []), "materials.double_sided")), seed=steps.seed,
        alpha_keep=steps.alpha_keep, visibility=steps.visibility, thin=steps.thin, light=steps.light,
        simplify=steps.simplify, scene_dependent=bool(steps.light or steps.visibility or any(variant.visibility for variant in variants)), named=("variants" in values),
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
    sources = {visibility.source for visibility in (item.component.variant.visibility or item.component.settings.visibility
                                                    for item in renderers) if visibility}
    camera_path = cameras[0].component.path if cameras else None
    for name, present, needed in (("lights", bool(lights), lit), ("tonemap_white", scene.tonemap_white is not None, lit),
                                  ("camera region", region is not None, "camera_region" in sources)):
        if needed and not present:
            raise SettingsError(f"scene {name} is required: a placed mesh has a step that reads it")
        if present and not needed:
            raise SettingsError(f"scene {name} is read by no placed mesh")
    if "camera_path" in sources and camera_path is None:
        raise SettingsError("scene camera path is required: a placed mesh keeps what the camera path sees")
    for item in renderers:
        if item.component.settings.scene_dependent and not item.identity:
            raise SettingsError(f"scene.objects {item.name!r}: a mesh with a scene-dependent step is baked where it "
                                "sits, so its transform must be identity")
    return scene
