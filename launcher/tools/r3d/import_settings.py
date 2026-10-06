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

from anim import tracks_asset
from asset.asset_pack import NAME_BYTES

RESERVED_LIGHTS = ("point", "spot")

# The one declaration of each light type's fields; light.py pairs each with
# the function that bakes it.
LIGHT_FIELDS = {
    "directional": {"direction": "vector", "color": "vector", "intensity": "number"},
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
    """Letters, digits and _, not starting with a digit, that fit a pack name
    field: an object's name names its baked mesh and its entity in the scene
    entry, a node's names its tracks."""
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value) or len(value) >= NAME_BYTES:
        raise SettingsError(f"{where} must be letters, digits and _, not starting with a digit, at most {NAME_BYTES - 1} bytes")
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


FIT_GROUPS = {
    "prune": ("budget", "coverage_every_ms"),
    "poses": ("train_every_ms", "held_out_every_ms"),
    "optimise": ("steps", "batch", "laplacian", "normal_weight"),
    "hashes": ("sha256", "recipe_sha256"),
}
FIT_KEYS = tuple(key for keys in FIT_GROUPS.values() for key in keys)


def load_fit(value, variant, where):
    """The grouped recipe of a variant the appearance fit makes offline from
    the mesh the import bakes at `triangles`: `prune` names the budget and
    coverage poses, `poses` names training and held-out camera-path poses,
    `optimise` holds settings, and `hashes` records the mesh and recipe
    SHA-256s (fitted_variant.recipe_digest)."""
    for group, keys in FIT_GROUPS.items():
        for key in keys:
            if key in value:
                raise SettingsError(f"{where}.{key} moved to {where}.{group}.{key}")
    check_keys(value, FIT_GROUPS, where)
    values = {}
    for group, keys in FIT_GROUPS.items():
        table = value[group]
        check_keys(table, keys, f"{where}.{group}")
        values.update(table)
    if variant.triangles is None:
        raise SettingsError(f"{where} needs the variant's triangles, the budget its start is simplified to")
    fit = SimpleNamespace(**{key: values[key] for key in FIT_KEYS})
    for key in ("budget", "train_every_ms", "held_out_every_ms", "coverage_every_ms", "steps", "batch"):
        setattr(fit, key, count(values[key], f"{where}.{key}"))
    fit.laplacian = number(values["laplacian"], f"{where}.laplacian")
    fit.normal_weight = number(values["normal_weight"], f"{where}.normal_weight")
    fit.sha256 = text(values["sha256"], f"{where}.sha256")
    fit.recipe_sha256 = text(values["recipe_sha256"], f"{where}.recipe_sha256")
    if fit.budget > variant.triangles:
        raise SettingsError(f"{where}.budget cannot exceed the variant's triangles")
    return fit


def load_variant(value, steps, where):
    homes = {"shading": "objects.mesh_renderer.shading", "visibility": "objects.mesh_renderer.visibility",
             "fit": "objects.mesh_renderer.fit", "indirect": "objects.mesh_renderer.indirect"}
    if isinstance(value, dict):
        for name, home in homes.items():
            if name in value:
                raise SettingsError(f"{where}.{name} moved to {home}")
    check_keys(value, ("name",), where, optional=("triangles",))
    variant = SimpleNamespace(name=text(value["name"], f"{where}.name"), triangles=None)
    if steps.simplify:
        if "triangles" not in value:
            raise SettingsError(f"{where}.triangles is required when geometry.simplify is present")
        variant.triangles = count(value["triangles"], f"{where}.triangles")
    elif "triangles" in value:
        raise SettingsError(f"{where}.triangles needs geometry.simplify")
    return variant


VISIBILITY_SOURCES = ("camera_region", "camera_path")


def load_visibility(table, where="visibility"):
    """Where the camera can be, so what it can see. `camera_region` keeps
    what any point of the scene camera's region box sees (`rounds` random
    tries per triangle); `camera_path` keeps what any pose of the scene
    camera's path sees at `size` pixels, sampled every `every_ms`, with
    `samples` squared rays per pixel and the view widened by `margin`
    pixels on each side. Each mesh renderer carries its own."""
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
    "visibility": "objects.mesh_renderer.visibility",
    "thin": "[geometry] thin",
    "light": "scene [bake]",
    "simplify": "[geometry] simplify",
}


