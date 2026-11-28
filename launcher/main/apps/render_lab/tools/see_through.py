"""Measure source-closed pixels that the drawn bake opens along a camera path."""
import pathlib
import sys

LAUNCHER = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(LAUNCHER / "tools" / "render"))
sys.path.insert(0, str(LAUNCHER / "tools"))

import numpy as np
from scipy import ndimage

from bake import bake
from r3d.appearance_simplify import start_mesh
from r3d.fit.copies import joined, placed_copies
from r3d.import_settings import flight_camera, lfs_pointer_oid, load_scene, source_files
from r3d.light import drop_masked
from r3d.mesh_source import load_source
from r3d.objects.instances import place
from r3d.poses import path_poses
from r3d.ray_query import RayQuery
from r3d.scene_asset import world_placement
from pinhole_rays import FrontMesh, front_hits, pose_rays

# The landscape panel at half resolution, as the host renderer draws a --quarter 1 frame.
RENDER_SIZE = (224, 184)
# The camera path's step, as a baked renderer's camera-path visibility samples it.
EVERY_MS = 500
# A bake hit this much farther than the source's is the view going past the surface, not the surface moved.
BEHIND = 1.5
# The square a see-through region must hold whole somewhere to count.
OPENING = 3
# Per scene, the most see-through pixels its bakes may show over the path: the measured sum plus PEG_MARGIN of it.
MOST_SEE_THROUGH = {"bistro": 165414, "sponza": 2306}
PEG_MARGIN = 0.01


def placed(points, scene, job):
    """`points` in the scene, under the renderer's object's world transform."""
    placement = world_placement(scene, job.object)
    return place(points, placement.matrix, placement.position)


def kept_source(scene, job):
    """The renderer's source as its import keeps it: (points, triangles) placed in the scene."""
    settings = job.settings
    source = load_source(settings)
    triangles, materials = source.tri_v, source.tri_m
    if settings.alpha_keep is not None:
        triangles, _, materials, _ = drop_masked(source.p, source.uv, source.tri_v, source.tri_t, source.tri_m,
                                                 source.textures, settings.alpha_keep)
    if settings.thin:
        triangles = triangles[np.array([source.names[m] != settings.thin.material for m in materials], dtype=bool)]
    return placed(source.p, scene, job), triangles


def drawn_bake(scene, job):
    """The bundle the scene draws for the renderer: (points, triangles, double-sided flags) placed in the scene."""
    bundle = bake.fetch(bake.locked_bake(scene.path, job.object.name, "levels" if job.renderer.fit else "mesh"))
    rest = start_mesh(bundle)
    mesh = joined(rest, placed_copies(bundle, rest.flat))
    return placed(mesh.points[mesh.vertex_point], scene, job), np.asarray(mesh.tris), np.asarray(mesh.double, bool)


def merged(parts):
    """One (points, triangles, ...) of several, each part's triangles offset past the points before it."""
    points, triangles, rest, offset = [], [], [], 0
    for part in parts:
        points.append(part[0])
        triangles.append(part[1] + offset)
        rest.append(part[2:])
        offset += len(part[0])
    return (np.concatenate(points), np.concatenate(triangles), *(np.concatenate(column) for column in zip(*rest)))


class Tracer:
    """The source and the drawn bake of a scene's baked renderers, traced from one pose at a time."""

    def __init__(self, source, drawn):
        points, triangles = source
        self.source = RayQuery(points, triangles)
        self.drawn = FrontMesh(*drawn)

    def see_through(self, width, height, lens, near, pose):
        """The pose's see-through regions as a height x width mask."""
        origins, directions = pose_rays(width, height, lens, near, pose)
        hit, distance, _ = self.source.first_hits(origins, directions)
        _, depth = front_hits(self.drawn, pose[:3], origins, directions)
        past = hit & (depth > BEHIND * np.where(hit, distance, 0.0))
        return ndimage.binary_opening(past.reshape(height, width), np.ones((OPENING, OPENING), bool))


def baked_scenes():
    """(key, scene) for every demo scene with a camera path and a baked renderer."""
    for path in sorted((LAUNCHER / "demo").glob("*/*.scene.toml")):
        scene = load_scene(path)
        if flight_camera(scene) is not None and any(job.bake for job in scene.renderers):
            yield path.name.removesuffix(".scene.toml"), scene


def measure_see_through(scene):
    """See-through pixel counts in camera path order."""
    baked = [job for job in scene.renderers if job.bake]
    tracer = Tracer(merged([kept_source(scene, job) for job in baked]),
                    merged([drawn_bake(scene, job) for job in baked]))
    width, height, lens, near, poses = path_poses(flight_camera(scene), RENDER_SIZE, EVERY_MS, either_way_up=False)
    return [int(tracer.see_through(width, height, lens, near, pose).sum()) for pose in poses]


def see_through_ceiling(key):
    """The total pixel ceiling of a scene, including its margin."""
    return round(MOST_SEE_THROUGH[key] * (1 + PEG_MARGIN))
