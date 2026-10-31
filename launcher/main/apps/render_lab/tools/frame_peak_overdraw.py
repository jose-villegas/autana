"""The host renderer's overdraw at each frame profile's replayed poses, its frame peaks and the evenly spaced
path poses, kept as a data file, and the tables frame_profile_tables.py writes from it."""
import concurrent.futures
import os
import pathlib
import re
import statistics
import subprocess
import tempfile

from build.c_header import read_define
from capture import QUARTER_HEADER, QUARTERS
from frame_cost import MS_PER_SECOND, US_PER_MS
from report_performance import path_poses

ROOT = pathlib.Path(__file__).resolve().parents[5]
HOST_SCRIPT = ROOT / "launcher/main/apps/render_lab/tools/render_lab_render_host.sh"
OVERDRAW_HEADER = ROOT / "launcher/main/render/view/raster_overdraw.h"
OVERDRAW_VIEW = "overdraw"
DATA_FILE = "frame-peak-overdraw.tsv"
PATH_POSE = "path"  # the pose column of an evenly spaced pose; a frame peak's holds its rank
POSE_FRAMES = 2  # as the declared renders: the pose's first frame, then the one written
COLUMNS = ("scene", "quarter", "pose", "t_ms")
TRIANGLES = "triangles_set_up"  # the cull count the overdraw is weighed against
PERCENT = 100


def overdraw_fields():
    """RASTER_OVERDRAW_LINE's field names, in its order."""
    text = OVERDRAW_HEADER.read_text(encoding="utf-8")
    line = re.search(r"#define RASTER_OVERDRAW_LINE((?:.*\\\n)*.*)", text)
    if line is None:
        raise ValueError(f"{OVERDRAW_HEADER}: no RASTER_OVERDRAW_LINE")
    return tuple(re.findall(r"(\w+)=%", line[1]))


FIELDS = overdraw_fields()
QUARTER_NUMBER = {name: read_define(QUARTER_HEADER, "DISPLAY_" + name.upper()) for name in QUARTERS}
OVERDRAW_RE = re.compile(r"^overdraw: " + " ".join(rf"{name}=(\d+)" for name in FIELDS) + "$", re.M)


def wanted(text, scenes):
    """{(scene, quarter, pose, t_ms)}: every frame peak and evenly spaced pose the capture replayed."""
    poses = path_poses(text)
    out = set()
    for scene in scenes:
        for quarter in QUARTERS:
            path, ranked = poses[scene, quarter]
            out.update((scene, quarter, PATH_POSE, pose["t_ms"]) for pose in path)
            out.update((scene, quarter, str(frame["rank"]), frame["t_ms"]) for frame, _ in ranked)
    return out


def build():
    result = subprocess.run(["sh", str(HOST_SCRIPT), "--build-only"], cwd=ROOT, capture_output=True, text=True)
    built = re.search(r"^built (.+)$", result.stdout, re.M)
    if result.returncode != 0 or built is None:
        raise ValueError(f"{HOST_SCRIPT.name} --build-only failed:\n{result.stderr}{result.stdout}")
    path = built[1].strip()
    if os.name == "nt" and path.startswith("/"):
        path = subprocess.run(["cygpath", "-w", path], capture_output=True, text=True, check=True).stdout.strip()
    return pathlib.Path(path)


def scene_keys(renderer, any_scene):
    """The render lab's scene keys, as its `render scenes` word lists them; a frame profile row that is not one
    (a take: a scene seen from a set view or with a set blend) is not drawn here."""
    command = [str(renderer), "--scene", any_scene, "--frames", "1", "--console", "0:scenes"]
    with tempfile.TemporaryDirectory() as tmp:
        result = subprocess.run(command + ["-o", str(pathlib.Path(tmp) / "scenes.bmp")], cwd=ROOT,
                                capture_output=True, text=True)
    keys = set(re.findall(r"^RENDER scene=(\S+) ", result.stdout, re.M))
    if result.returncode != 0 or any_scene not in keys:
        raise ValueError(f"{' '.join(command)}: exit {result.returncode}, no scene list\n{result.stderr}")
    return keys