def load_process(process, steps):
    """The seed shared by every random import step."""
    if isinstance(process, dict):
        for name, home in LEGACY_PROCESS_HOMES.items():
            if name in process:
                raise SettingsError(f"[process.{name}] moved to {home}")
    check_keys(process, (), "process", optional=("seed",))
    steps.seed_given = "seed" in process
    steps.seed = integer(process["seed"], "process.seed") if steps.seed_given else 0


def load_geometry(table, steps):
    """The optional steps that decide which source triangles remain."""
    check_keys(table, (), "geometry", optional=("alpha_mask", "thin", "simplify"))
    if "alpha_mask" in table:
        alpha_mask = table["alpha_mask"]
        check_keys(alpha_mask, ("keep_alpha",), "geometry.alpha_mask")
        steps.alpha_keep = number(alpha_mask["keep_alpha"], "geometry.alpha_mask.keep_alpha")
    if "thin" in table:
        thin = table["thin"]
        check_keys(thin, ("material", "keep"), "geometry.thin")
        steps.thin = SimpleNamespace(material=text(thin["material"], "geometry.thin.material"),
                                     keep=number(thin["keep"], "geometry.thin.keep"))
    if "simplify" in table:
        simplify = table["simplify"]
        check_keys(simplify, ("dense_edge", "props", "props_share", "seal_seams"), "geometry.simplify")
        steps.simplify = SimpleNamespace(
            dense_edge=number(simplify["dense_edge"], "geometry.simplify.dense_edge"),
            props=set(strings(simplify["props"], "geometry.simplify.props")),
            props_share=number(simplify["props_share"], "geometry.simplify.props_share"),
            seal_seams=boolean(simplify["seal_seams"], "geometry.simplify.seal_seams"))


def load_import_settings(path):
    """One mesh asset: its source, output and opt-in processing. With only
    those, the mesh is imported as authored."""
    path = pathlib.Path(path).resolve()
    with open(path, "rb") as source:
        values = tomllib.load(source)
    for name, home in (("visibility", "objects.mesh_renderer.visibility"), ("lighting", "scene [bake]")):
        if name in values:
            raise SettingsError(f"[{name}] moved to {home}")
    check_keys(values, ("source", "output"), "settings", optional=("materials", "process", "geometry", "variants"))
    source = values["source"]
    check_keys(source, ("path", "credit"), "source")
    for name in source:
        text(source[name], f"source.{name}")
    source["path"] = (path.parent / source["path"]).resolve()
    if source["path"].suffix.lower() != ".obj":
        raise SettingsError("source.path has an unsupported extension; supported: .obj")
    output = values["output"]
    check_keys(output, ("directory",), "output", optional=("name", "position_scale"))
    directory = text(output["directory"], "output.directory")
    materials = values.get("materials", {})
    check_keys(materials, (), "materials", optional=("double_sided",))
    steps = SimpleNamespace(seed=0, seed_given=False, alpha_keep=None, thin=None, simplify=None)
    load_process(values.get("process", {}), steps)
    load_geometry(values.get("geometry", {}), steps)
    if steps.seed_given and not steps.thin:
        raise SettingsError("process.seed needs geometry.thin")
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
        budgets = [variant.triangles for variant in variants]
        if len(set(budgets)) != len(budgets):
            raise SettingsError("variants with the same triangle budget would produce the same mesh")
    else:
        if steps.simplify:
            raise SettingsError("geometry.simplify needs variants, each with its triangles budget")
        variants = [SimpleNamespace(name=text(output.get("name"), "output.name"), triangles=None)]
    return SimpleNamespace(
        path=path, source=source, out_dir=(path.parent / directory).resolve(), mesh_dir=path.parent,
        position_scale=count(output["position_scale"], "output.position_scale") if "position_scale" in output else None,
        double_sided=set(strings(materials.get("double_sided", []), "materials.double_sided")), seed=steps.seed,
        alpha_keep=steps.alpha_keep, thin=steps.thin, simplify=steps.simplify, named=("variants" in values),
        variants=variants)


