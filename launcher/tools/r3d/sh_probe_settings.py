"""Validate and key the import's incident-radiance probe grid request."""

import tomllib
from types import SimpleNamespace

from r3d.import_settings import SettingsError, check_keys, count, number

DIRECTIONS = 512
SPP = 4
SUFFIX = '.shpr'


def load(path):
    with open(path, 'rb') as source:
        table = tomllib.load(source).get('geometry', {}).get('sh_probes')
    if table is None:
        return None
    where = 'geometry.sh_probes'
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
        raise SettingsError('geometry.sh_probes needs scene bake.indirect')
    settings = job.settings
    return canonical(dict(probes=load(settings.path), sources=sources,
                          seed=settings.seed, alpha_keep=settings.alpha_keep,
                          double_sided=settings.double_sided, camera=scene.camera.component.region,
                          near=scene.camera.component.near_z, lights=scene.lights,
                          bounces=job.bake.indirect.bounces, albedo_boost=scene.indirect.albedo_boost))
