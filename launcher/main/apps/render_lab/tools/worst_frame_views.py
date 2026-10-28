"""Each frame profile's poses drawn by the host renderer: the worst frames shaded and as overdraw, and the
overdraw at the evenly spaced path poses, kept as a data file and one picture per scene."""
import concurrent.futures
import os
import pathlib
import re
import statistics
import subprocess
import tempfile

from PIL import Image

from build.c_header import read_define
from capture import QUARTER_HEADER, QUARTERS
from docs.plot_style import LABEL_SIZE, save, subplots
from frame_cost import MS_PER_SECOND, US_PER_MS
from report_performance import path_poses

ROOT = pathlib.Path(__file__).resolve().parents[5]
HOST_SCRIPT = ROOT / "launcher/main/apps/render_lab/tools/render_lab_render_host.sh"
OVERDRAW_HEADER = ROOT / "launcher/main/render/view/raster_overdraw.h"
OVERDRAW_VIEW = "overdraw"
DATA_FILE = "worst-frame-overdraw.tsv"
PATH_POSE = "path"  # the pose column of an evenly spaced pose; a worst frame's holds its rank
POSE_FRAMES = 2  # as the declared renders: the pose's first frame, then the one written
COLUMNS = ("scene", "quarter", "pose", "t_ms")
CELL_INCHES = 2.4  # a picture's width in the per-scene sheet
SHEET_COLORS = 128  # the sheet's palette: the overdraw ramp's few colours and the shaded poses'
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
RAMP_LAYERS = read_define(OVERDRAW_HEADER, "RASTER_OVERDRAW_RAMP_LAYERS")
QUARTER_NUMBER = {name: read_define(QUARTER_HEADER, "DISPLAY_" + name.upper()) for name in QUARTERS}
OVERDRAW_RE = re.compile(r"^overdraw: " + " ".join(rf"{name}=(\d+)" for name in FIELDS) + "$", re.M)


def wanted(text, scenes):
    """{(scene, quarter, pose, t_ms)}: every worst frame and evenly spaced pose the capture replayed."""
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


def render(renderer, pose, view, out):
    """Draws `pose` into `out`; the overdraw view's totals, else None."""
    scene, quarter, _, t_ms = pose
    command = [str(renderer), "--scene", scene, "--quarter", str(QUARTER_NUMBER[quarter]), "--no-hud",
               "--dt", "0", "--frames", str(POSE_FRAMES), "--console", f"0:path {t_ms}", "-o", str(out)]
    if view is not None:
        command += ["--view", view]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        raise ValueError(f"{' '.join(command)} failed:\n{result.stderr}")
    if view != OVERDRAW_VIEW:
        return None
    lines = OVERDRAW_RE.findall(result.stderr)
    if len(lines) != 1:
        raise ValueError(f"{' '.join(command)}: {len(lines)} overdraw lines")
    return dict(zip(FIELDS, map(int, lines[0])))


def provenance():
    commit = subprocess.run(["git", "describe", "--always", "--dirty"], cwd=ROOT, capture_output=True, text=True,
                            check=True).stdout.strip()
    return f"# host render at {commit}"


def render_views(text, capture_name, scenes, data_dir, images):
    """Renders every pose, writes the data file and each scene's sheet of worst poses."""
    renderer = build()
    poses = sorted(wanted(text, scenes))
    with tempfile.TemporaryDirectory() as tmp, concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        tmp = pathlib.Path(tmp)

        def picture(pose, view):
            return tmp / f"{'-'.join(map(str, pose))}-{view or 'shaded'}.bmp"

        totals = dict(zip(poses, pool.map(lambda p: render(renderer, p, OVERDRAW_VIEW, picture(p, OVERDRAW_VIEW)),
                                          poses)))
        worst = [pose for pose in poses if pose[2] != PATH_POSE]
        list(pool.map(lambda p: render(renderer, p, None, picture(p, None)), worst))
        lines = [provenance() + f" of the poses in {capture_name}", "\t".join(COLUMNS + FIELDS)]
        lines.extend("\t".join(map(str, pose + tuple(totals[pose][name] for name in FIELDS))) for pose in poses)
        (data_dir / DATA_FILE).write_text("\n".join(lines) + "\n", encoding="utf-8")
        for scene in scenes:
            sheet(scene, [p for p in worst if p[0] == scene], totals, picture, images / f"{scene}-poses.png")


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


