"""CPU levels stage: rebake a fit's rest mesh beside its start's copy entries."""
from r3d import mesh_bundle, lod_asset, morph_asset
from r3d.lit_mesh import bundle_entries, finest_triangles, read_bundle_mesh, read_one_bundle_mesh
from r3d.lod.bake_levels import bake_lit_mesh
from r3d.lod.morph import morph_blob


def levels(data, name, lod, meshlet_triangles, start=None, **options):
    """The bundle `data` with its rest `name` rebaked with levels. Every other entry and the copy rows come from
    `start`, the bytes the fit began from, when given: the fit only moves the rest, so a fit that kept a start's
    copies, levelled or not, in this LLOD or an older one, gives the same bundle. Morph targets stay when the
    bundle the entries come from had them."""
    if start is None:
        entries, copies = mesh_bundle.decode(data)
        mesh = read_bundle_mesh(entries, name)
    else:
        mesh = read_one_bundle_mesh(data, name)
        entries, copies = mesh_bundle.decode(start)
        entries = mesh_bundle.rename_rest(entries, copies, name)
    morph = morph_asset.name_for_mesh(name) in {key for key, _, _ in entries}
    q, rgb, tris, double, face = finest_triangles(mesh)
    baked = bake_lit_mesh(q / mesh.position_scale, rgb, tris, double, position_scale=mesh.position_scale,
                          meshlet_triangles=meshlet_triangles, face_rgb=face, lod=lod, **options)
    rest = bundle_entries(name, baked, morph_blob if morph else None)
    replaced = {name, lod_asset.name_for_mesh(name), morph_asset.name_for_mesh(name)}
    return mesh_bundle.encode([row for row in entries if row[0] not in replaced] + rest, copies)
