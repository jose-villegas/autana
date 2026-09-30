"""What drawing a baked mesh's cluster levels would save, before any runtime
does it: at each pose, picks per cluster the level whose projected error is at
most a tolerance and whose parent's is above it, counts what that draws
against the finest level alone, and renders both on the host to diff them.

    python -m r3d.lod_eval MESH_mesh_generated.c POSES.txt [options]

Run from launcher/tools. MESH is any baked mesh; POSES holds one pose per
line, eye x y z then forward x y z in model units, '#' starting a comment.
"""

import argparse
import pathlib
import sys

import numpy as np

from r3d import log
from r3d.lit_mesh import LOD_TOP, read_lit_mesh


class Camera:
    """A pinhole looking along `forward` with +Y up, its lens fitted to the
    shorter side of a width by height picture as r3d_lit_view_look() does."""

    def __init__(self, eye, forward, width, height, half_fov_short_tan, near):
        f = np.asarray(forward, dtype=np.float64)
        f = f / np.linalg.norm(f)
        right = np.cross(f, [0.0, 1.0, 0.0])
        right /= np.linalg.norm(right)
        down = np.cross(f, right)
        self.eye = np.asarray(eye, dtype=np.float64)
        self.axes = np.array([right, down, f])
        self.width, self.height, self.near = width, height, near
        self.k = min(width, height) / (2.0 * half_fov_short_tan)  # pixels at unit depth per unit of size

    def to_camera(self, points):
        return (points - self.eye) @ self.axes.T

    def project(self, cam):
        z = cam[..., 2]
        return np.stack([self.width / 2 + self.k * cam[..., 0] / z, self.height / 2 + self.k * cam[..., 1] / z], axis=-1)


class Level:
    """A mesh's clusters as flat arrays across every level: the triangle
    range each holds, its box, its LOD record and the array it indexes."""

    def __init__(self, mesh):
        scale = float(mesh.position_scale)
        parts = [(mesh.pos, mesh.rgb, mesh.tris, mesh.clusters)]
        if mesh.lod:
            parts.append((mesh.lod.pos, mesh.lod.rgb, mesh.lod.tris, mesh.lod.clusters))
        self.scale = scale
        self.pos = [p[0] / scale for p in parts]  # model units
        self.rgb = [p[1] for p in parts]
        self.tris = [p[2] for p in parts]
        rows = [(a, c) for a, p in enumerate(parts) for c in p[3]]
        self.array = np.array([a for a, _ in rows])
        self.first = np.array([c[2] for _, c in rows])
        self.count = np.array([c[3] for _, c in rows])
        self.lo = np.array([c[4] for _, c in rows], dtype=np.float64) / scale
        self.hi = np.array([c[5] for _, c in rows], dtype=np.float64) / scale
        self.double = np.array([c[6] for _, c in rows])
        rec = mesh.records
        if not rec:  # no levels: every cluster is finest and never replaced
            rec = [{"self": (np.zeros(3), 1, 0.0), "parent": (np.zeros(3), 1, LOD_TOP), "cone": (np.zeros(3), 127),
                    "level": 0}] * len(rows)
        self.level = np.array([r["level"] for r in rec])
        self.self_c = np.array([r["self"][0] for r in rec], dtype=np.float64) / scale
        self.self_r = np.array([r["self"][1] for r in rec], dtype=np.float64) / scale
        self.self_e = np.array([r["self"][2] for r in rec], dtype=np.float64) / scale
        self.parent_c = np.array([r["parent"][0] for r in rec], dtype=np.float64) / scale
        self.parent_r = np.array([r["parent"][1] for r in rec], dtype=np.float64) / scale
        self.parent_e = np.array([r["parent"][2] for r in rec], dtype=np.float64) / scale
        self.cone_axis = np.array([r["cone"][0] for r in rec], dtype=np.float64) / 127.0
        self.cone_cutoff = np.array([r["cone"][1] for r in rec], dtype=np.float64) / 127.0

    def __len__(self):
        return len(self.first)


def projected(camera, centre, radius, error):
    """Pixels an error of `error` model units spans at the sphere's nearest point."""
    distance = np.linalg.norm(centre - camera.eye, axis=-1)
    return error * camera.k / np.maximum(distance - radius, camera.near)


