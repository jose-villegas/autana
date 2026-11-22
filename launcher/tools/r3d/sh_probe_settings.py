"""Validate and key a scene's sibling incident-radiance probe grid request."""

import pathlib
import tomllib
from types import SimpleNamespace

from r3d.import_settings import SettingsError, check_keys, count, number
from r3d.scene_name import scene_id
from r3d.sh_probe_asset import SUFFIX

DIRECTIONS = 512
SPP = 4
REQUEST_SUFFIX = '.probes.toml'


def load(path):
    path = pathlib.Path(path)
    request = path.with_name(scene_id(path) + REQUEST_SUFFIX)
    if not request.is_file():
        return None
    with request.open('rb') as source:
        table = tomllib.load(source)
    where = str(request)
    check_keys(table, (), where, optional=('spacing', 'directions', 'spp'))
    spacing = number(table['spacing'], f'{where}.spacing') if 'spacing' in table else None
    if spacing is not None and spacing <= 0:
        raise SettingsError(f'{where}.spacing must be positive')
    return SimpleNamespace(spacing=spacing,
                           directions=count(table.get('directions', DIRECTIONS), f'{where}.directions'),
                           spp=count(table.get('spp', SPP), f'{where}.spp'))


def recipe(job, scene, sources):
    from r3d.reference_set import canonical

    if job.bake is None or job.bake.indirect is None:
        raise SettingsError('SH probes need scene bake.indirect')
    settings = job.settings
    return canonical(dict(probes=load(scene.path), sources=sources,
                          seed=settings.seed, alpha_keep=settings.alpha_keep,
                          double_sided=settings.double_sided, camera=scene.camera.component.region,
                          near=scene.camera.component.near_z, lights=scene.lights,
                          bounces=job.bake.indirect.bounces, albedo_boost=scene.indirect.albedo_boost))
