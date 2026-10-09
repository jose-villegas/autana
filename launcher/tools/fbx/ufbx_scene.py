"""An FBX file read by ufbx, through ctypes. The library is ufbx_glue.c plus
the pinned submodule third_party/upstream/ufbx, compiled once into .cache/
with the host C compiler (CC, else cc, gcc or clang). The scene arrives as
glTF wants it: metres, right-handed, +Y up (see ufbx_glue.c)."""

import ctypes
import os
import pathlib
import shutil
import subprocess
import sys
from types import SimpleNamespace

import numpy as np

from . import log

HERE = pathlib.Path(__file__).resolve().parent
UFBX = HERE.parents[2] / "third_party" / "upstream" / "ufbx"
GLUE = HERE / "ufbx_glue.c"
CACHE = HERE / ".cache"

ERROR_SIZE = 1024
INFLUENCES = 4  # per vertex, as ufbx_glue.c writes them
NO_MATERIAL = -1
TRANSLATION, ROTATION, SCALE = range(3)
CHANNEL_WIDTHS = {TRANSLATION: 3, ROTATION: 4, SCALE: 3}

_lib = None


def _compiler():
    for name in (os.environ.get("CC"), "cc", "gcc", "clang"):
        if name and shutil.which(name):
            return shutil.which(name)
    raise SystemExit("no C compiler found to build ufbx: set CC")


def _library():
    global _lib
    if _lib is not None:
        return _lib
    if not (UFBX / "ufbx.c").exists():
        raise SystemExit(f"{UFBX} is missing: run `git submodule update --init third_party/upstream/ufbx`")
    CACHE.mkdir(exist_ok=True)
    suffix = ".dll" if sys.platform == "win32" else ".so"
    path = CACHE / f"ufbx_glue{suffix}"
    sources = (GLUE, UFBX / "ufbx.c", UFBX / "ufbx.h")
    if not path.exists() or path.stat().st_mtime < max(s.stat().st_mtime for s in sources):
        cc = _compiler()
        log(f"building {path.name} with {cc}")
        extra = ["-static"] if sys.platform == "win32" else ["-fPIC"]
        subprocess.run([cc, "-O2", "-shared", *extra, f"-I{UFBX}", "-o", str(path), str(GLUE), "-lm"], check=True)
    lib = ctypes.CDLL(str(path))
    size_t, c_int, c_void_p, c_char_p = ctypes.c_size_t, ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p
    for name, restype, argtypes in (
        ("fbxg_open", c_void_p, (c_char_p, c_char_p, size_t)),
        ("fbxg_close", None, (c_void_p,)),
        ("fbxg_node_count", size_t, (c_void_p,)),
        ("fbxg_node_name", c_char_p, (c_void_p, size_t)),
        ("fbxg_node_transform", c_int, (c_void_p, size_t, c_void_p)),
        ("fbxg_material_count", size_t, (c_void_p,)),
        ("fbxg_material_name", c_char_p, (c_void_p, size_t)),
        ("fbxg_material_colour", None, (c_void_p, size_t, c_void_p)),
        ("fbxg_mesh_triangles", size_t, (c_void_p, size_t, c_void_p, c_void_p)),
        ("fbxg_mesh_corners", None, (c_void_p, size_t) + (c_void_p,) * 6),
        ("fbxg_skin_joint", c_int, (c_void_p, size_t, size_t, c_void_p)),
        ("fbxg_animation_count", size_t, (c_void_p,)),
        ("fbxg_animation_name", c_char_p, (c_void_p, size_t)),
        ("fbxg_animation_nodes", size_t, (c_void_p, size_t)),
        ("fbxg_animation_node", c_int, (c_void_p, size_t, size_t, c_void_p)),
        ("fbxg_animation_keys", None, (c_void_p, size_t, size_t, c_int, c_void_p, c_void_p)),
    ):
        function = getattr(lib, name)
        function.restype, function.argtypes = restype, argtypes
    _lib = lib
    return lib


def _ptr(array):
    return array.ctypes.data_as(ctypes.c_void_p)


