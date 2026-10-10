#!/usr/bin/env python3
"""Fit a baked mesh's vertex positions and colours to reference renders
along a camera path, keeping its triangles (appearance-driven
simplification, Hasselgren et al. 2021). A smooth mesh fits a colour per
vertex, a flat one a colour per triangle.

    python launcher/tools/r3d/appearance_simplify.py --scene SCENE.scene.toml --start NAME.mesh \\
        --poses POSES.txt --reference DIR [--poses ... --reference ...] --out DIR [--per-shot]

--start is a baked smooth or flat mesh, usually the simplifier's output at the
triangle budget. Each --poses file (tools/anim/track_host.py) pairs with
the --reference directory reference_render.py wrote for it; --scene is the
scene they were rendered from, whose camera background shows where nothing
is drawn. All pairs train one mesh; with --per-shot each pair trains its
own. A mesh is written as NAME.mesh by the same writer as mesh_import.py,
under DIR, or DIR/shot<k>, and as a coloured OBJ beside it.

The optimiser needs PyTorch with CUDA and nvdiffrast; see the README.
"""

import argparse
import pathlib
import sys
import time
from typing import NamedTuple

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import log  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402
from r3d.lit_mesh import finest_triangles, read_lit_mesh  # noqa: E402
from r3d.cost_model import load as load_cost  # noqa: E402
from r3d.poses import camera_basis, read_poses  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))

from render_compare import lab  # noqa: E402

FAR = 1.0e5


class FitMesh(NamedTuple):
    """What the fit moves and colours. points are the welded positions in
    model units; vertex_point maps each vertex to its position, so a colour
    seam cannot open into a crack when positions move. rgb (0..1) is one
    colour per vertex, or per triangle when the mesh is flat."""
    points: np.ndarray
    rgb: np.ndarray
    tris: np.ndarray
    double: np.ndarray
    scale: int
    vertex_point: np.ndarray
    flat: bool = False


def start_mesh(path):
    """The FitMesh of a generated mesh: a smooth one welded back to one
    vertex per position and colour, a flat one to one per position."""
    mesh = read_lit_mesh(path)
    q, rgb, tris, double, face = finest_triangles(mesh)
    flat = rgb is None
    points, vertex_point = np.unique(q, axis=0, return_inverse=True)
    return FitMesh(points / float(mesh.position_scale), (face if flat else rgb) / 255.0, tris, double, mesh.position_scale,
                   vertex_point.reshape(-1), flat)


def point_edges(tris, vertex_point):
    """The unique undirected edges between welded positions."""
    corners = vertex_point[tris]
    edges = np.concatenate([corners[:, [0, 1]], corners[:, [1, 2]], corners[:, [2, 0]]])
    edges = np.sort(edges[edges[:, 0] != edges[:, 1]], axis=1)
    return np.unique(edges, axis=0)


def projection(eye, forward, width, height, lens, near):
    """A 4x4 world-to-clip matrix whose pixel centres cast the same rays as
    reference_render.py's pinhole, clip y up."""
    right, up, forward = camera_basis(forward)
    short = min(width, height)
    horizontal, vertical = lens * width / short, lens * height / short
    a, b = (FAR + near) / (FAR - near), -2.0 * FAR * near / (FAR - near)
    rows = np.array([right / horizontal, up / vertical, forward * a, forward])
    offsets = -rows @ np.asarray(eye, dtype=float) + np.array([0.0, 0.0, b, 0.0])
    return np.concatenate([rows, offsets[:, None]], axis=1)


def pose_views(poses_path, scale):
    """[(clip matrix, None, None, eye)] for every pose of a poses file: views
    with no target, for counting what each triangle shows."""
    width, height, lens, near, poses = read_poses(poses_path)
    return [(projection(pose[:3], pose[3:], width, height, lens, near), None, None, pose[:3]) for pose in poses]


