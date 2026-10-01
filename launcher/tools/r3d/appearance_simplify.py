#!/usr/bin/env python3
"""Fit a baked smooth mesh's vertex positions and colours to reference
renders along a camera path, keeping its triangles (appearance-driven
simplification, Hasselgren et al. 2021).

    python launcher/tools/r3d/appearance_simplify.py --start MESH_mesh_generated.c \\
        --poses POSES.txt --reference DIR [--poses ... --reference ...] --out DIR [--per-shot]

--start is a generated smooth mesh, usually the simplifier's output at the
triangle budget. Each --poses file (tools/anim/sample_tracks.sh) pairs with
the --reference directory reference_render.py wrote for it. All pairs train
one mesh; with --per-shot each pair trains its own. A mesh is written as
<name>_mesh_generated.{c,h} by the same writer as mesh_import.py, under
DIR, or DIR/shot<k>, and as a vertex-coloured OBJ beside it.

The optimiser needs PyTorch with CUDA and nvdiffrast; see the README.
"""

import argparse
import pathlib
import sys
import time

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import log  # noqa: E402
from r3d.lit_mesh import finest_triangles, read_lit_mesh  # noqa: E402
from r3d.poses import camera_basis, read_poses  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))

from render_compare import D65_WHITE, GAMMA, SRGB_TO_XYZ  # noqa: E402

FAR = 1.0e5


def start_mesh(path):
    """(positions in model units, rgb 0..1, tris, double, position_scale,
    vertex_point): a generated smooth mesh welded back to one vertex per
    position and colour. vertex_point maps each vertex to its position, so a
    colour seam cannot open into a crack when positions move."""
    mesh = read_lit_mesh(path)
    if mesh.rgb is None:
        raise ValueError("the start mesh must be a smooth (vertex-coloured) bake")
    q, rgb, tris, double, _ = finest_triangles(mesh)
    points, vertex_point = np.unique(q, axis=0, return_inverse=True)
    return (points / float(mesh.position_scale), rgb / 255.0, tris, double, mesh.position_scale,
            vertex_point.reshape(-1))


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


def load_views(pairs, scale):
    """[(clip matrix, target sRGB image 0..1 at render size)] for every pose
    of every (poses file, reference directory) pair, and the render size."""
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
            views.append((projection(pose[:3], pose[3:], width, height, lens, near), target))
    return views, size


def lab(srgb):
    """CIELAB of 0..1 gamma-encoded colours: render_compare.py's dE space, in torch."""
    import torch

    linear = srgb.clamp(1e-6, 1.0) ** GAMMA
    xyz = linear @ torch.tensor(SRGB_TO_XYZ, device=srgb.device).T / torch.tensor(D65_WHITE, device=srgb.device)
    delta = 6.0 / 29.0
    f = torch.where(xyz > delta**3, xyz.clamp_min(delta**3) ** (1.0 / 3.0), xyz / (3 * delta**2) + 4.0 / 29.0)
    return torch.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], dim=-1)


def delta_e76(a, b):
    """Per-pixel CIE76 dE; the epsilon keeps the gradient finite at zero."""
    return ((lab(a) - lab(b)) ** 2).sum(dim=-1).add(1e-6).sqrt()