def in_view(camera, lo, hi):
    """Whether each box may be on screen: not wholly behind the near plane or
    outside one side plane."""
    corners = np.stack(np.meshgrid([0, 1], [0, 1], [0, 1], indexing="ij"), axis=-1).reshape(8, 3)
    box = lo[:, None, :] + (hi - lo)[:, None, :] * corners[None]
    cam = camera.to_camera(box)
    x, y, z = cam[..., 0], cam[..., 1], cam[..., 2]
    hw, hh = camera.width / 2, camera.height / 2
    outside = [z <= camera.near, x < -hw * z / camera.k, x > hw * z / camera.k, y < -hh * z / camera.k,
               y > hh * z / camera.k]
    return ~np.any([np.all(o, axis=1) for o in outside], axis=0)


def facing(camera, level):
    """False for a cluster the cone shows to face away from the eye."""
    centre = (level.lo + level.hi) / 2
    radius = np.linalg.norm(level.hi - level.lo, axis=1) / 2
    d = centre - camera.eye
    length = np.linalg.norm(d, axis=1)
    back = np.sum(d * level.cone_axis, axis=1) >= level.cone_cutoff * length + radius
    return ~back | (level.double == 1)


def cut(level, camera, tolerance=1.0):
    """The clusters whose own projected error is at most `tolerance` pixels
    and whose parent's is above it: exactly one per stretch of surface, with
    no regard to what is on screen."""
    own = projected(camera, level.self_c, level.self_r, level.self_e)
    parent = projected(camera, level.parent_c, level.parent_r, level.parent_e)
    return (own <= tolerance) & (parent > tolerance)


def pick(level, camera, tolerance=1.0):
    """The clusters to draw: the cut, in view and not facing away."""
    return cut(level, camera, tolerance) & in_view(camera, level.lo, level.hi) & facing(camera, level)


def finest(level, camera):
    """The clusters a renderer drawing only the finest level would draw."""
    return (level.level == 0) & in_view(camera, level.lo, level.hi) & facing(camera, level)


def cluster_triangles(level, k):
    a, first, count = level.array[k], level.first[k], level.count[k]
    t = level.tris[a][first : first + count]
    return level.pos[a][t], level.rgb[a][t]


def gather(level, mask):
    """(corner positions (n,3,3), corner colours (n,3,3)) of the picked clusters' triangles."""
    parts = [cluster_triangles(level, k) for k in np.flatnonzero(mask)]
    if not parts:
        return np.zeros((0, 3, 3)), np.zeros((0, 3, 3))
    return np.concatenate([p[0] for p in parts]), np.concatenate([p[1] for p in parts])


def render(camera, corners, colours, double):
    """A z-buffered Gouraud rendering, pixel centres, first drawn wins a tie.
    Returns (image (h,w,3) uint8, triangles drawn: in front of the near plane,
    facing and touching the picture)."""
    w, h = camera.width, camera.height
    image = np.zeros((h, w, 3))
    depth = np.zeros((h, w))
    cam = camera.to_camera(corners)
    front = np.all(cam[..., 2] > camera.near, axis=1)
    screen = camera.project(np.where(front[:, None, None], cam, 1.0))
    drawn = 0
    for i in np.flatnonzero(front):
        a, b, c = screen[i]
        area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])
        if area == 0 or (area >= 0 and not double[i]):
            continue
        x0, x1 = max(int(np.floor(min(a[0], b[0], c[0]))), 0), min(int(np.ceil(max(a[0], b[0], c[0]))), w - 1)
        y0, y1 = max(int(np.floor(min(a[1], b[1], c[1]))), 0), min(int(np.ceil(max(a[1], b[1], c[1]))), h - 1)
        if x0 > x1 or y0 > y1:
            continue
        drawn += 1
        xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        w0 = ((b[0] - xs) * (c[1] - ys) - (c[0] - xs) * (b[1] - ys)) / area
        w1 = ((c[0] - xs) * (a[1] - ys) - (a[0] - xs) * (c[1] - ys)) / area
        w2 = 1.0 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        z = 1.0 / cam[i, :, 2]
        iz = w0 * z[0] + w1 * z[1] + w2 * z[2]
        sub = depth[y0 : y1 + 1, x0 : x1 + 1]
        take = inside & (iz > sub)
        if not take.any():
            continue
        sub[take] = iz[take]
        rgb = w0[..., None] * colours[i, 0] + w1[..., None] * colours[i, 1] + w2[..., None] * colours[i, 2]
        image[y0 : y1 + 1, x0 : x1 + 1][take] = rgb[take]
    return np.clip(np.rint(image), 0, 255).astype(np.uint8), drawn


