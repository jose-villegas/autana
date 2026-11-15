"""Which baked renderers of a scene draw textured (render/texture/): `<scene>.textures.toml` beside
`<scene>.scene.toml`, one row per renderer, named by its scene object.

    [[renderers]]
    object = "street"                 # a scene object with a baked mesh_renderer
    materials = ["Brick_*"]           # source materials, matched as objects.meshes match them
    size = 64                         # each texture's side in texels, level 0

The texture bake alone reads this file, so a scene's other bakes keep their keys when it changes."""
import functools
import pathlib
import tomllib
from types import SimpleNamespace

from r3d.import_settings import SettingsError, check_keys, count, strings, text
from r3d.texture.texture_asset import size_log2_of

SCENE_SUFFIX = ".scene.toml"
SUFFIX = ".textures.toml"


def settings_path(scene_path):
    scene_path = pathlib.Path(scene_path)
    return scene_path.with_name(scene_path.name.removesuffix(SCENE_SUFFIX) + SUFFIX)


@functools.cache
def load(path):
    """{object name: SimpleNamespace(materials, size)} of the textures file at `path`, empty when there is none."""
    path = pathlib.Path(path)
    if not path.is_file():
        return {}
    table = tomllib.loads(path.read_text(encoding="utf-8"))
    check_keys(table, ("renderers",), str(path))
    rows = {}
    for index, row in enumerate(table["renderers"]):
        where = f"{path.name}: renderers[{index}]"
        check_keys(row, ("object", "materials", "size"), where)
        name = text(row["object"], f"{where}.object")
        textures = SimpleNamespace(materials=strings(row["materials"], f"{where}.materials"),
                                   size=count(row["size"], f"{where}.size"))
        if not textures.materials:
            raise SettingsError(f"{where}.materials must name a material")
        try:
            size_log2_of(textures.size)
        except ValueError as error:
            raise SettingsError(f"{where}.size: {error}") from error
        if name in rows:
            raise SettingsError(f"{where}.object {name!r} is named twice")
        rows[name] = textures
    return rows


def renderer_textures(scene_path, object_name):
    """The textures of scene object `object_name` in the scene at `scene_path`, or None when it draws plain."""
    return load(settings_path(scene_path).resolve()).get(object_name)
