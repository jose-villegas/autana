"""CPU levels stage: rebake a fit's rest mesh and retain its copy entries."""
from r3d import mesh_bundle, lod_asset
from r3d.lit_mesh import finest_triangles
from r3d.lod.bake_levels import bake_lit_mesh


def levels(data, name, lod, meshlet_triangles):
    entries, copies = mesh_bundle.decode(data)
    mesh = mesh_bundle.read_mesh(entries, name)
    q, rgb, tris, double, face = finest_triangles(mesh)
    baked = bake_lit_mesh(q / mesh.position_scale, rgb, tris, double, position_scale=mesh.position_scale,
                          meshlet_triangles=meshlet_triangles, face_rgb=face, lod=lod)
    rest = mesh_bundle.mesh_entries(name, baked)
    replaced = {name, lod_asset.name_for_mesh(name)}
    return mesh_bundle.encode([row for row in entries if row[0] not in replaced] + rest, copies)
