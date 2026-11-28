"""mesh_import.bake_geometry with its lighting stubbed out, for tests that check which triangles it traces."""

from types import SimpleNamespace
from unittest import mock

import numpy as np

from r3d import mesh_import


def unlit_bake_geometry(job, scene, query, path_light_for=lambda *rest: None, **options):
    """Runs bake_geometry on `job` with every light off, `scene`'s fields beside the lights' given as a dict
    (lights, indirect and tonemap_white filled in), and each RayQuery made by `query(points, tris, *rest,
    **options)`. Returns the geometry, the triangle count of each query built, and the intersector each shaded
    material was lit against."""
    built, shaded = [], []

    def ray_query(points, tris, *rest, **query_options):
        built.append(len(tris))
        return query(points, tris, *rest, **query_options)

    def shade_lit(src, job, scene, material, mp, mt, double, intersector, bounce, *, labels, **shade_options):
        shaded.append(intersector)
        return (*mesh_import.shade_unlit(src, material, mp, mt, double), labels)

    job = SimpleNamespace(settings=job.settings, renderer=job.renderer,
                          bake=SimpleNamespace(ray_offset=0.0, ao=None, indirect=None))
    scene = SimpleNamespace(lights=[], indirect=SimpleNamespace(intensity=1.0), tonemap_white=1.0, **scene)
    with mock.patch.object(mesh_import, "RayQuery", side_effect=ray_query), \
            mock.patch.object(mesh_import, "shade_lit", side_effect=shade_lit), \
            mock.patch.object(mesh_import, "light", side_effect=lambda p, *rest, **kw: np.ones((len(p), 3))), \
            mock.patch.object(mesh_import, "path_light_for", side_effect=path_light_for):
        geometry = mesh_import.bake_geometry(job, scene, **options)
    return geometry, built, shaded