def render(renderer, pose, out):
    """Draws `pose` through the overdraw view into `out`; its totals."""
    scene, quarter, _, t_ms = pose
    command = [str(renderer), "--scene", scene, "--quarter", str(QUARTER_NUMBER[quarter]), "--no-hud",
               "--dt", "0", "--frames", str(POSE_FRAMES), "--console", f"0:pose {t_ms}", "--view", OVERDRAW_VIEW,
               "-o", str(out)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    lines = OVERDRAW_RE.findall(result.stderr)
    if result.returncode != 0 or len(lines) != 1:
        raise ValueError(f"{' '.join(command)}: exit {result.returncode}, {len(lines)} overdraw lines\n{result.stderr}")
    return dict(zip(FIELDS, map(int, lines[0])))


def provenance():
    commit = subprocess.run(["git", "describe", "--always", "--dirty"], cwd=ROOT, capture_output=True, text=True,
                            check=True).stdout.strip()
    return f"# host render at {commit}"


def render_views(text, capture_name, scenes, data_dir):
    """Renders every replayed pose of the rows that are scenes and writes the data file."""
    renderer = build()
    keys = scene_keys(renderer, scenes[0])
    poses = sorted(wanted(text, [scene for scene in scenes if scene in keys]))
    with tempfile.TemporaryDirectory() as tmp, concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        pictures = [pathlib.Path(tmp) / f"{index}.bmp" for index in range(len(poses))]
        totals = list(pool.map(lambda pose, out: render(renderer, pose, out), poses, pictures))
    lines = [provenance() + f" of the poses in {capture_name}", "\t".join(COLUMNS + FIELDS)]
    lines.extend("\t".join(map(str, pose + tuple(counts[name] for name in FIELDS))) for pose, counts in zip(poses, totals))
    (data_dir / DATA_FILE).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def read(data_dir):
    """{(scene, quarter, pose, t_ms): {field: count}} from the data file."""
    path = data_dir / DATA_FILE
    rows = [line.split("\t") for line in path.read_text(encoding="utf-8").splitlines() if not line.startswith("#")]
    if not rows or tuple(rows[0]) != COLUMNS + FIELDS:
        raise ValueError(f"{path}: columns are not {', '.join(COLUMNS + FIELDS)}")
    return {(scene, quarter, pose, int(t_ms)): dict(zip(FIELDS, map(int, counts)))
            for scene, quarter, pose, t_ms, *counts in rows[1:]}


def mean(totals, field):
    return totals[field] / totals["pixels"]


def spearman(ms, values):
    """Spearman's ρ of the two, None when either holds one value throughout."""
    try:
        return statistics.correlation(ms, values, method="ranked")
    except statistics.StatisticsError:
        return None


def signed(rho):
    return "–" if rho is None else f"{rho:+.2f}"


def percentile(value, values):
    """The share of `values` at or below `value`, in percent."""
    return PERCENT * sum(v <= value for v in values) / len(values)


def tables(text, scenes, data_dir):
    """The README's per-peak overdraw and findings for the rows the data file holds, from it; fails when its poses
    are not the capture's."""
    measured = read(data_dir)
    scenes = [scene for scene in scenes if any(pose[0] == scene for pose in measured)]
    missing, extra = wanted(text, scenes) - measured.keys(), measured.keys() - wanted(text, scenes)
    if missing or extra:
        raise ValueError(f"{DATA_FILE}: {len(missing)} poses missing and {len(extra)} not in the capture; "
                         "run with --render-views")
    poses = path_poses(text)
    triangles = TRIANGLES.replace("_", " ")
    lines = ["### Overdraw at the frame peaks", "",
             "Each replayed pose of every row that is a scene drawn by the host renderer through the overdraw view. "
             "Tested counts every span "
             "that reached a pixel, written every one that won its depth test, per pixel drawn.",
             "Path percentile places a peak's tested layers among the evenly spaced poses' (100: none had more).", "",
             "| Scene | Quarter | Rank | Path s | Frame ms | Tested / px | Written / px | Most tested | "
             "Most written | Path percentile |", "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    findings = ["#### Is a frame peak overdraw-heavy?", "",
                f"Over the evenly spaced poses, ρ ranks each pose's replayed ms against its tested layers and its "
                f"{triangles}: whichever is higher follows the frame time more closely.", "",
                "| Scene | Quarter | Path tested / px, median | Peak tested / px, median | Peak path percentile, "
                f"median | ρ ms ~ layers | ρ ms ~ {triangles} | Follows |",
                "|---|---|---:|---:|---:|---:|---:|---|"]
    for scene in scenes:
        for quarter in QUARTERS:
            path, ranked = poses[scene, quarter]
            along = [measured[scene, quarter, PATH_POSE, pose["t_ms"]] for pose in path]
            layers = [mean(totals, "tested") for totals in along]
            peaks = []
            for frame, _ in ranked:
                totals = measured[scene, quarter, str(frame["rank"]), frame["t_ms"]]
                peaks.append(mean(totals, "tested"))
                lines.append(f"| {scene} | {quarter} | {frame['rank']} | {frame['t_ms'] / MS_PER_SECOND:.2f} | "
                             f"{frame['frame_us'] / US_PER_MS:.1f} | {peaks[-1]:.2f} | "
                             f"{mean(totals, 'written'):.2f} | {totals['tested_max']} | {totals['written_max']} | "
                             f"{percentile(peaks[-1], layers):.0f} |")
            ms = [sum(pose["stages"].values()) for pose in path]
            by_layers = spearman(ms, [t["tested"] for t in along])
            by_triangles = spearman(ms, [pose["counts"][TRIANGLES] for pose in path])
            follows = ("–" if by_layers is None or by_triangles is None
                       else "layers" if by_layers > by_triangles else triangles)
            findings.append(f"| {scene} | {quarter} | {statistics.median(layers):.2f} | "
                            f"{statistics.median(peaks):.2f} | "
                            f"{statistics.median(percentile(p, layers) for p in peaks):.0f} | {signed(by_layers)} | "
                            f"{signed(by_triangles)} | {follows} |")
    return "\n".join(lines + [""] + findings) + "\n"
