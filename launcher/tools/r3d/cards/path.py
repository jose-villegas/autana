"""Sample the scene camera with the card selection clip and fit pose resolution."""

from types import SimpleNamespace

from r3d.import_settings import SettingsError
from r3d.mesh_source import camera_path_poses

PATH_SAMPLE_MS = 1000


def camera_path(path, scene, renderer):
    size = getattr(getattr(renderer, "fit", None), "size", None)
    if scene is None or size is None:
        raise SettingsError("geometry.cards.path needs a scene renderer with fit.poses.size")
    camera = scene.camera.component
    if camera.path is None:
        raise SettingsError("geometry.cards.path needs the scene camera's path.node")
    camera = SimpleNamespace(**{**vars(camera), "path": SimpleNamespace(**{**vars(camera.path), "animation": path})})
    sampled_scene = SimpleNamespace(camera=SimpleNamespace(component=camera))
    visibility = SimpleNamespace(size=size)
    return camera_path_poses(sampled_scene, visibility, PATH_SAMPLE_MS, either_way_up=False)
