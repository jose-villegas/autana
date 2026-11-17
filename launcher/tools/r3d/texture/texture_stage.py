"""The texture bake (bake.py's "texture" kind): a textured renderer's LTEX and LUVS entries as one asset pack, made
from its packed mesh bundle and the import's source."""
import numpy as np

from asset.asset_pack import build_pack
from r3d import mesh_bundle
from r3d.lit_mesh import read_bundle_mesh
from r3d.mesh_source import load_source
from r3d.texture.uv_transfer import textured_entries


def texture(bundle, job, textures):
    """The pack of `job`'s entries drawing `textures` (texture_settings) over its packed mesh `bundle` (bytes)."""
    entries, copies = mesh_bundle.decode(bundle)
    mesh = read_bundle_mesh(entries, mesh_bundle.rest_name(entries, copies))
    source = load_source(job.settings, np.float32)
    return build_pack(textured_entries(mesh, job.asset_name, source, textures.materials, textures.size))