def load_views(pairs, scale):
    """[(clip matrix, target sRGB image 0..1, target normals or None, eye)],
    images at render size, for every pose of every (poses file, reference
    directory) pair, and the render size. The normals are the reference's
    NNNN.normal.npy, when it wrote them."""
    from PIL import Image

    views = []
    size = None
    for poses_path, reference in pairs:
        width, height, lens, near, poses = read_poses(poses_path)
        images = sorted(pathlib.Path(reference).glob("*.png"))
        if len(images) != len(poses):
            raise ValueError(f"{reference} has {len(images)} images for {len(poses)} poses")
        size = (width * scale, height * scale)
        for pose, image in zip(poses, images):
            target = np.asarray(Image.open(image).convert("RGB"), dtype=np.float32) / 255.0
            target = target.repeat(scale, axis=0).repeat(scale, axis=1)
            normal_path = image.with_name(image.stem + ".normal.npy")
            normal = None
            if normal_path.exists():
                normal = np.load(normal_path).astype(np.float32).repeat(scale, axis=0).repeat(scale, axis=1)
            views.append((projection(pose[:3], pose[3:], width, height, lens, near), target, normal, pose[:3]))
    return views, size


def delta_e76(a, b):
    """Per-pixel CIE76 dE in torch; the floor and the epsilon keep the
    gradient finite at black and at zero."""
    import torch

    return ((lab(torch, a, 1e-6) - lab(torch, b, 1e-6)) ** 2).sum(dim=-1).add(1e-6).sqrt()


def vertex_normals(points, tris, vertex_point):
    """Per vertex, the unit area-weighted normal of the triangles around its
    welded position, in torch, differentiable in `points`."""
    import torch

    corners = points[vertex_point][tris]
    face = torch.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0], dim=1)
    around = torch.zeros_like(points).index_add_(0, vertex_point[tris].reshape(-1), face.repeat_interleave(3, dim=0))
    return torch.nn.functional.normalize(around, dim=1)[vertex_point]