class FbxScene:
    """Opens `path`; use as a context manager. Nodes are indexed as glTF
    nodes are: node 0 is the root, and `parent` is -1 for it."""

    def __init__(self, path):
        self._lib = _library()
        error = ctypes.create_string_buffer(ERROR_SIZE)
        self._scene = self._lib.fbxg_open(os.fsencode(str(path)), error, ERROR_SIZE)
        if not self._scene:
            raise ValueError(f"{path}: {error.value.decode('utf-8', 'replace')}")

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self._lib.fbxg_close(self._scene)
        self._scene = None

    def _text(self, function, *index):
        return function(self._scene, *index).decode("utf-8", "replace")

    def nodes(self):
        """One namespace per node: name, parent, translation, rotation
        (xyzw), scale."""
        lib = self._lib
        out = []
        for i in range(lib.fbxg_node_count(self._scene)):
            trs = np.zeros(10)
            parent = lib.fbxg_node_transform(self._scene, i, _ptr(trs))
            out.append(SimpleNamespace(name=self._text(lib.fbxg_node_name, i), parent=parent,
                                       translation=trs[0:3], rotation=trs[3:7], scale=trs[7:10]))
        return out

    def materials(self):
        """One namespace per material: name and base colour rgba."""
        lib = self._lib
        out = []
        for i in range(lib.fbxg_material_count(self._scene)):
            rgba = np.zeros(4)
            lib.fbxg_material_colour(self._scene, i, _ptr(rgba))
            out.append(SimpleNamespace(name=self._text(lib.fbxg_material_name, i), colour=rgba))
        return out

    def mesh(self, node):
        """The triangulated corners of a node's mesh, three per triangle, or
        None when it has none: position, normal, colour (None when absent),
        joints and weights (None when unskinned), the scene material of each
        triangle, and for a skin its joints as (node, inverse bind matrix)."""
        lib = self._lib
        joints, has_colour = ctypes.c_int(), ctypes.c_int()
        triangles = lib.fbxg_mesh_triangles(self._scene, node, ctypes.byref(joints), ctypes.byref(has_colour))
        if not triangles:
            return None
        corners = 3 * triangles
        position = np.zeros((corners, 3), np.float32)
        normal = np.zeros((corners, 3), np.float32)
        colour = np.zeros((corners, 4), np.float32)
        joint = np.zeros((corners, INFLUENCES), np.uint16)
        weight = np.zeros((corners, INFLUENCES), np.float32)
        material = np.zeros(triangles, np.int32)
        lib.fbxg_mesh_corners(self._scene, node, _ptr(position), _ptr(normal), _ptr(colour), _ptr(joint),
                              _ptr(weight), _ptr(material))
        skin = []
        for j in range(joints.value):
            inverse = np.zeros(16, np.float32)
            skin.append((lib.fbxg_skin_joint(self._scene, node, j, _ptr(inverse)), inverse))
        return SimpleNamespace(position=position, normal=normal, colour=colour if has_colour.value else None,
                               joint=joint if skin else None, weight=weight if skin else None,
                               material=material, skin=skin)

    def animations(self):
        """One namespace per animation stack: name, and channels as
        (node, path, seconds, values) with path TRANSLATION, ROTATION or SCALE."""
        lib = self._lib
        out = []
        for i in range(lib.fbxg_animation_count(self._scene)):
            channels = []
            for j in range(lib.fbxg_animation_nodes(self._scene, i)):
                counts = np.zeros(3, np.uintp)
                node = lib.fbxg_animation_node(self._scene, i, j, _ptr(counts))
                for path in (TRANSLATION, ROTATION, SCALE):
                    count = int(counts[path])
                    if not count:
                        continue
                    seconds = np.zeros(count)
                    values = np.zeros((count, CHANNEL_WIDTHS[path]), np.float32)
                    lib.fbxg_animation_keys(self._scene, i, j, path, _ptr(seconds), _ptr(values))
                    channels.append((node, path, seconds, values))
            out.append(SimpleNamespace(name=self._text(lib.fbxg_animation_name, i), channels=channels))
        return out
