"""Load reference source geometry, its path light and the source camera samples."""

from types import SimpleNamespace

import numpy as np

from gltf.gltf_read import ASSET_SUFFIXES
from r3d import log, ray_query
from r3d.gltf_mesh import load_gltf_mesh
from r3d.import_settings import BLEND_SUFFIX, SettingsError, lfs_pointer_oid, source_files
from r3d.obj import load_mtl, load_obj, load_textures
from r3d.path_bake import PathLight
from r3d.poses import either_way, sample_camera_path


def load_source(settings, texture_dtype=np.float64):
    if settings.source["path"].suffix.lower() == BLEND_SUFFIX:
        raise SettingsError(f"{settings.source['path'].name}: a .blend is exported by launcher/tools/bake/bake.py "
                            "bake, which hands this import the export")
    for path in source_files(settings):
        if lfs_pointer_oid(path) is not None:
            raise SettingsError(f'{path}: Git LFS source is not pulled; run git lfs pull --exclude=""')
    if settings.source["path"].suffix.lower() in ASSET_SUFFIXES:
        return load_glb_source(settings)
    obj_path = settings.source["path"]
    materials = load_mtl(obj_path.with_suffix(".mtl"))
    p, uv, tri_v, tri_t, tri_m, names, tri_o, object_names = load_obj(obj_path)
    log(f"loaded {len(p)} vertices, {len(tri_v)} triangles, {len(names)} materials")
    textures = load_textures(obj_path.parent, materials, names, texture_dtype)
    return SimpleNamespace(p=p, uv=uv, tri_v=tri_v, tri_t=tri_t, tri_m=tri_m, names=names, materials=materials,
                           textures=textures, colors=None, tri_o=tri_o, object_names=object_names)


def load_glb_source(settings):
    """A glTF binary, or an FBX, as the OBJ path's source: untextured, its colour per vertex."""
    mesh = load_gltf_mesh(settings.source["path"])
    tri_v = np.array(mesh.tri_v, dtype=np.int64)
    log(f"loaded {len(mesh.positions)} vertices, {len(tri_v)} triangles, {len(mesh.names)} materials")
    return SimpleNamespace(p=np.array(mesh.positions, dtype=np.float64), uv=np.zeros((0, 2)), tri_v=tri_v,
                           tri_t=tri_v.copy(), tri_m=np.array(mesh.tri_m, dtype=np.int64), names=mesh.names,
                           materials={}, textures=[None] * len(mesh.names),
                           tri_o=np.asarray(mesh.tri_o, dtype=np.int64), object_names=mesh.object_names,
                           colors=np.array(mesh.colors, dtype=np.float64))


PATH_LIGHTS = {}


def path_light_for(src, job, scene, variant=None):
    """The bounced-light tracer of a renderer's source on `variant` (`ray_query.VARIANT` when None), built once per
    run: renderers of one import with the same lights, settings and variant share it. None when the renderer takes no
    bounced light. Only the latest is kept: each holds the source's textures a second time."""
    if job.bake is None or job.bake.indirect is None:
        return None
    settings = job.settings
    boost = scene.indirect.albedo_boost
    variant = variant or ray_query.VARIANT
    key = (str(settings.path), repr(vars(job.bake.indirect)), repr(scene.lights), boost, variant)
    if key not in PATH_LIGHTS:
        PATH_LIGHTS.clear()
        PATH_LIGHTS[key] = PathLight(src, scene.lights, settings.double_sided, job.bake.indirect, boost, variant)
    return PATH_LIGHTS[key]


def camera_path_poses(scene, visibility, every_ms=None, either_way_up=True):
    """The scene camera's path sampled every `every_ms` (the visibility
    step's own when None) at the step's size, through a square view that
    covers the panel held either way up unless `either_way_up` is False."""
    camera = scene.camera.component
    width, height = visibility.size
    poses = sample_camera_path(camera.path.animation, camera.path.node,
                               every_ms or visibility.every_ms, width, height, camera.half_fov_short_tan, camera.near_z)
    return either_way(*poses) if either_way_up else poses
