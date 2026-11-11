"""Generate mean set-up triangle bins along scene paths with the host renderer."""
import argparse
import csv
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from render.cull_counts import size_fields

BIN_FIELDS = size_fields()
FRAME_DT_MS = 16
RENDER_SCRIPT = pathlib.Path(__file__).with_name("render_lab_render_host.sh")
DEFAULT_OUTPUT = ROOT / "docs/render/data/scene-triangle-bins.json"


def mean_bins(path):
    """Mean bins from every frame, rejecting missing fields or inconsistent totals."""
    with pathlib.Path(path).open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError(f"{path}: no frames")
    totals = dict.fromkeys(BIN_FIELDS, 0)
    for row in rows:
        values = {name: int(row[name]) for name in BIN_FIELDS}
        if sum(values.values()) != int(row["triangles_set_up"]):
            raise ValueError(f"{path}: bins do not sum to triangles_set_up")
        for name, value in values.items():
            totals[name] += value
    return {name: total / len(rows) for name, total in totals.items()}


def generate(binary, scenes, folder, commands):
    """Render (key, frames) inputs in portrait, returning machine-independent means."""
    result = {}
    for key, frames in scenes:
        counts = folder / (key + ".csv")
        command = [str(binary), "--scene", key, "--quarter", "0", "--view", "cull", "--counts", str(counts),
                   "--frames", str(frames), "--dt", str(FRAME_DT_MS), "--no-hud", "-o", str(folder / (key + ".bmp"))]
        for text in commands.get(key, []):
            command += ["--console", text]
        subprocess.run(command, check=True, stdin=subprocess.DEVNULL)
        result[key] = mean_bins(counts)
    return {"quarter": 0, "dt_ms": FRAME_DT_MS, "frames": dict(scenes),
            "console": {key: commands[key] for key, _ in scenes if key in commands}, "scenes": result}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", action="append", nargs=2, metavar=("KEY", "FRAMES"), required=True)
    parser.add_argument("--console", action="append", nargs=2, metavar=("KEY", "COMMAND"), default=[])
    parser.add_argument("--renderer", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args(argv)
    scenes = [(key, int(frames)) for key, frames in args.scene]
    if any(frames <= 0 for _, frames in scenes) or len(dict(scenes)) != len(scenes):
        parser.error("scene keys must be unique and frame counts positive")
    commands = {}
    for key, command in args.console:
        commands.setdefault(key, []).append(command)
    with tempfile.TemporaryDirectory(prefix="scene_triangle_bins_") as directory:
        folder = pathlib.Path(directory)
        binary = args.renderer.resolve() if args.renderer else None
        if binary is None:
            subprocess.run(["sh", str(RENDER_SCRIPT), "--build-only", "-o", str(folder)], check=True, stdin=subprocess.DEVNULL)
            binary = folder / "render_lab_render"
            if not binary.exists():
                binary = binary.with_suffix(".exe")
        result = generate(binary, scenes, folder, commands)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
