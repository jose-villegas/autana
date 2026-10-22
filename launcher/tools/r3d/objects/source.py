"""Triangle ownership beside the source loaders; moves into them with M2's rebake."""

from pathlib import Path
from types import SimpleNamespace

import numpy as np

from gltf.gltf_read import ASSET_SUFFIXES, load_asset, read_accessor
from r3d.gltf_mesh import TRIANGLES
from r3d.mesh_import import load_source
from r3d.obj import load_obj


def obj_ownership(path):
    """OBJ o/g boundaries, in load_obj's fan-triangulated face order."""
    names, owners = [], []
    current = None
    with open(path, encoding='latin-1') as stream:
        for line in stream:
            if line.startswith(('o ', 'g ')):
                names.append(line[2:].strip() or Path(path).stem)
                current = len(names) - 1
            elif line.startswith('f '):
                if current is None:
                    names.append(Path(path).stem)
                    current = len(names) - 1
                owners.extend([current] * max(0, len(line.split()) - 3))
    return np.asarray(owners, dtype=np.int64), names


def gltf_ownership(path):
    """Mesh-node ownership, in load_gltf_mesh's node and primitive order."""
    document, binary = load_asset(path)
    owners, names = [], []
    for index, node in enumerate(document.get('nodes', [])):
        if 'mesh' not in node:
            continue
        owner = len(names)
        names.append(node.get('name') or f'node_{index}')
        for primitive in document['meshes'][node['mesh']]['primitives']:
            if primitive.get('mode', TRIANGLES) != TRIANGLES:
                raise ValueError('object ownership needs triangle primitives')
            accessor = primitive.get('indices', primitive['attributes']['POSITION'])
            count = len(read_accessor(document, binary, accessor)) // 3
            owners.extend([owner] * count)
    return np.asarray(owners, dtype=np.int64), names


def load_object_source(settings):
    """The import's source and aligned ownership, loading textures only for alpha masks."""
    path = settings.source['path']
    if path.suffix.lower() in ASSET_SUFFIXES or settings.alpha_keep is not None:
        src = load_source(settings)
    else:
        fields = ('p', 'uv', 'tri_v', 'tri_t', 'tri_m', 'names')
        src = SimpleNamespace(**dict(zip(fields, load_obj(path))))
    reader = gltf_ownership if path.suffix.lower() in ASSET_SUFFIXES else obj_ownership
    src.tri_o, src.object_names = reader(path)
    if len(src.tri_o) != len(src.tri_v):
        raise ValueError(f'{path}: source triangles and object ownership disagree')
    return src