def source_files(settings):
    """The OBJ, its sibling MTL and every texture the loader reads."""
    from r3d.obj import TEXTURE_KEYS, load_mtl

    path = settings.source["path"]
    material = path.with_suffix(".mtl")
    files = {path, material}
    for entry in load_mtl(material).values():
        for name in TEXTURE_KEYS:
            if name in entry:
                files.add((path.parent / entry[name]).resolve())
    return sorted(files)


def lfs_pointer_oid(path) -> bytes | None:
    with path.open("rb") as source:
        lines = source.read(1024).splitlines()
    if not lines or lines[0] != b"version https://git-lfs.github.com/spec/v1":
        return None
    oids = [line.removeprefix(b"oid sha256:") for line in lines if line.startswith(b"oid sha256:")]
    if len(oids) != 1 or re.fullmatch(rb"[0-9a-f]{64}", oids[0]) is None:
        raise SettingsError(f"{path}: malformed Git LFS pointer; run git lfs pull --exclude=\"\"")
    return oids[0]


def content_checksum(path):
    """An LFS pointer and its hydrated contents identify the same source."""
    import hashlib

    oid = lfs_pointer_oid(path)
    return oid if oid is not None else hashlib.sha256(path.read_bytes()).hexdigest().encode()


def source_digest(settings):
    """Hash source contents and relative names, including hydrated LFS objects."""
    import hashlib
    import os

    digest = hashlib.sha256()
    for path in source_files(settings):
        digest.update(pathlib.Path(os.path.relpath(path, settings.source["path"].parent)).as_posix().encode())
        digest.update(b"\0" + content_checksum(path))
    return digest.hexdigest()


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


def load_camera(component, base, where):
    check_keys(component, ("half_fov_short_tan", "near_z"), where, optional=("region", "path", "background"))
    camera = SimpleNamespace(half_fov_short_tan=number(component["half_fov_short_tan"], f"{where}.half_fov_short_tan"),
                             near_z=number(component["near_z"], f"{where}.near_z"), region=None, path=None,
                             background=colour_rgb(component.get("background", 0), f"{where}.background"))
    if "region" in component:
        region = component["region"]
        check_keys(region, ("min", "max"), f"{where}.region")
        camera.region = (vector(region["min"], f"{where}.region.min"), vector(region["max"], f"{where}.region.max"))
    if "path" in component:
        camera.path = load_camera_path(component["path"], base, f"{where}.path")
    return camera


def load_camera_path(path, base, where):
    """The clip a camera flies, named as a .anim.toml relative to the scene
    file (its id is the stem), and the node in it that is the camera."""
    check_keys(path, ("animation", "node"), where)
    animation = (base / text(path["animation"], f"{where}.animation")).resolve()
    if not animation.name.endswith(tracks_asset.SUFFIX) or not animation.is_file():
        raise SettingsError(f"{where}.animation {path['animation']!r} is not an {tracks_asset.SUFFIX} file")
    node = identifier(path["node"], f"{where}.node")
    if len(f"{node}/translation") >= tracks_asset.NAME_BYTES:
        raise SettingsError(f"{where}.node {node!r}: its track names exceed {tracks_asset.NAME_BYTES - 1} bytes")
    clip = tracks_asset.clip_id(animation)
    if len(clip.encode("utf-8")) >= NAME_BYTES:
        raise SettingsError(f"{where}.animation: clip id {clip!r} exceeds the pack's {NAME_BYTES - 1}-byte limit")
    return SimpleNamespace(animation=animation, clip=clip, node=node)


