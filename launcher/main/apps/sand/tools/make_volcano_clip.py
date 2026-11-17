"""Render the host scene and place its turning panel inside its corner sweep."""

import argparse
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw


HERE = Path(__file__).resolve().parent
DEFAULT_OUTPUT = HERE.parents[4] / "docs/images/overview/sand-simulation.gif"
sys.path.insert(0, str(HERE.parents[3] / "tools"))
from docs.figures import save_gif  # noqa: E402
from render.check_avi import read_video  # noqa: E402

FRAME_STEP = 6  # every sixth rendered frame, one GIF frame
FRAME_MS = 99


def render_scene(work: Path) -> tuple[Path, list[float]]:
    angles = work / "angles.txt"
    env = os.environ.copy()
    env["SAND_ANGLE_PATH"] = str(angles)
    subprocess.run(
        ["sh", str(HERE / "sand_sim_render_host.sh"), "-o", str(work), "--video"],
        check=True,
        env=env,
    )
    return work / "simulation-landscape.avi", [float(line) for line in angles.read_text().splitlines()]


MARGIN = 14


def canvas_size(panel: Image.Image) -> int:
    """Wide enough for the plate's diagonal, so every angle of the turn fits."""
    return math.ceil(math.hypot(panel.width + 2 * MARGIN, panel.height + 2 * MARGIN)) + 12


def composite(panel: Image.Image, angle: float) -> Image.Image:
    size = canvas_size(panel)
    canvas = Image.new("RGB", (size, size), "#101923")
    draw = ImageDraw.Draw(canvas)
    draw.ellipse((9, 9, size - 10, size - 10), outline="#39566a", width=2)
    draw.ellipse((size // 2 - 3, size // 2 - 3, size // 2 + 3, size // 2 + 3), fill="#39566a")
    # The panel at its own resolution: only the turn itself resamples it.
    plate = Image.new("RGBA", (panel.width + 2 * MARGIN, panel.height + 2 * MARGIN), "#080d13")
    plate.paste(panel.convert("RGBA"), (MARGIN, MARGIN))
    spun = plate.rotate(-angle, Image.Resampling.BICUBIC, expand=True)
    canvas.paste(spun, ((size - spun.width) // 2, (size - spun.height) // 2), spun)
    # Scaled once, after the turn, to the panel's width: the README table
    # keeps every column as wide as one screen.
    return canvas.resize((panel.width, panel.width), Image.Resampling.LANCZOS)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--contact", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        video, angles = render_scene(work)
        frames = list(read_video(video)[1])[::FRAME_STEP]
        images = [composite(Image.fromarray(frame), angles[i * FRAME_STEP]) for i, frame in enumerate(frames)]
        if args.contact:
            size = images[0].width
            contact = Image.new("RGB", (3 * size, 2 * size))
            for slot, index in enumerate((0, 12, 24, 28, 29, 39)):
                contact.paste(images[index], ((slot % 3) * size, (slot // 3) * size))
            args.contact.parent.mkdir(parents=True, exist_ok=True)
            contact.save(args.contact)
        sequence = images + images[-2:0:-1]
        save_gif(sequence, args.output, FRAME_MS)
        print(f"{args.output}: {len(sequence)} frames, {args.output.stat().st_size} bytes")


if __name__ == "__main__":
    main()
