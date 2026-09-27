"""Wavefront OBJ and MTL loading, and the textures an MTL names, as linear-light mip chains."""

import os

import numpy as np
from PIL import Image


def load_mtl(path):
    materials = {}
    name = None
    with open(path, encoding="latin-1") as f:
        for line in f:
            parts = line.strip().split()
            if not parts:
                continue
            if parts[0] == "newmtl":
                name = parts[1]
                materials[name] = {"Kd": (1.0, 1.0, 1.0)}
            elif name and parts[0] == "Kd":
                materials[name]["Kd"] = tuple(float(v) for v in parts[1:4])
            elif name and parts[0] in ("map_Kd", "map_d"):
                materials[name][parts[0]] = parts[1].replace("\\", "/")
    return materials


def load_obj(path):
    positions, uvs = [], []
    tri_v, tri_t, tri_m = [], [], []
    material_names = []
    material_index = {}
    current = -1
    with open(path, encoding="latin-1") as f:
        for line in f:
            if line.startswith("v "):
                positions.append([float(v) for v in line.split()[1:4]])
            elif line.startswith("vt "):
                uvs.append([float(v) for v in line.split()[1:3]])
            elif line.startswith("usemtl "):
                name = line.split()[1]
                if name not in material_index:
                    material_index[name] = len(material_names)
                    material_names.append(name)
                current = material_index[name]
            elif line.startswith("f "):
                corners = []
                for c in line.split()[1:]:
                    fields = c.split("/")
                    corners.append((int(fields[0]) - 1, int(fields[1]) - 1 if len(fields) > 1 and fields[1] else 0))
                for i in range(1, len(corners) - 1):
                    tri = (corners[0], corners[i], corners[i + 1])
                    tri_v.append([c[0] for c in tri])
                    tri_t.append([c[1] for c in tri])
                    tri_m.append(current)
    return (
        np.array(positions, dtype=np.float64),
        np.array(uvs, dtype=np.float64),
        np.array(tri_v, dtype=np.int64),
        np.array(tri_t, dtype=np.int64),
        np.array(tri_m, dtype=np.int64),
        material_names,
    )


class Texture:
    """A linear-light mip chain, sampled bilinearly with wrap-around."""

    def __init__(self, path, alpha_path=None):
        image = Image.open(path).convert("RGBA")
        rgba = np.asarray(image, dtype=np.float64) / 255.0
        rgb = rgba[..., :3] ** 2.2
        alpha = rgba[..., 3:4]
        if alpha_path is not None:
            mask = Image.open(alpha_path).convert("L").resize(image.size)
            alpha = np.asarray(mask, dtype=np.float64)[..., None] / 255.0
        level = np.concatenate([rgb, alpha], axis=2)
        self.levels = [level]
        while min(level.shape[0], level.shape[1]) > 1:
            h, w = level.shape[0] // 2 * 2, level.shape[1] // 2 * 2
            level = level[:h, :w]
            level = 0.25 * (level[0::2, 0::2] + level[1::2, 0::2] + level[0::2, 1::2] + level[1::2, 1::2])
            self.levels.append(level)

    @property
    def size(self):
        return self.levels[0].shape[1], self.levels[0].shape[0]

    def sample(self, uv, lod):
        """uv (n,2), lod (n,) in mip levels; returns (n,4) linear RGBA."""
        out = np.zeros((len(uv), 4))
        lod = np.clip(np.round(lod).astype(np.int64), 0, len(self.levels) - 1)
        for level_index in np.unique(lod):
            sel = lod == level_index
            level = self.levels[level_index]
            h, w = level.shape[:2]
            x = uv[sel, 0] * w - 0.5
            y = (1.0 - uv[sel, 1]) * h - 0.5
            x0 = np.floor(x).astype(np.int64)
            y0 = np.floor(y).astype(np.int64)
            fx = (x - x0)[:, None]
            fy = (y - y0)[:, None]
            x0m, x1m = x0 % w, (x0 + 1) % w
            y0m, y1m = y0 % h, (y0 + 1) % h
            top = level[y0m, x0m] * (1 - fx) + level[y0m, x1m] * fx
            bottom = level[y1m, x0m] * (1 - fx) + level[y1m, x1m] * fx
            out[sel] = top * (1 - fy) + bottom * fy
        return out


def load_textures(root, materials, names):
    textures = []
    for name in names:
        m = materials.get(name, {})
        if "map_Kd" in m:
            alpha = os.path.join(root, m["map_d"]) if "map_d" in m else None
            textures.append(Texture(os.path.join(root, m["map_Kd"]), alpha))
        else:
            textures.append(None)
    return textures