def load_renderer(component, base, where):
    check_keys(component, ("mesh",), where, optional=("variant", "bake", "shading", "visibility", "fit", "indirect"))
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
    bake = boolean(component["bake"], f"{where}.bake") if "bake" in component else False
    face_samples = None
    if "shading" in component:
        shading = component["shading"]
        if shading == "smooth":
            pass
        elif isinstance(shading, dict):
            check_keys(shading, ("flat",), f"{where}.shading")
            face_samples = face_sample_options(shading["flat"], f"{where}.shading.flat")
        else:
            raise SettingsError(f"{where}.shading must be smooth or {{ flat = ... }}")
    visibility = load_visibility(component["visibility"], f"{where}.visibility") if "visibility" in component else None
    fit = load_fit(component["fit"], variant, f"{where}.fit") if "fit" in component else None
    indirect = True
    if "indirect" in component:
        if boolean(component["indirect"], f"{where}.indirect"):
            raise SettingsError(f"{where}.indirect can only be false")
        indirect = False
    if not bake and (face_samples or visibility or fit or not indirect):
        raise SettingsError(f"{where} bake = true is required for shading, visibility, fit or indirect")
    return SimpleNamespace(settings=settings, variant=variant, bake=bake, face_samples=face_samples, visibility=visibility,
                           fit=fit, indirect=indirect)


def make_job(settings, renderer, obj, asset_name, asset_path, bake):
    """One mesh to write: its import, the renderer that decides its look, the scene object it belongs to
    (None for a bare import), where it is written and the scene bake settings it is traced with (None for albedo)."""
    return SimpleNamespace(settings=settings, renderer=renderer, object=obj, asset_name=asset_name, asset_path=asset_path,
                           bake=bake)


def albedo_jobs(settings):
    """The jobs of a bare import: each variant's authored albedo, written beside the import file."""
    jobs = []
    for variant in settings.variants:
        renderer = SimpleNamespace(settings=settings, variant=variant, bake=False, face_samples=None, visibility=None,
                                   fit=None, indirect=True)
        jobs.append(make_job(settings, renderer, None, variant.name, settings.mesh_dir / f"{variant.name}.mesh", None))
    return jobs


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
        identifier(obj.name, f"{where}.name")
    if kind == "mesh_renderer":
        obj.component = load_renderer(value[kind], base, spot)
    elif kind == "light":
        obj.component = load_light(value[kind], obj.rotation, spot)
    else:
        obj.component = load_camera(value[kind], base, spot)
    return obj


def load_indirect_look(table):
    """The scene's `[indirect]` look controls; each defaults to the physical 1.0."""
    check_keys(table, (), "scene.indirect", optional=("intensity", "albedo_boost"))
    look = SimpleNamespace(intensity=number(table.get("intensity", 1.0), "scene.indirect.intensity"),
                           albedo_boost=number(table.get("albedo_boost", 1.0), "scene.indirect.albedo_boost"))
    if look.intensity < 0:
        raise SettingsError("scene.indirect.intensity must not be negative")
    if look.albedo_boost <= 0:
        raise SettingsError("scene.indirect.albedo_boost must be positive")
    return look


def load_bake(table):
    """The scene's bake settings: ray offset, colour merging, bounced light and local occlusion."""
    check_keys(table, ("ray_offset", "colour_merge_step"), "scene.bake", optional=("indirect", "ao"))
    bake = SimpleNamespace(ray_offset=number(table["ray_offset"], "scene.bake.ray_offset"),
                           colour_merge_step=count(table["colour_merge_step"], "scene.bake.colour_merge_step"),
                           indirect=None, ao=None)
    if "indirect" in table:
        indirect = table["indirect"]
        check_keys(indirect, ("bounces", "rays"), "scene.bake.indirect")
        bake.indirect = SimpleNamespace(bounces=count(indirect["bounces"], "scene.bake.indirect.bounces"),
                                        rays=count(indirect["rays"], "scene.bake.indirect.rays"))
    if "ao" in table:
        ao = table["ao"]
        check_keys(ao, ("distance", "rays"), "scene.bake.ao", optional=("strength", "indirect"))
        bake.ao = SimpleNamespace(distance=number(ao["distance"], "scene.bake.ao.distance"),
                                  rays=count(ao["rays"], "scene.bake.ao.rays"),
                                  strength=number(ao.get("strength", 1.0), "scene.bake.ao.strength"),
                                  indirect=boolean(ao.get("indirect", False), "scene.bake.ao.indirect"))
        if bake.ao.distance <= 0:
            raise SettingsError("scene.bake.ao.distance must be positive")
        if not 0.0 <= bake.ao.strength <= 1.0:
            raise SettingsError("scene.bake.ao.strength must be between 0 and 1")
    return bake


