"""Render the host scene and place its turning panel inside its corner sweep."""

import argparse
import math
import os
from pathlib import Path
import subprocess
import tempfile

from PIL import Image, ImageDraw


HERE = Path(__file__).resolve().parent
DEFAULT_OUTPUT = HERE.parents[4] / "docs/images/overview/sand-simulation.gif"


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


def extract_frames(video: Path, work: Path) -> list[Path]:
    subprocess.run(
        [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(video),
            "-vf", "select=not(mod(n\\,6))", "-vsync", "0", str(work / "frame-%03d.png"),
        ],
        check=True,
    )
    return sorted(work.glob("frame-*.png"))


def composite(panel: Image.Image, angle: float) -> Image.Image:
    size = 670
    canvas = Image.new("RGB", (size, size), "#101923")
    draw = ImageDraw.Draw(canvas)
    draw.ellipse((9, 9, size - 10, size - 10), outline="#39566a", width=2)
    draw.ellipse((size // 2 - 3, size // 2 - 3, size // 2 + 3, size // 2 + 3), fill="#39566a")
    plate = Image.new("RGBA", (508, 418), "#080d13")
    plate.paste(panel.resize((480, 390), Image.Resampling.LANCZOS).convert("RGBA"), (14, 14))
    spun = plate.rotate(-angle, Image.Resampling.BICUBIC, expand=True)
    canvas.paste(spun, ((size - spun.width) // 2, (size - spun.height) // 2), spun)
    return canvas


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--contact", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        video, angles = render_scene(work)
        paths = extract_frames(video, work)
        images = [composite(Image.open(path), angles[i * 6]) for i, path in enumerate(paths)]
        if args.contact:
            contact = Image.new("RGB", (2010, 1340))
            for slot, index in enumerate((0, 12, 24, 28, 29, 39)):
                contact.paste(images[index], ((slot % 3) * 670, (slot // 3) * 670))
            args.contact.parent.mkdir(parents=True, exist_ok=True)
            contact.save(args.contact)
        sequence = [image.quantize(colors=32, method=Image.Quantize.FASTOCTREE) for image in images]
        sequence += sequence[-2:0:-1]
        args.output.parent.mkdir(parents=True, exist_ok=True)
        sequence[0].save(
            args.output, save_all=True, append_images=sequence[1:], duration=99, loop=0,
            optimize=True, disposal=2,
        )
        print(f"{args.output}: {len(sequence)} frames, {args.output.stat().st_size} bytes")


if __name__ == "__main__":
    main()