class Renderer:
    """Draws the mesh as the device does: Gouraud vertex colours, or with
    `flat` one colour per triangle, single-sided triangles culled when they
    face away, `clear` (0..1 RGB) where nothing is drawn. A flat mesh is drawn
    with three vertices of its own per triangle, so every edge between two
    colours is antialiased and moves its positions."""

    def __init__(self, tris, double, vertex_point, size, device, clear=(0.0, 0.0, 0.0), flat=False):
        import nvdiffrast.torch as dr
        import torch

        if flat:
            vertex_point = np.asarray(vertex_point)[tris].reshape(-1)
            tris = np.arange(len(vertex_point)).reshape(-1, 3)
        self.flat = flat
        self.dr = dr
        self.context = dr.RasterizeCudaContext(device=device)
        self.tris = torch.as_tensor(tris, dtype=torch.int32, device=device)
        self.double = torch.as_tensor(np.asarray(double, dtype=bool), device=device)
        self.vertex_point = torch.as_tensor(vertex_point, dtype=torch.int64, device=device)
        self.size = size
        self.clear = torch.as_tensor(clear, dtype=torch.float32, device=device)

    def clip(self, points, clip_matrix):
        """Clip-space positions of every vertex, y up."""
        import torch

        positions = points[self.vertex_point]
        return torch.cat([positions, torch.ones_like(positions[:, :1])], dim=1) @ clip_matrix.T

    def facing(self, clip):
        """Which triangles the device would draw: double-sided, front-facing,
        or crossing the eye plane, where facing is not yet known."""
        import torch

        with torch.no_grad():
            corners = clip[self.tris.long()]
            w = corners[..., 3]
            xy = corners[..., :2] / w.clamp_min(1e-6)[..., None]
            area = (xy[:, 1, 0] - xy[:, 0, 0]) * (xy[:, 2, 1] - xy[:, 0, 1]) - (xy[:, 2, 0] - xy[:, 0, 0]) * (xy[:, 1, 1] - xy[:, 0, 1])
            return self.double | (area > 0) | (w <= 0).any(dim=1)

    def visible_ids(self, points, clip_matrix):
        """The triangle each pixel shows, -1 where none."""
        import torch

        clip = self.clip(points, clip_matrix)
        keep = torch.nonzero(self.facing(clip))[:, 0]
        width, height = self.size
        rast, _ = self.dr.rasterize(self.context, clip[None].contiguous(), self.tris[keep].contiguous(), resolution=[height, width])
        local = rast[0, ..., 3].long() - 1
        return torch.where(local >= 0, keep[local.clamp_min(0)], local)

    def normals(self, points, clip_matrix, eye, clip=None):
        """(per-pixel world normal turned toward `eye`, covered mask): the
        mesh's area-weighted vertex normals, interpolated."""
        import torch

        clip = self.clip(points, clip_matrix) if clip is None else clip
        tris = self.tris[self.facing(clip)].contiguous()
        width, height = self.size
        rast, _ = self.dr.rasterize(self.context, clip[None].contiguous(), tris, resolution=[height, width])
        vertex = vertex_normals(points, self.tris.long(), self.vertex_point)
        attributes = torch.cat([vertex, points[self.vertex_point]], dim=1)[None].contiguous()
        values, _ = self.dr.interpolate(attributes, rast, tris)
        normal = torch.nn.functional.normalize(values[0, ..., :3], dim=-1)
        towards = torch.as_tensor(eye, dtype=normal.dtype, device=normal.device) - values[0, ..., 3:]
        normal = torch.where((normal * towards).sum(-1, keepdim=True) < 0, -normal, normal)
        return normal.flip(0), (rast[0, ..., 3] > 0).flip(0)

    def __call__(self, points, colours, clip_matrix, clip=None):
        import torch

        clip = self.clip(points, clip_matrix) if clip is None else clip
        tris = self.tris[self.facing(clip)].contiguous()
        clip = clip[None].contiguous()
        width, height = self.size
        rast, _ = self.dr.rasterize(self.context, clip, tris, resolution=[height, width])
        if self.flat:
            colours = colours.repeat_interleave(3, dim=0)
        colour, _ = self.dr.interpolate(colours[None].contiguous(), rast, tris)
        colour = torch.where(rast[..., 3:] > 0, colour, self.clear.expand_as(colour))
        colour = self.dr.antialias(colour.contiguous(), rast, clip, tris)
        return colour[0].flip(0)


def coverage(mesh, views, size, device="cuda"):
    """Per triangle, the pixels it shows summed over every view: zero for a
    triangle no view sees, small for a sliver or a speck."""
    import torch

    render = Renderer(mesh.tris, mesh.double, mesh.vertex_point, size, device, flat=mesh.flat)
    points = torch.as_tensor(mesh.points, dtype=torch.float32, device=device)
    total = torch.zeros(len(mesh.tris), dtype=torch.int64, device=device)
    with torch.no_grad():
        for matrix, *_rest in views:
            ids = render.visible_ids(points, torch.as_tensor(matrix, dtype=torch.float32, device=device))
            ids = ids[ids >= 0]
            total += torch.bincount(ids, minlength=len(mesh.tris))
    return total.cpu().numpy()


def prune(mesh, shown, budget):
    """The mesh without the triangles that show least, down to `budget`:
    every triangle no view shows goes first, then those showing fewest
    pixels. A flat mesh's colours go with their triangles."""
    order = np.argsort(-np.asarray(shown), kind="stable")
    keep = np.sort(order[: min(budget, int(np.count_nonzero(shown)))])
    return mesh._replace(rgb=mesh.rgb[keep] if mesh.flat else mesh.rgb, tris=mesh.tris[keep],
                         double=np.asarray(mesh.double)[keep])