def load_scene(path):
    """A scenario: objects (each a transform and one component), the sky and
    ambient settings, and the tone map the lit meshes use."""
    path = pathlib.Path(path).resolve()
    with open(path, "rb") as source:
        values = tomllib.load(source)
    check_keys(values, ("objects",), "scene", optional=("tonemap_white", "sky", "ambient", "indirect", "bake"))
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
    lights = [item.component for item in objects if item.kind == "light"]
    for name in ("sky", "ambient"):
        if name in values:
            lights.append(load_environment_light(name, values[name], f"scene.{name}"))
    region = cameras[0].component.region if cameras else None
    bake = load_bake(values["bake"]) if "bake" in values else None
    if any(item.component.bake for item in renderers) and bake is None:
        raise SettingsError("scene [bake] is required: a renderer has bake = true")
    jobs = []
    scene_name = path.name.removesuffix(".scene.toml").removesuffix(".toml")
    for item in renderers:
        component = item.component
        effective = None
        if component.bake:
            if not component.indirect and bake.indirect is None:
                raise SettingsError("objects.mesh_renderer.indirect = false needs scene.bake.indirect")
            effective = SimpleNamespace(
                ray_offset=bake.ray_offset, colour_merge_step=bake.colour_merge_step,
                indirect=bake.indirect if component.indirect else None, ao=bake.ao)
            asset_name = f"{scene_name}.{item.name}"
            asset_path = path.parent / f"{asset_name}.mesh"
        else:
            asset_name = component.variant.name
            asset_path = component.settings.mesh_dir / f"{asset_name}.mesh"
        try:
            too_long = len(asset_name.encode("ascii")) >= NAME_BYTES
        except UnicodeEncodeError:
            too_long = True
        if too_long:
            raise SettingsError(f"scene.objects {item.name!r}: asset name {asset_name!r} exceeds the pack's {NAME_BYTES - 1}-byte limit")
        jobs.append(make_job(component.settings, component, item, asset_name, asset_path, effective))
    scene = SimpleNamespace(
        path=path, objects=objects, renderers=jobs, bake=bake,
        camera=cameras[0] if cameras else None, region=region, lights=lights,
        tonemap_white=number(values["tonemap_white"], "scene.tonemap_white") if "tonemap_white" in values else None,
        indirect=load_indirect_look(values.get("indirect", {})))
    lit = any(item.renderer.bake for item in jobs)
    sources = {item.renderer.visibility.source for item in jobs if item.renderer.visibility}
    camera_path = cameras[0].component.path if cameras else None
    bounced = bool(bake and bake.indirect and any(item.renderer.indirect for item in jobs if item.renderer.bake))
    if bake and bake.ao:
        scaled = any(light["type"] == "ambient" for light in lights) or (bake.ao.indirect and bounced)
        if not scaled:
            raise SettingsError("scene.bake.ao scales the ambient light or, with indirect = true, the bounce light "
                                "a baked renderer takes bounced light: the scene has neither")
        if bake.ao.indirect and not bounced:
            raise SettingsError("scene.bake.ao.indirect is read by no placed mesh: no baked renderer takes bounced light")
    if "indirect" in values and not bounced:
        raise SettingsError("scene indirect settings is read by no placed mesh")
    for name, present, needed in (("lights", bool(lights), lit), ("tonemap_white", scene.tonemap_white is not None, lit),
                                  ("camera region", region is not None, "camera_region" in sources)):
        if needed and not present:
            raise SettingsError(f"scene {name} is required: a placed mesh has a step that reads it")
        if present and not needed:
            raise SettingsError(f"scene {name} is read by no placed mesh")
    if "camera_path" in sources and camera_path is None:
        raise SettingsError("scene camera path is required: a placed mesh keeps what the camera path sees")
    for item in jobs:
        if item.renderer.bake and not item.object.identity:
            raise SettingsError(f"scene.objects {item.object.name!r}: a baked renderer is traced where it "
                                "sits, so its transform must be identity")
    return scene
