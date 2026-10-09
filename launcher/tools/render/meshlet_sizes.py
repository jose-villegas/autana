"""Generate meshlet-size costs from board captures and scratch bakes."""
import argparse
import statistics
import zlib
import pathlib
import re
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from r3d.cost_model import FEATURES, mesh_rows
from r3d.dynres_report import frame_cost_means, frame_cost_windows
from r3d.import_settings import load_scene
from r3d.lit_mesh import read_lit_mesh
from r3d.poses import sample_camera_path
from r3d.fitted_variant import poses_text
from generated_blocks import replace_block

ROWS = ("cull-off", "16", "32", "64")
STAGES = ("r3d.cull", "r3d.transform", "r3d.draw")
IDENTITY = re.compile(r"MESHLET_CAPTURE build_id=([0-9a-f]{12}-diag) pack_crc32=([0-9a-f]{8}) size=(\d+) cull=([01]) period_ms=(\d+) pose_every_ms=(\d+)")
BLOCK = re.compile(r"=== (\w+) FRAME COST .*?===.*?(?:\1 both cores: mean [^\n]+)", re.S)


def capture_means(directory, object_name, expected_packs=None, expected_poses=None):
    result, packs, poses = {}, {}, None
    for row in ROWS:
        path = pathlib.Path(directory) / f"meshlets-{row}-board.log"
        text = path.read_text(encoding="utf-8")
        if re.search(r"\bFAIL\b", text):
            raise ValueError(f"{path}: failed suite")
        label = "cull_off" if row == "cull-off" else object_name
        blocks = [match[0] for match in BLOCK.finditer(text) if match[1] == label]
        if len(blocks) != 1:
            raise ValueError(f"{path}: needs one suite FRAME COST block for {label}")
        block = blocks[0]
        identities = IDENTITY.findall(block)
        if len(identities) != 1:
            raise ValueError(f"{path}: needs one suite capture build identity")
        build, pack, size, cull, period, every = identities[0]
        if (int(size), int(cull)) != (32 if row == "cull-off" else int(row), int(row != "cull-off")):
            raise ValueError(f"{path}: wrong size or cull setting")
        reported = re.findall(r"BUILD_ID[= ]([0-9a-f]{12}-\w+)", text)
        if any(value != build for value in reported) or any(identity[0] != build for identity in IDENTITY.findall(text)):
            raise ValueError(f"{path}: mismatched build id")
        if expected_packs is not None and pack != expected_packs[str(size)]:
            raise ValueError(f"{path}: pack hash differs from the row bake")
        every, period = int(every), int(period)
        if every <= 0 or period <= 0 or every % 1000:
            raise ValueError(f"{path}: invalid suite pose interval")
        frames = re.findall(rf"{re.escape(label)} t=\s*(\d+)s .*?both cores: frame\s+(\d+)us", block)
        times = [int(time) * 1000 for time, _ in frames]
        if times != list(range(0, period, every)) or (poses is not None and times != poses):
            raise ValueError(f"{path}: needs the shared suite poses exactly once")
        if expected_poses is not None and times != list(expected_poses):
            raise ValueError(f"{path}: suite poses differ from the host camera samples")
        poses = times
        packs[row] = pack
        means = dict(frame_cost_means(path, text=block))
        if (any(not set(STAGES).issubset(dict(window)) for window in frame_cost_windows(path, block))
                or block.count("ms/frame avg/worst:") != len(frames)):
            raise ValueError(f"{path}: missing per-pose frame stages")
        result[row] = [*(means[stage] for stage in STAGES), statistics.mean(int(us) for _, us in frames) / 1000]
    if packs["cull-off"] != packs["32"] or len({packs[row] for row in ("16", "32", "64")}) != 3:
        raise ValueError("captures need distinct size pack hashes, cull-off must use the shipped pack")
    return result