def uniform_laplacian(points, edges):
    """Each position minus the mean of its neighbours."""
    import torch

    total = torch.zeros_like(points)
    count = torch.zeros(len(points), 1, device=points.device)
    for a, b in ((0, 1), (1, 0)):
        total.index_add_(0, edges[:, a], points[edges[:, b]])
        count.index_add_(0, edges[:, a], torch.ones(len(edges), 1, device=points.device))
    return points - total / count.clamp_min(1.0)


def predicted_ms(xp, cost, clip, tris, double, size, scale):
    """cost_model's milliseconds for one view, less the constant and the
    cluster terms, which moving vertices cannot change."""
    from r3d.cost_model import triangle_terms, variable_ms

    drawn, rows, pixels = triangle_terms(xp, clip, tris, double, size[0] // scale, size[1] // scale)
    return variable_ms(cost, drawn, rows, pixels)


def normal_pairs(render, points, view, clip):
    """(the mesh's normals, the reference's, the pixels both cover) in `view`."""
    matrix, _target, reference, eye = view
    normal, covered = render.normals(points, matrix, eye, clip)
    return normal, reference, covered & (reference.abs().sum(-1) > 0)


def normal_l1(render, points, view, clip):
    """Mean L1 distance between the mesh's and the reference's normals over
    the pixels both cover, and the angles there in degrees."""
    import torch

    normal, reference, both = normal_pairs(render, points, view, clip)
    if not bool(both.any()):
        return normal.sum() * 0, normal.new_zeros(0)
    distance = (normal - reference).abs().sum(-1)[both].mean()
    cosine = (normal * reference).sum(-1)[both].clamp(-1.0, 1.0)
    return distance, torch.rad2deg(torch.acos(cosine.detach()))


def normal_error(mesh, views, size, device="cuda", angle_dir=None):
    """Mean angular error in degrees between the mesh's normals and the
    reference's, over the pixels both cover in every view. With `angle_dir`,
    each view's per-pixel angle is also written there as NNNN.angle.npy, NaN
    where the mesh or the reference shows nothing."""
    import torch

    render = Renderer(mesh.tris, mesh.double, mesh.vertex_point, size, device, flat=mesh.flat)
    points = torch.as_tensor(mesh.points, dtype=torch.float32, device=device)
    angles = []
    with torch.no_grad():
        for index, (matrix, target, reference, eye) in enumerate(views):
            view = (torch.as_tensor(matrix, dtype=torch.float32, device=device), None,
                    torch.as_tensor(reference, device=device), eye)
            angles.append(normal_l1(render, points, view, None)[1])
            if angle_dir is not None:
                normal, reference, both = normal_pairs(render, points, view, None)
                cosine = (normal * reference).sum(-1).clamp(-1.0, 1.0)
                degrees = torch.where(both, torch.rad2deg(torch.acos(cosine)), torch.full_like(cosine, float("nan")))
                pathlib.Path(angle_dir).mkdir(parents=True, exist_ok=True)
                np.save(pathlib.Path(angle_dir) / f"{index:04d}.angle.npy", degrees.cpu().numpy())
    return float(torch.cat(angles).mean())


def triangle_error(mesh, views, size, clear, device="cuda"):
    """Per triangle, the dE76 summed over the pixels it shows in every view."""
    import torch

    render = Renderer(mesh.tris, mesh.double, mesh.vertex_point, size, device, clear, mesh.flat)
    points = torch.as_tensor(mesh.points, dtype=torch.float32, device=device)
    colours = torch.as_tensor(mesh.rgb, dtype=torch.float32, device=device)
    total = torch.zeros(len(mesh.tris), device=device)
    with torch.no_grad():
        for matrix, target, *_rest in views:
            matrix = torch.as_tensor(matrix, dtype=torch.float32, device=device)
            error = delta_e76(render(points, colours, matrix), torch.as_tensor(target, device=device))
            ids = render.visible_ids(points, matrix).flip(0)
            shown = ids >= 0
            total.index_add_(0, ids[shown], error[shown])
    return total.cpu().numpy()


def refine(mesh, error, budget):
    """The mesh with the longest edge of its worst triangles split, both
    sides of the edge at once, until it holds about `budget` triangles: a
    fitted coarse mesh warm-starts a finer one where its error is. A flat
    mesh's halves keep their triangle's colour."""
    from r3d.tessellate import split_marked_edges

    points, rgb, tris, double, _scale, vertex_point, flat = mesh
    positions = points[vertex_point]
    while len(tris) < budget:
        corners = vertex_point[tris]
        lengths = np.stack([np.linalg.norm(points[corners[:, k]] - points[corners[:, (k + 1) % 3]], axis=1) for k in range(3)], axis=1)
        longest = lengths.argmax(axis=1)
        marked_points = set()
        for index in np.argsort(-error)[: max(1, (budget - len(tris)) // 2)]:
            a, b = corners[index, longest[index]], corners[index, (longest[index] + 1) % 3]
            marked_points.add((min(a, b), max(a, b)))
        marked = set()
        for t in tris:
            for k in range(3):
                a, b = t[k], t[(k + 1) % 3]
                pa, pb = vertex_point[a], vertex_point[b]
                if (min(pa, pb), max(pa, pb)) in marked_points:
                    marked.add((min(a, b), max(a, b)))
        positions, tris, midpoint, parent = split_marked_edges(positions, tris, marked)
        if flat:
            colours = rgb[parent]
        else:
            colours = np.concatenate([rgb, np.zeros((len(positions) - len(rgb), 3))])
            for (a, b), m in midpoint.items():
                colours[m] = (rgb[a] + rgb[b]) / 2
        rgb, double, error = colours, np.asarray(double)[parent], error[parent] / 2
        points, vertex_point = np.unique(positions, axis=0, return_inverse=True)
        vertex_point = vertex_point.reshape(-1)
    return mesh._replace(points=points, rgb=rgb, tris=tris, double=double, vertex_point=vertex_point)


def optimise(mesh, views, size, steps, batch, lr_position, lr_colour, laplacian, seed=1, device="cuda", report=100,
             clear=(0.0, 0.0, 0.0), cost=None, cost_weight=0.0, scale=1, normal_weight=0.0):
    """(points, rgb 0..1, per-step mean dE) after `steps` Adam steps of
    `batch` random views; the learning rates decay tenfold over the run and
    position steps are in bounding diagonals. The loss adds `laplacian` times
    the drift of the uniform-Laplacian coordinates (in mean edge lengths),
    `cost_weight` times cost_model's predicted ms at 1 / `scale` of the
    render size, and `normal_weight` times the L1 normal distance."""
    import torch

    points0, rgb0, tris, double, _scale, vertex_point, flat = mesh
    torch.manual_seed(seed)
    rng = np.random.default_rng(seed)
    diagonal = float(np.linalg.norm(points0.max(axis=0) - points0.min(axis=0)))
    renderers = {}

    def renderer_for(index):
        """The renderer at view `index`'s size: a reference of either
        orientation trains the same mesh."""
        height, width = targets[index].shape[:2]
        if (width, height) not in renderers:
            renderers[width, height] = Renderer(tris, double, vertex_point, (width, height), device, clear, flat)
        return renderers[width, height]

    edges = torch.as_tensor(point_edges(tris, vertex_point), device=device)
    start = torch.as_tensor(points0, dtype=torch.float32, device=device)
    points = start.clone().requires_grad_(True)
    colours = torch.as_tensor(rgb0, dtype=torch.float32, device=device).requires_grad_(True)
    rest = uniform_laplacian(start, edges)
    edge_length = float((start[edges[:, 0]] - start[edges[:, 1]]).norm(dim=1).mean())
    matrices = [torch.as_tensor(view[0], dtype=torch.float32, device=device) for view in views]
    targets = [torch.as_tensor(view[1], device=device) for view in views]
    normals = [None if len(view) < 3 or view[2] is None else torch.as_tensor(view[2], device=device) for view in views]
    if normal_weight and any(normal is None for normal in normals):
        raise ValueError("a normal term needs the reference's normal buffers: reference_render.py --normals")
    adam = torch.optim.Adam([{"params": [points], "lr": lr_position * diagonal}, {"params": [colours], "lr": lr_colour}])
    schedule = torch.optim.lr_scheduler.LambdaLR(adam, lambda step: 0.1 ** (step / max(steps, 1)))
    history = []
    started = time.time()
    for step in range(steps):
        chosen = rng.choice(len(views), size=min(batch, len(views)), replace=False)
        adam.zero_grad()
        error, predicted = 0.0, 0.0
        for i in chosen:
            render = renderer_for(i)
            clip = render.clip(points, matrices[i])
            error = error + delta_e76(render(points, colours, matrices[i], clip), targets[i]).mean() / len(chosen)
            if cost is not None:
                predicted = predicted + predicted_ms(torch, cost, clip, render.tris.long(), render.double, render.size,
                                                     scale) / len(chosen)
            if normal_weight:
                distance, _angles = normal_l1(render, points, (matrices[i], None, normals[i], views[i][3]), clip)
                error = error + normal_weight * distance / len(chosen)
        drift = ((uniform_laplacian(points, edges) - rest) / edge_length).pow(2).sum(dim=1).mean()
        (error + laplacian * drift + cost_weight * predicted).backward()
        adam.step()
        schedule.step()
        with torch.no_grad():
            colours.clamp_(0.0, 1.0)
        history.append(float(error))
        if report and (step % report == 0 or step == steps - 1):
            log(f"step {step}: mean dE76 {history[-1]:.3f}, predicted {float(predicted):.2f} ms, drift {float(drift):.3e}, "
                f"{time.time() - started:.1f} s")
    return points.detach().cpu().numpy().astype(np.float64), colours.detach().cpu().numpy().astype(np.float64), history


def write_mesh(out_dir, name, points, rgb, mesh):
    """The fitted mesh as <name>.mesh and <name>.obj, a flat one's OBJ with
    three vertices of each triangle's colour; returns the baked mesh's
    triangle count."""
    from r3d.lit_mesh import write_lit_mesh

    positions = points[mesh.vertex_point]
    colours = np.clip(np.rint(rgb * 255.0), 0, 255)
    out_dir = pathlib.Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    tris = mesh.tris
    if mesh.flat:
        baked = write_lit_mesh(out_dir, name, positions, None, tris, mesh.double, position_scale=mesh.scale,
                               face_rgb=colours)
        positions, rgb, tris = positions[tris].reshape(-1, 3), rgb.repeat(3, axis=0), np.arange(3 * len(tris)).reshape(-1, 3)
    else:
        baked = write_lit_mesh(out_dir, name, positions, colours, tris, mesh.double, position_scale=mesh.scale)
    with open(out_dir / f"{name}.obj", "w", newline="\n") as obj:
        for p, c in zip(positions, rgb):
            obj.write("v %.4f %.4f %.4f %.4f %.4f %.4f\n" % (*p, *c))
        for t in tris:
            obj.write("f %d %d %d\n" % tuple(t + 1))
    return len(baked.tris)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scene", required=True, help="the scene file the references were rendered from")
    parser.add_argument("--start", required=True, help="a smooth or flat NAME.mesh to start from")
    parser.add_argument("--poses", action="append", required=True, help="a track_host.py poses file")
    parser.add_argument("--reference", action="append", required=True, help="reference_render.py output for the poses")
    parser.add_argument("--out", required=True)
    parser.add_argument("--per-shot", action="store_true", help="one mesh per --poses file")
    parser.add_argument("--scale", type=int, default=2, help="render pixels per reference pixel")
    parser.add_argument("--steps", type=int, default=1500)
    parser.add_argument("--batch", type=int, default=4)
    parser.add_argument("--lr-position", type=float, default=2e-4, help="per step, in bounding diagonals")
    parser.add_argument("--lr-colour", type=float, default=0.01)
    parser.add_argument("--laplacian", type=float, default=10.0)
    parser.add_argument("--budget", type=int, help="prune to this many triangles, those that show least first")
    parser.add_argument("--coverage-poses", help="a denser poses file to count shown pixels over; the training poses otherwise")
    parser.add_argument("--cost-model", help="a cost_model.py weights file, such as board_cost_weights.txt")
    parser.add_argument("--cost-weight", type=float, default=0.0, help="dE76 per predicted millisecond")
    parser.add_argument("--normal-weight", type=float, default=0.0, help="weight of the L1 normal term")
    parser.add_argument("--refine-to", type=int, help="first split the start's worst triangles up to this many")
    parser.add_argument("--score", action="store_true", help="fit nothing: print the start's mean normal error on the poses")
    parser.add_argument("--angle-dir", help="with --score, also write each pose's per-pixel normal angle there, for render_compare.py --angle-sheet")
    args = parser.parse_args(argv)
    if len(args.poses) != len(args.reference):
        parser.error("give one --reference per --poses")
    scene = load_scene(args.scene)
    if scene.camera is None:
        parser.error("--scene needs a camera")
    background = scene.camera.component.background
    clear = tuple(((background >> shift) & 255) / 255.0 for shift in (16, 8, 0))
    start = pathlib.Path(args.start)
    if start.suffix != ".mesh":
        parser.error("--start must be a NAME.mesh")
    name = start.stem
    mesh = start_mesh(start)
    log(f"start: {len(mesh.tris)} {'flat' if mesh.flat else 'smooth'} triangles, {len(mesh.points)} positions")
    pairs = list(zip(args.poses, args.reference))
    shots = [[pair] for pair in pairs] if args.per_shot else [pairs]
    for index, shot in enumerate(shots):
        views, size = load_views(shot, args.scale)
        out = pathlib.Path(args.out) / (f"shot{index}" if args.per_shot else "")
        log(f"{out}: {len(views)} views at {size[0]}x{size[1]}")
        if args.score:
            print(f"normal error: {normal_error(mesh, views, size, angle_dir=args.angle_dir):.3f} degrees")
            continue
        fitted = mesh
        if args.refine_to is not None:
            fitted = refine(mesh, triangle_error(mesh, views, size, clear), args.refine_to)
            log(f"refined {len(mesh.tris)} -> {len(fitted.tris)} triangles")
        if args.budget is not None:
            shown = coverage(fitted, pose_views(args.coverage_poses, args.scale) if args.coverage_poses else views, size)
            pruned = prune(fitted, shown, args.budget)
            log(f"pruned {len(fitted.tris) - len(pruned.tris)} triangles: {np.count_nonzero(shown == 0)} never shown")
            fitted = pruned
        cost = None if args.cost_model is None else load_cost(args.cost_model)[0]
        points, rgb, history = optimise(fitted, views, size, args.steps, args.batch, args.lr_position, args.lr_colour,
                                        args.laplacian, clear=clear, cost=cost, cost_weight=args.cost_weight,
                                        scale=args.scale, normal_weight=args.normal_weight)
        count = write_mesh(out, name, points, rgb, fitted)
        np.savetxt(out / "loss.txt", np.array(history), fmt="%.4f")
        print(f"{out}: {count} triangles, mean dE76 {np.mean(history[:20]):.3f} -> {np.mean(history[-20:]):.3f}")
    # The fit's cached device memory goes back for a reference set traced next in this process.
    torch = sys.modules.get("torch")
    if torch is not None and torch.cuda.is_available():
        torch.cuda.empty_cache()
    return 0


if __name__ == "__main__":
    sys.exit(main())