class Renderer:
    """Draws the mesh as the device does: Gouraud vertex colours, single-sided
    triangles culled when they face away, `clear` (0..1 RGB) where nothing is
    drawn."""

    def __init__(self, tris, double, vertex_point, size, device, clear=(0.0, 0.0, 0.0)):
        import nvdiffrast.torch as dr
        import torch

        self.dr = dr
        self.context = dr.RasterizeCudaContext(device=device)
        self.tris = torch.as_tensor(tris, dtype=torch.int32, device=device)
        self.double = torch.as_tensor(np.asarray(double, dtype=bool), device=device)
        self.vertex_point = torch.as_tensor(vertex_point, dtype=torch.int64, device=device)
        self.size = size
        self.clear = torch.as_tensor(clear, dtype=torch.float32, device=device)

    def __call__(self, points, colours, clip_matrix):
        import torch

        positions = points[self.vertex_point]
        homogeneous = torch.cat([positions, torch.ones_like(positions[:, :1])], dim=1)
        clip = homogeneous @ clip_matrix.T
        with torch.no_grad():
            corners = clip[self.tris.long()]
            w = corners[..., 3]
            xy = corners[..., :2] / w.clamp_min(1e-6)[..., None]
            area = (xy[:, 1, 0] - xy[:, 0, 0]) * (xy[:, 2, 1] - xy[:, 0, 1]) - (xy[:, 2, 0] - xy[:, 0, 0]) * (xy[:, 1, 1] - xy[:, 0, 1])
            keep = self.double | (area > 0) | (w <= 0).any(dim=1)
        tris = self.tris[keep].contiguous()
        clip = clip[None].contiguous()
        width, height = self.size
        rast, _ = self.dr.rasterize(self.context, clip, tris, resolution=[height, width])
        colour, _ = self.dr.interpolate(colours[None].contiguous(), rast, tris)
        colour = torch.where(rast[..., 3:] > 0, colour, self.clear.expand_as(colour))
        colour = self.dr.antialias(colour.contiguous(), rast, clip, tris)
        return colour[0].flip(0)


def uniform_laplacian(points, edges):
    """Each position minus the mean of its neighbours."""
    import torch

    total = torch.zeros_like(points)
    count = torch.zeros(len(points), 1, device=points.device)
    for a, b in ((0, 1), (1, 0)):
        total.index_add_(0, edges[:, a], points[edges[:, b]])
        count.index_add_(0, edges[:, a], torch.ones(len(edges), 1, device=points.device))
    return points - total / count.clamp_min(1.0)


def optimise(mesh, views, size, steps, batch, lr_position, lr_colour, laplacian, seed=1, device="cuda", report=100,
             clear=(0.0, 0.0, 0.0)):
    """(points, rgb 0..1, per-step mean dE): the start mesh's welded positions
    and vertex colours after `steps` Adam steps of `batch` random views. Both
    learning rates decay tenfold over the run; `laplacian` weights how far the
    positions' differential coordinates may drift from the start's, in units
    of the mean edge length; position steps are in bounding diagonals.
    `clear` is the colour the scene clears to."""
    import torch

    points0, rgb0, tris, double, _scale, vertex_point = mesh
    torch.manual_seed(seed)
    rng = np.random.default_rng(seed)
    diagonal = float(np.linalg.norm(points0.max(axis=0) - points0.min(axis=0)))
    render = Renderer(tris, double, vertex_point, size, device, clear)
    edges = torch.as_tensor(point_edges(tris, vertex_point), device=device)
    start = torch.as_tensor(points0, dtype=torch.float32, device=device)
    points = start.clone().requires_grad_(True)
    colours = torch.as_tensor(rgb0, dtype=torch.float32, device=device).requires_grad_(True)
    rest = uniform_laplacian(start, edges)
    edge_length = float((start[edges[:, 0]] - start[edges[:, 1]]).norm(dim=1).mean())
    matrices = [torch.as_tensor(m, dtype=torch.float32, device=device) for m, _ in views]
    targets = [torch.as_tensor(t, device=device) for _, t in views]
    adam = torch.optim.Adam([{"params": [points], "lr": lr_position * diagonal}, {"params": [colours], "lr": lr_colour}])
    schedule = torch.optim.lr_scheduler.LambdaLR(adam, lambda step: 0.1 ** (step / max(steps, 1)))
    history = []
    started = time.time()
    for step in range(steps):
        chosen = rng.choice(len(views), size=min(batch, len(views)), replace=False)
        adam.zero_grad()
        error = sum(delta_e76(render(points, colours, matrices[i]), targets[i]).mean() for i in chosen) / len(chosen)
        drift = ((uniform_laplacian(points, edges) - rest) / edge_length).pow(2).sum(dim=1).mean()
        (error + laplacian * drift).backward()
        adam.step()
        schedule.step()
        with torch.no_grad():
            colours.clamp_(0.0, 1.0)
        history.append(float(error))
        if report and (step % report == 0 or step == steps - 1):
            log(f"step {step}: mean dE76 {history[-1]:.3f}, drift {float(drift):.3e}, {time.time() - started:.1f} s")
    return points.detach().cpu().numpy().astype(np.float64), colours.detach().cpu().numpy().astype(np.float64), history