def host_columns(paths, poses, cull=True):
    meshes = [read_lit_mesh(path) for path in paths]
    clusters = sum(len(mesh.clusters) for mesh in meshes)
    vertices = sum(len(mesh.pos) for mesh in meshes)
    triangles = sum(len(mesh.tris) for mesh in meshes)
    if not triangles:
        raise ValueError("host columns need triangles")
    submitted = sum(mesh_rows(path, poses, cull=cull)[:, FEATURES.index("submitted")] for path in paths)
    return clusters, vertices / triangles, float(submitted.mean())


def default_size():
    def define(path, name):
        return int(re.search(rf"^#define {name}\s+(\d+)", path.read_text(), re.M)[1])
    scale = define(ROOT / "launcher/main/render/context/render_context.h", "RENDER_CONTEXT_DEFAULT_SCALE_PERCENT")
    header = ROOT / "launcher/main/gfx/gfx.h"
    return tuple(define(header, name) * scale // 100 for name in ("GFX_WIDTH", "GFX_HEIGHT"))


def table(scene_path, captures, bakes, object_name=None):
    from meshlet_capture import rebake_scene
    scene = load_scene(scene_path)
    camera = scene.camera.component
    jobs = [job for job in scene.renderers if job.object.name == object_name] if object_name else scene.renderers[:1]
    if not jobs:
        raise ValueError("scene has no selected mesh renderer")
    if any(not job.object.identity for job in jobs):
        raise ValueError("meshlet table requires unplaced mesh geometry")
    width, height = default_size()
    from doc_stages import capture_viewport, markdown
    for row in ROWS:
        capture_viewport(captures / f"meshlets-{row}-board.log", (width, height))
    poses = sample_camera_path(camera.path.animation, camera.path.node, 5000, width, height,
                               camera.half_fov_short_tan, camera.near_z)
    from r3d.build_pack import pack_bytes
    from r3d.scene_asset import scene_id
    packs = {}
    with tempfile.TemporaryDirectory() as directory:
        work = pathlib.Path(directory)
        poses_path = work / "poses.txt"
        poses_path.write_text(poses_text(*poses))
        host = {}
        for size in (16, 32, 64):
            paths = rebake_scene(scene_path, bakes / str(size), size)
            replacements = [f"{job.asset_name}={path}" for job, path in zip(scene.renderers, paths)]
            pack = pack_bytes([scene_path], replacements)[scene_id(scene_path)]
            packs[str(size)] = f"{zlib.crc32(pack):08x}"
            paths = [path for path in paths if path.name in {job.asset_path.name for job in jobs}]
            host[str(size)] = host_columns(paths, poses_path)
            if size == 32:
                host["cull-off"] = host_columns(paths, poses_path, cull=False)
    means = capture_means(captures, jobs[0].object.name, packs, range(0, len(poses[-1]) * 5000, 5000))
    rows = []
    for row in ROWS:
        clusters, vertices, submitted = host[row]
        rows.append(["cull off" if row == "cull-off" else f"meshlets of {row}" + (" (shipped)" if row == "32" else ""),
                     str(clusters), f"{vertices:.3f}", f"{submitted:.1f}", *(f"{value:.2f}" for value in means[row])])
    return markdown(["Row", "Clusters", "Vertices/triangle", "Mean submitted triangles", "Cull ms", "Transform ms",
                     "Draw ms", "Frame ms"], rows)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", type=pathlib.Path, required=True)
    parser.add_argument("--object", help="renderer enabled in the capture; first renderer when omitted")
    parser.add_argument("--captures", type=pathlib.Path, default=ROOT / "docs/render/data")
    parser.add_argument("--bakes", type=pathlib.Path, default=ROOT / "launcher/tools/results/meshlet-sizes/bakes")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        body = table(args.scene, args.captures, args.bakes, args.object)
        changed = replace_block(ROOT / "docs/render/Render-Pipeline.md", "meshlet-sizes", body, args.check)
        print(f"{'changed' if changed else 'same'} docs/render/Render-Pipeline.md#meshlet-sizes")
        return int(changed and args.check)
    except (ValueError, OSError) as error:
        print(error, file=sys.stderr)
        return 2

if __name__ == "__main__":
    sys.exit(main())
