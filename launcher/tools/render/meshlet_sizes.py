"""Generate meshlet-size costs from board captures and scratch bakes."""
import statistics
import zlib
import pathlib
import re
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from r3d.cost_model import FEATURES, mesh_rows
sys.path.insert(0, str(ROOT / "launcher/tools/perf"))
import frame_cost
from capture import build_ids, capture_board, quarter_row, require_same_board
from r3d.import_settings import load_scene
from r3d.lit_mesh import read_lit_mesh
from r3d.poses import sample_camera_path
from r3d.fitted_variant import poses_text

ROWS = ("16", "32", "64")  # the cluster sizes a capture measures; the scene's shipped size is one of them
STAGES = ("r3d.cull", "r3d.transform", "r3d.draw")  # magic: the frame-cost stages the table shows, as raster.c names them
REST = "rest of frame"  # the frame's time outside STAGES
CHART_WIDTH = 8  # inches
CHART_ROW_HEIGHT = 0.45  # inches per bar
CHART_ROOM = 1.1  # inches for the title, axis and legend
TRADE_CHART = "meshlet-size-trade.png"  # written beside the frame-cost chart
TRADE_HEIGHT = 3.2  # inches
TRADE_X_ROOM = 1.4  # factor of room before the smallest size; its square past the largest, for the labels
TRADE_Y_ROOM = 1.15  # factor of room above the highest point
TRADE_LABEL_GAP = 8  # points between a point and its label
IDENTITY = re.compile(r"\bCAPTURE build_id=([0-9a-f]{12}-diag) pack_crc32=([0-9a-f]{8}) object=(\w+) size=(\d+) cull=([01]) period_ms=(\d+) pose_every_ms=(\d+)")
BLOCK = re.compile(r"=== (\w+) FRAME COST .*?===.*?(?:\1 both cores: mean [^\n]+)", re.S)


def capture_means(directory, object_name, shipped, expected_packs=None, expected_poses=None, expected_geometry=None):
    """{row: means} of each size's capture; the culling-off row comes from the `shipped` size's."""
    paths = [pathlib.Path(directory) / f"meshlets-{row}-board.log" for row in ROWS]
    require_same_board({str(path): capture_board(path) for path in paths})
    from doc_stages import capture_pose_every_ms
    every_ms = capture_pose_every_ms(paths)
    result, packs, poses = {}, {}, None
    for row in ROWS:
        path = pathlib.Path(directory) / f"meshlets-{row}-board.log"
        from capture import select_quarter
        text = select_quarter(path.read_text(encoding="utf-8"))
        if re.search(r"\bFAIL\b", text):
            raise ValueError(f"{path}: failed suite")
        if (not re.search(r":test_\w*frame_cost\w*:PASS\b", text)
                or not re.search(r"RUNSUITE_COMPLETE name=\w+ found=1 selected=[1-9]\d* unmatched=0", text)):
            raise ValueError(f"{path}: needs successful suite PASS and complete verdict")
        measured = text[:re.search(r":test_\w*frame_cost\w*:PASS\b", text).start()]
        blocks_by_label = {}
        for match in BLOCK.finditer(measured):
            blocks_by_label.setdefault(match[1], []).append(match[0])
        selected = [(row, object_name)]
        if row == shipped:
            selected.insert(0, ("cull-off", "cull_off"))  # magic: the suite's label for its culling-off pass
        for key, label in selected:
            blocks = blocks_by_label.get(label, [])
            if len(blocks) != 1:
                raise ValueError(f"{path}: needs one suite FRAME COST block for {label}")
            block = blocks[0]
            identities = IDENTITY.findall(block)
            if len(identities) != 1:
                raise ValueError(f"{path}: needs one suite capture build identity")
            build, pack, measured_object, size, cull, period, every = identities[0]
            if (int(size), int(cull)) != (int(row), int(key != "cull-off")):
                raise ValueError(f"{path}: wrong size or cull setting")
            if measured_object != object_name:
                raise ValueError(f"{path}: cull pass measured another object")
            if expected_geometry is not None:
                geometry = tuple(map(int, re.search(r"FRAME COST \((\d+) tris, (\d+) verts, (\d+) clusters", block).groups()))
                if geometry != expected_geometry[row]:
                    raise ValueError(f"{path}: header geometry differs from selected bake")
            reported = build_ids(text)
            if any(value != build for value in reported) or any(identity[0] != build for identity in IDENTITY.findall(text)):
                raise ValueError(f"{path}: mismatched build id")
            if expected_packs is not None and pack != expected_packs[row]:
                raise ValueError(f"{path}: pack hash differs from the row bake")
            every, period = int(every), int(period)
            if every != every_ms or period <= 0:
                raise ValueError(f"{path}: invalid suite pose interval")
            frames = re.findall(rf"{re.escape(label)} t=\s*(\d+)(ms|s) .*?both cores: frame\s+(\d+)us", block)
            times = list(range(0, period, every))
            if ([int(time) * (1000 if unit == "s" else 1) for time, unit, _ in frames] != times
                    or (poses is not None and times != poses)):
                raise ValueError(f"{path}: needs the shared suite poses exactly once")
            if expected_poses is not None and times != list(expected_poses):
                raise ValueError(f"{path}: suite poses differ from the host camera samples")
            poses = times
            packs[row] = pack
            stages = [quarter_row(stage, block) for stage in STAGES]
            means = dict(frame_cost.frame_cost_means(path, text=block))
            if (any(not set(stages).issubset(dict(window)) for window in frame_cost.frame_cost_windows(path, block))
                    or block.count("ms/frame avg/worst:") != len(frames)):
                raise ValueError(f"{path}: missing per-pose frame stages")
            result[key] = [*(means[stage] for stage in stages), statistics.mean(int(us) for _, _, us in frames) / 1000]
    if len(set(packs.values())) != len(ROWS):
        raise ValueError("captures need distinct size pack hashes")
    return {key: result[key] for key in ("cull-off", *ROWS)}


