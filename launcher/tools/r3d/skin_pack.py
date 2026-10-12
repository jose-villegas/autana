"""Rig identities and entries added to scene and import packs after their LMSH entries are ready."""

from pathlib import Path
import functools

from bake import bake
from anim import skeleton_asset, tracks_asset
from build.c_header import read_define
from asset.asset_pack import NAME_BYTES
from gltf import gltf_read
from r3d import skin_asset
from r3d.import_settings import SettingsError

ID_BYTES = NAME_BYTES - 1
SKIN_OWNER = Path(__file__).resolve().parents[2] / "main/render/r3d_skin.h"
SKIN_ID_SUFFIX = read_define(SKIN_OWNER, "R3D_SKIN_ID_SUFFIX")


SOURCE_RIG_CACHE_SIZE = 16


@functools.lru_cache(maxsize=SOURCE_RIG_CACHE_SIZE)
def source_rig(source, modified, size):
    return gltf_read.load_asset(source)


def skin_id(mesh_id):
    return mesh_id + SKIN_ID_SUFFIX


def rig_ids(mesh_id, settings, cache=None, offline=False):
    rig = load_rig(settings, cache or bake.default_cache(), offline)
    if rig is None:
        raise SettingsError(f'{settings.path}: skinned_renderer requires a rig')
    _, document, _, node = rig
    _, _, skeleton = skeleton_asset.rig(document, node)
    return skin_id(mesh_id), skeleton


def load_rig(settings, cache, offline):
    """Resolve the import's exported source and sole skinned mesh node."""
    source = settings.source['path']
    origin = source
    if source.suffix == bake.BLEND_SUFFIX:
        stages = {'blend': bake.tool_digest('blend')}
        export = bake.blend_export(settings, stages)
        source = bake.fetch_all([export], bake.read_lock(), cache, offline)[export]
    if source.suffix.lower() not in gltf_read.ASSET_SUFFIXES:
        return None
    stamp = source.stat()
    document, binary = source_rig(source, stamp.st_mtime_ns, stamp.st_size)
    mesh_nodes = [i for i, node in enumerate(document.get('nodes', [])) if 'skin' in node and 'mesh' in node]
    if not mesh_nodes:
        return None
    if len(mesh_nodes) != 1:
        names = [document['nodes'][i].get('name', str(i)) for i in mesh_nodes]
        raise SettingsError(f'{origin}: one LMSH source must identify one skinned mesh node; found {names}')
    node = mesh_nodes[0]
    return origin, document, binary, node


def entries(meshes, jobs, cache, offline, influences=skin_asset.DEFAULT_INFLUENCES):
    """(id, type, bytes, source identity) for rigs in scene or standalone import packs."""
    result = []
    for mesh_id, mesh_source, mesh_entry in meshes:
        job, scene, root = jobs[mesh_source.resolve()]
        settings = job.settings
        rig = load_rig(settings, cache, offline)
        if rig is None:
            continue
        origin, document, binary, node = rig
        try:
            name, skeleton, skin = skin_asset.bake(document, binary, node, mesh_entry, influences,
                                                  skin_asset.LIGHTING_MODES[settings.skin_lighting])
        except ValueError as error:
            raise SettingsError(f'{origin}: {error}') from error
        rig_source = (origin, 'skin', document['nodes'][node]['skin'])
        result.extend([(name, skeleton_asset.TYPE, skeleton, rig_source),
                       (skin_id(mesh_id), skin_asset.TYPE, skin, (settings.path, mesh_id, 'skin'))])
        animator = getattr(job.object, 'animator', None)
        clips = list(dict.fromkeys([*settings.source.get('clips', []), *(animator.clips if animator else [])]))
        for clip in clips:
            animation = tracks_asset.find_animation(document, clip, origin)
            tracks, duration, clip_root = tracks_asset.clip_tracks(document, binary, animation)
            result.append((clip, tracks_asset.TYPE, tracks_asset.encode(tracks, duration, clip_root),
                           (origin, 'clip', clip)))
    return result


def append(pack_entries, additions, sources):
    for key, kind, data, source in additions:
        if not key or len(key.encode('utf-8')) > ID_BYTES or '\0' in key:
            raise SettingsError(f'{source}: entry id {key!r} exceeds {ID_BYTES} bytes or is invalid')
        if not key.isascii():
            raise SettingsError(f'{source}: entry id {key!r} must be ASCII')
        if key in sources:
            if sources[key] != source:
                raise SettingsError(f'entry {key!r}: {sources[key]} and {source} both make this id')
            continue
        sources[key] = source
        pack_entries.append((key, kind, data))
