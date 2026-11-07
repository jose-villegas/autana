"""Sample a camera animation for billboard-cloud visibility weights."""

from anim import tracks_asset
from gltf import gltf_read
from r3d.import_settings import SettingsError
from r3d.poses import sample_camera_path


def camera_path(path, *, size, every_ms):
    source, _ = tracks_asset.load_source(path)
    document, _ = gltf_read.load_asset(source)
    holders = [(i, node['camera']) for i, node in enumerate(document.get('nodes', [])) if 'camera' in node]
    if len(holders) != 1:
        raise SettingsError('geometry.cards.path needs exactly one camera node')
    node, index = holders[0]
    camera = document['cameras'][index]
    if camera['type'] != 'perspective':
        raise SettingsError('geometry.cards.path needs a perspective camera')
    perspective = camera['perspective']
    lens = gltf_read.camera_half_fov_short_tan(perspective['yfov'], perspective.get('aspectRatio', 1.0))
    return sample_camera_path(path, tracks_asset.object_name(document, 'nodes', node), every_ms,
                              *size, lens, perspective['znear'])