def host_columns(paths, poses, cull=True):
    meshes = [read_lit_mesh(path) for path in paths]
    clusters = sum(len(mesh.clusters) for mesh in meshes)
    vertices = sum(len(mesh.pos) for mesh in meshes)
    triangles = sum(len(mesh.tris) for mesh in meshes)
    if not triangles:
        raise ValueError("host columns need triangles")
    submitted = sum(mesh_rows(path, poses, cull=cull)[:, FEATURES.index("submitted")] for path in paths)
    return (triangles, vertices, clusters), float(submitted.mean())


def default_size():
    def define(path, name):
        return int(re.search(rf"^#define {name}\s+(\d+)", path.read_text(), re.M)[1])
    scale = define(ROOT / "launcher/main/render/context/render_context.h", "RENDER_CONTEXT_DEFAULT_SCALE_PERCENT")
    header = ROOT / "launcher/main/gfx/gfx.h"
    return tuple(define(header, name) * scale // 100 for name in ("GFX_WIDTH", "GFX_HEIGHT"))


def chart(rows, path):
    """Each row's frame as stacked board stages, its total and submitted triangles at the bar's end."""
    from docs.plot_style import LABEL_SIZE, MUTED, SLOTS, save, stacked_barh, style_axes, subplots
    figure, axis = subplots(1, 1, (CHART_WIDTH, CHART_ROW_HEIGHT * len(rows) + CHART_ROOM))
    stacked_barh(axis, [(label, list(zip((*STAGES, REST), (*stages, frame - sum(stages)))))
                        for label, _clusters, _ratio, _submitted, *stages, frame in rows],
                 dict(zip((*STAGES, REST), SLOTS)),
                 [f"{frame:.1f} ms, {submitted:,.0f} triangles" for _, _, _, submitted, *_, frame in rows])
    axis.set_xlabel("board ms per frame", fontsize=LABEL_SIZE, color=MUTED)
    style_axes(axis, "Frame cost by meshlet size")
    figure.legend(loc="outside lower center", ncol=len(STAGES) + 1, frameon=False, fontsize=LABEL_SIZE)
    save(figure, path)


def trade_chart(rows, path):
    """The size trade: clusters to walk and triangles submitted against the meshlet size, each point labelled
    with the board stage it costs. `rows` are the sized rows of `table`, smallest first."""
    from docs.plot_style import LABEL_SIZE, MUTED, SLOTS, save, style_axes, subplots
    sizes = [int(row) for row in ROWS]
    figure, axes = subplots(1, 2, (CHART_WIDTH, TRADE_HEIGHT))
    panels = (("clusters", [clusters for _, clusters, *_ in rows], "cull", STAGES.index("r3d.cull"),
               "Smaller meshlets: more clusters to walk"),
              ("mean triangles submitted", [submitted for _, _, _, submitted, *_ in rows], "draw",
               STAGES.index("r3d.draw"), "Bigger meshlets: looser boxes, more submitted"))
    for axis, (label, values, stage, at, title), colour in zip(axes, panels, SLOTS):
        axis.plot(sizes, values, "o-", color=colour)
        axis.set_xscale("log", base=2)
        axis.set_xticks(sizes, [str(size) for size in sizes])
        axis.set_xlim(sizes[0] / TRADE_X_ROOM, sizes[-1] * TRADE_X_ROOM ** 2)
        axis.set_ylim(0, max(values) * TRADE_Y_ROOM)
        axis.set_xlabel("geometry.meshlet_triangles", fontsize=LABEL_SIZE, color=MUTED)
        axis.set_ylabel(label, fontsize=LABEL_SIZE, color=MUTED)
        # A rising line's labels sit under their points, a falling line's to their right: both keep clear of it.
        rising = values[-1] > values[0]
        offset, align = ((0, -TRADE_LABEL_GAP), dict(ha="center", va="top")) if rising else             ((TRADE_LABEL_GAP, 0), dict(ha="left", va="center"))
        for size, value, row in zip(sizes, values, rows):
            axis.annotate(f"{value:,.0f}\n{stage} {row[4 + at]:.2f} ms", (size, value), textcoords="offset points",
                          xytext=offset, fontsize=LABEL_SIZE, **align)
        style_axes(axis, title)
    save(figure, path)


def table(scene_path, captures, bakes, chart_path):
    """The block of the scene's showcase: the chart, written to `chart_path`, then its rows collapsed."""
    from meshlet_capture import rebake_scene
    from showcase import showcase
    scene = load_scene(scene_path)
    camera = scene.camera.component
    jobs = [job for job in scene.renderers if job.object.name == showcase(scene)]
    if any(not job.object.identity for job in jobs):
        raise ValueError("meshlet table requires unplaced mesh geometry")
    width, height = default_size()
    from doc_stages import capture_viewport, markdown
    for row in ROWS:
        capture_viewport(captures / f"meshlets-{row}-board.log", (width, height))
    from doc_stages import capture_pose_every_ms
    every_ms = capture_pose_every_ms([captures / f"meshlets-{row}-board.log" for row in ROWS])
    poses = sample_camera_path(camera.path.animation, camera.path.node, every_ms, width, height,
                               camera.half_fov_short_tan, camera.near_z)
    from r3d.build_pack import pack_bytes
    from r3d.scene_name import scene_id
    packs = {}
    with tempfile.TemporaryDirectory() as directory:
        work = pathlib.Path(directory)
        poses_path = work / "poses.txt"
        poses_path.write_text(poses_text(*poses))
        host, geometry = {}, {}
        shipped = str(jobs[0].settings.meshlet_triangles)
        for size in map(int, ROWS):
            paths = rebake_scene(scene_path, bakes / str(size), size)
            replacements = [f"{job.asset_name}={path}" for job, path in zip(scene.renderers, paths)]
            pack = pack_bytes([scene_path], replacements)[scene_id(scene_path)]
            packs[str(size)] = f"{zlib.crc32(pack):08x}"
            paths = [path for path in paths if path.name in {job.asset_path.name for job in jobs}]
            host[str(size)] = host_columns(paths, poses_path)
            geometry[str(size)] = host[str(size)][0]
            if str(size) == shipped:
                host["cull-off"] = host_columns(paths, poses_path, cull=False)
    means = capture_means(captures, jobs[0].object.name, shipped, packs, range(0, len(poses[-1]) * every_ms, every_ms), geometry)
    rows = []
    for row in ("cull-off", *ROWS):
        (triangles, vertices, clusters), submitted = host[row]
        rows.append(("cull off" if row == "cull-off" else f"meshlets of {row}" + (" (shipped)" if row == shipped else ""),
                     clusters, vertices / triangles, submitted, *means[row]))
    chart(rows, chart_path)
    trade_chart(rows[1:], pathlib.Path(chart_path).with_name(TRADE_CHART))
    from render.doc_tables import collapsed
    body = markdown(["Row", "Clusters", "Vertices/triangle", "Mean submitted triangles", "Cull ms", "Transform ms",
                     "Draw ms", "Frame ms"],
                    [(label, clusters, f"{ratio:.3f}", f"{submitted:.1f}", *(f"{value:.2f}" for value in measured))
                     for label, clusters, ratio, submitted, *measured in rows])
    return (f"![Frame cost by meshlet size](../images/render/meshlets/{pathlib.Path(chart_path).name})\n\n"
            + collapsed("Meshlet sizes on the board", body))
