"""Uncached rig entries added to a scene pack after its LMSH entries are ready."""

from anim import skeleton_asset, tracks_asset
from asset.asset_pack import NAME_BYTES
from gltf import gltf_read
from r3d import skin_asset
from r3d.import_settings import SettingsError

ID_BYTES = NAME_BYTES - 1


def entries(meshes, jobs, cache, offline, influences=skin_asset.DEFAULT_INFLUENCES):
    """(id, type, bytes, source identity) for rigs placed by a scene."""
    from bake import bake

    loaded, result = {}, []
    for mesh_id, mesh_source, mesh_entry in meshes:
        job, scene, root = jobs[mesh_source.resolve()]
        if scene is None:
            continue
        settings = job.settings
        source = settings.source['path']
        origin = source
        if source.suffix == bake.BLEND_SUFFIX:
            stages = {'blend': bake.tool_digest('blend')}
            key = bake.blend_key(settings, stages)
            export = bake.Bake(output=source.with_suffix(bake.GLB_SUFFIX).name, source=settings.path,
                               holder=source.name, kind='blend', key=key, suffix=bake.GLB_SUFFIX,
                               tree=source.with_suffix(bake.GLB_SUFFIX), job=settings)
            source = bake.fetch_all([export], bake.read_lock(), cache, offline)[export]
        if source.suffix.lower() not in gltf_read.ASSET_SUFFIXES:
            continue
        if source not in loaded:
            loaded[source] = gltf_read.load_asset(source)
        document, binary = loaded[source]
        mesh_nodes = [i for i, node in enumerate(document.get('nodes', [])) if 'skin' in node and 'mesh' in node]
        if not mesh_nodes:
            continue
        if len(mesh_nodes) != 1:
            names = [document['nodes'][i].get('name', str(i)) for i in mesh_nodes]
            raise SettingsError(f'{origin}: one LMSH source must identify one skinned mesh node; found {names}')
        node = mesh_nodes[0]
        try:
            name, skeleton, skin = skin_asset.bake(document, binary, node, mesh_entry, influences)
        except ValueError as error:
            raise SettingsError(f'{origin}: {error}') from error
        rig_source = (origin, 'skin', document['nodes'][node]['skin'])
        result.extend([(name, skeleton_asset.TYPE, skeleton, rig_source),
                       (mesh_id + '.skin', skin_asset.TYPE, skin, (settings.path, mesh_id, 'skin'))])
        clips = settings.source.get('clips', [])
        if len(set(clips)) != len(clips):
            raise SettingsError(f'{settings.path}: duplicate clip names {clips!r}')
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