def sheet(scene, worst, totals, picture, out):
    """Rank across, and per quarter the shaded pose above its overdraw."""
    ranks = sorted({int(pose[2]) for pose in worst})
    size = {quarter: Image.open(picture(next(p for p in worst if p[1] == quarter), None)).size for quarter in QUARTERS}
    heights = [size[quarter][1] / size[quarter][0] for quarter in QUARTERS for _ in range(2)]
    fig, axes = subplots(len(heights), len(ranks), (CELL_INCHES * len(ranks), CELL_INCHES * sum(heights)),
                         gridspec_kw={"height_ratios": heights})
    for row, quarter in enumerate(QUARTERS):
        for column, rank in enumerate(ranks):
            pose = next(p for p in worst if p[1] == quarter and p[2] == str(rank))
            shaded, overdraw = axes[2 * row][column], axes[2 * row + 1][column]
            shaded.imshow(Image.open(picture(pose, None)))
            shaded.set_title(f"{quarter} rank {rank}, {pose[3] / MS_PER_SECOND:.2f} s", fontsize=LABEL_SIZE)
            overdraw.imshow(Image.open(picture(pose, OVERDRAW_VIEW)))
            overdraw.set_title(f"{mean(totals[pose], 'tested'):.2f} tested, {mean(totals[pose], 'written'):.2f}"
                               " written / px", fontsize=LABEL_SIZE)
            for axis in (shaded, overdraw):
                axis.set_axis_off()
    save(fig, out)
    Image.open(out).convert("RGB").quantize(SHEET_COLORS, dither=Image.Dither.NONE).save(out, optimize=True)


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


def tables(text, scenes, data_dir, folder):
    """The README's pose pictures, per-pose overdraw and findings, from the data file; fails when the data file's
    poses are not the capture's."""
    measured = read(data_dir)
    missing, extra = wanted(text, scenes) - measured.keys(), measured.keys() - wanted(text, scenes)
    if missing or extra:
        raise ValueError(f"{DATA_FILE}: {len(missing)} poses missing and {len(extra)} not in the capture; "
                         "run with --render-views")
    poses = path_poses(text)
    lines = ["### Worst frames on the host", "",
             "Each worst frame's pose drawn by the host renderer, shaded above the overdraw view: "
             f"blue is one layer tested, red {RAMP_LAYERS} or more.",
             "Tested counts every span that reached a pixel, written every one that won its depth test; "
             "per pixel is over the pixels drawn.", ""]
    for scene in scenes:
        lines.append(f"![{scene}: the worst frames' poses, shaded and as overdraw]({folder}/{scene}-poses.png)")
    lines.extend(("", "#### Overdraw at the worst frames", "",
                  "Path percentile places a pose's tested layers per pixel among the evenly spaced poses' "
                  "(100: none had more).", "",
                  "| Scene | Quarter | Rank | Path s | Frame ms | Tested / px | Written / px | Most tested | "
                  "Most written | Path percentile |", "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"))
    findings = ["#### Is a worst frame overdraw-heavy?", "",
                f"Over the evenly spaced poses, ρ ranks each pose's replayed ms against its tested layers and its "
                f"{TRIANGLES.replace('_', ' ')}: whichever is higher follows the frame time more closely.", "",
                "| Scene | Quarter | Path tested / px, median | Worst tested / px, median | Worst path percentile, "
                f"median | ρ ms ~ layers | ρ ms ~ {TRIANGLES.replace('_', ' ')} | Follows |",
                "|---|---|---:|---:|---:|---:|---:|---|"]
    for scene in scenes:
        for quarter in QUARTERS:
            path, ranked = poses[scene, quarter]
            along = [measured[scene, quarter, PATH_POSE, pose["t_ms"]] for pose in path]
            layers = [mean(totals, "tested") for totals in along]
            worst = []
            for frame, _ in ranked:
                totals = measured[scene, quarter, str(frame["rank"]), frame["t_ms"]]
                worst.append(mean(totals, "tested"))
                lines.append(f"| {scene} | {quarter} | {frame['rank']} | {frame['t_ms'] / MS_PER_SECOND:.2f} | "
                             f"{frame['frame_us'] / US_PER_MS:.1f} | {worst[-1]:.2f} | "
                             f"{mean(totals, 'written'):.2f} | {totals['tested_max']} | {totals['written_max']} | "
                             f"{percentile(worst[-1], layers):.0f} |")
            ms = [sum(pose["stages"].values()) for pose in path]
            by_layers = spearman(ms, [t["tested"] for t in along])
            by_triangles = spearman(ms, [pose["counts"][TRIANGLES] for pose in path])
            follows = ("–" if by_layers is None or by_triangles is None
                       else "layers" if by_layers > by_triangles else TRIANGLES.replace("_", " "))
            findings.append(f"| {scene} | {quarter} | {statistics.median(layers):.2f} | "
                            f"{statistics.median(worst):.2f} | "
                            f"{statistics.median(percentile(w, layers) for w in worst):.0f} | {signed(by_layers)} | "
                            f"{signed(by_triangles)} | {follows} |")
    return "\n".join(lines + [""] + findings) + "\n"