def gather_double(level, mask):
    flags = [np.full(level.count[k], level.double[k]) for k in np.flatnonzero(mask)]
    return np.concatenate(flags) if flags else np.zeros(0, dtype=np.int64)


def evaluate(level, camera, tolerance=1.0, images=False):
    """Counts and, when asked, the two renderings for one pose."""
    fine, lod = finest(level, camera), pick(level, camera, tolerance)
    out = {
        "clusters_finest": int(fine.sum()),
        "clusters_lod": int(lod.sum()),
        "clusters_coarser": int((lod & (level.level > 0)).sum()),
        "triangles_finest": int(level.count[fine].sum()),
        "triangles_lod": int(level.count[lod].sum()),
    }
    if images:
        results = []
        for mask in (fine, lod):
            corners, colours = gather(level, mask)
            results.append(render(camera, corners, colours, gather_double(level, mask)))
        (a, drawn_fine), (b, drawn_lod) = results
        gap = np.abs(a.astype(int) - b.astype(int)).max(axis=2)
        out.update({
            "drawn_finest": drawn_fine, "drawn_lod": drawn_lod, "images": (a, b),
            "differ": float((gap > 0).mean()), "differ_big": float((gap > 32).mean()),
            "mean_gap": float(np.abs(a.astype(int) - b.astype(int)).mean()),
        })
    return out


def read_poses(path):
    poses = []
    for line in pathlib.Path(path).read_text().splitlines():
        line = line.split("#")[0].split()
        if line:
            poses.append((line[:3], line[3:6]))
    return [(np.array(e, dtype=float), np.array(f, dtype=float)) for e, f in poses]


def save(prefix, result):
    from PIL import Image

    a, b = result["images"]
    gap = np.clip(np.abs(a.astype(int) - b.astype(int)) * 4, 0, 255).astype(np.uint8)
    Image.fromarray(np.concatenate([a, b, gap], axis=1)).save(prefix + ".png")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mesh")
    parser.add_argument("poses")
    parser.add_argument("--width", type=int, default=184)
    parser.add_argument("--height", type=int, default=224)
    parser.add_argument("--half-fov-short-tan", type=float, default=0.62)
    parser.add_argument("--near", type=float, default=6.0, help="model units")
    parser.add_argument("--tolerance", type=float, default=1.0, help="projected error, pixels")
    parser.add_argument("--no-render", action="store_true", help="counts only")
    parser.add_argument("--save", help="write finest | LOD | 4x difference as PNGs with this path prefix")
    args = parser.parse_args(argv)

    level = Level(read_lit_mesh(args.mesh))
    total = {}
    header = "pose  clusters finest/lod (coarser)   triangles finest/lod (saved)"
    header += "   drawn finest/lod (saved)   pixels differ / >32 / mean" if not args.no_render else ""
    print(header)
    for n, (eye, forward) in enumerate(read_poses(args.poses)):
        camera = Camera(eye, forward, args.width, args.height, args.half_fov_short_tan, args.near)
        r = evaluate(level, camera, args.tolerance, images=not args.no_render)
        line = (f"{n:4d}  {r['clusters_finest']:6d} /{r['clusters_lod']:5d} ({r['clusters_coarser']:4d})"
                f"   {r['triangles_finest']:9d} /{r['triangles_lod']:6d} ({1 - r['triangles_lod'] / max(r['triangles_finest'], 1):5.1%})")
        if not args.no_render:
            line += (f"   {r['drawn_finest']:6d} /{r['drawn_lod']:5d} ({1 - r['drawn_lod'] / max(r['drawn_finest'], 1):5.1%})"
                     f"   {r['differ']:6.2%} / {r['differ_big']:6.2%} / {r['mean_gap']:.3f}")
            if args.save:
                save(f"{args.save}{n}", r)
        print(line)
        for key in ("clusters_finest", "clusters_lod", "clusters_coarser", "triangles_finest", "triangles_lod",
                    "drawn_finest", "drawn_lod"):
            total[key] = total.get(key, 0) + r.get(key, 0)
    if total:
        log("total", {k: v for k, v in total.items()})


if __name__ == "__main__":
    sys.exit(main())