def write_mesh(out_dir, name, points, rgb, mesh, banner):
    """The fitted mesh as <name>_mesh_generated.{c,h} and <name>.obj; returns
    the baked mesh's triangle count."""
    from r3d.lit_mesh import write_lit_mesh

    _p0, _c0, tris, double, scale, vertex_point = mesh
    positions = points[vertex_point]
    colours = np.clip(np.rint(rgb * 255.0), 0, 255)
    out_dir = pathlib.Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    baked = write_lit_mesh(out_dir, name, positions, colours, tris, double, banner, position_scale=scale)
    with open(out_dir / f"{name}.obj", "w", newline="\n") as obj:
        for p, c in zip(positions, rgb):
            obj.write("v %.4f %.4f %.4f %.4f %.4f %.4f\n" % (*p, *c))
        for t in tris:
            obj.write("f %d %d %d\n" % tuple(t + 1))
    return len(baked.tris)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--start", required=True, help="a smooth <name>_mesh_generated.c to start from")
    parser.add_argument("--poses", action="append", required=True, help="a sample_tracks.sh poses file")
    parser.add_argument("--reference", action="append", required=True, help="reference_render.py output for the poses")
    parser.add_argument("--out", required=True)
    parser.add_argument("--per-shot", action="store_true", help="one mesh per --poses file")
    parser.add_argument("--scale", type=int, default=2, help="render pixels per reference pixel")
    parser.add_argument("--steps", type=int, default=1500)
    parser.add_argument("--batch", type=int, default=4)
    parser.add_argument("--lr-position", type=float, default=2e-4, help="per step, in bounding diagonals")
    parser.add_argument("--lr-colour", type=float, default=0.01)
    parser.add_argument("--laplacian", type=float, default=10.0)
    parser.add_argument("--clear", default="000000", help="RRGGBB the scene clears to, as reference_render.py was given")
    args = parser.parse_args(argv)
    if len(args.poses) != len(args.reference):
        parser.error("give one --reference per --poses")
    if len(args.clear) != 6:
        parser.error("--clear must be RRGGBB")
    clear = tuple(int(args.clear[index : index + 2], 16) / 255.0 for index in (0, 2, 4))
    start = pathlib.Path(args.start)
    if not start.name.endswith("_mesh_generated.c"):
        parser.error("--start must be a <name>_mesh_generated.c")
    name = start.name[: -len("_mesh_generated.c")]
    mesh = start_mesh(start)
    log(f"start: {len(mesh[2])} triangles, {len(mesh[0])} positions")
    pairs = list(zip(args.poses, args.reference))
    shots = [[pair] for pair in pairs] if args.per_shot else [pairs]
    for index, shot in enumerate(shots):
        views, size = load_views(shot, args.scale)
        out = pathlib.Path(args.out) / (f"shot{index}" if args.per_shot else "")
        log(f"{out}: {len(views)} views at {size[0]}x{size[1]}")
        points, rgb, history = optimise(mesh, views, size, args.steps, args.batch, args.lr_position, args.lr_colour, args.laplacian,
                                        clear=clear)
        command = " ".join(["python launcher/tools/r3d/appearance_simplify.py", *(argv if argv is not None else sys.argv[1:])])
        banner = ["GENERATED FILE - do not edit.", "", f"    {command}", "", f"Fitted from {start.name}."]
        count = write_mesh(out, name, points, rgb, mesh, banner)
        np.savetxt(out / "loss.txt", np.array(history), fmt="%.4f")
        print(f"{out}: {count} triangles, mean dE76 {np.mean(history[:20]):.3f} -> {np.mean(history[-20:]):.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
