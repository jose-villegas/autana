"""Write UI example render and state tables from scene captures.

    python launcher/tools/render/ui_examples_doc.py --renders DIR --out OUT

Identical state renders share the rest image. Tables are written under OUT/tables.
"""
import argparse
import os
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "gen"))
from examples_doc import block_name
from generated_blocks import block_owners

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
EXAMPLES = HERE / "ui_examples"
BASELINE = HERE / "scenes" / "ui_examples_render_baseline.txt"
IMAGES = ROOT / "docs" / "images" / "ui"
# The scene's rest renders by label suffix, and the column each one heads.
ORIENTATIONS = {"": "Portrait", "-landscape": "Landscape"}


def block(name, kind):
    return block_name("ui", name, kind)


def examples():
    """Control examples and screen examples, by name."""
    return sorted(EXAMPLES.glob("*.c")), sorted((EXAMPLES / "screens").glob("*.c"))


def pinned():
    """The scene's render labels, in the order it renders them."""
    return [line.split()[0] for line in BASELINE.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")]


def states(name):
    """Example `name`'s states, read from its pinned labels: `name` is rest,
    `name-<state>` the others."""
    found = [label[len(name) + 1:] for label in pinned() if label.startswith(f"{name}-")]
    return ["rest", *(state for state in found if f"-{state}" not in ORIENTATIONS)]


def orientations(name):
    """(heading, label) for each orientation `name` is pinned at rest in."""
    labels = set(pinned())
    return [(heading, name + suffix) for suffix, heading in ORIENTATIONS.items() if name + suffix in labels]


def image_dir(owners, name):
    """The images folder as the document owning block `name` links it."""
    return pathlib.PurePath(os.path.relpath(IMAGES, (ROOT / owners[name]).parent)).as_posix()


def fires(renders, label):
    log = (renders / f"{label}.log").read_text(encoding="utf-8").splitlines()
    said = [line[len("fires "):] for line in log if line.startswith("fires ")]
    if len(said) != 1:
        raise ValueError(f"{label}.log reports {len(said)} taps, not one")
    return said[0]


def write_renders(renders, out):
    from PIL import Image

    (out / "ui").mkdir(parents=True, exist_ok=True)
    (out / "tables").mkdir(parents=True, exist_ok=True)

    def save(label):
        Image.open(renders / f"{label}.bmp").save(out / "ui" / f"{label}.png")

    owners = block_owners(ROOT)
    controls, screens = examples()
    for path in controls + screens:
        name = path.stem
        table = block(name, "renders")
        images = image_dir(owners, table)
        found = orientations(name)
        for _, label in found:
            save(label)
        rows = ["| " + " | ".join(heading for heading, _ in found) + " |", "|---" * len(found) + "|",
                "| " + " | ".join(f"![{name}, {heading.lower()}]({images}/{label}.png)" for heading, label in found)
                + " |"]
        (out / "tables" / f"{table}.md").write_text("\n".join(rows) + "\n", encoding="utf-8")
    for path in controls:
        name = path.stem
        images = image_dir(owners, block(name, "states"))
        rest = (renders / f"{name}.bmp").read_bytes()
        rows = ["| State | Render | Fires |", "|---|---|---|"]
        for state in states(name):
            label = name if state == "rest" else f"{name}-{state}"
            if state != "rest" and (renders / f"{label}.bmp").read_bytes() == rest:
                render = "looks like rest"
            else:
                if state != "rest":
                    save(label)
                render = f"![{name}, {state}]({images}/{label}.png)"
            rows.append(f"| {state} | {render} | {fires(renders, label)} |")
        (out / "tables" / f"{block(name, 'states')}.md").write_text("\n".join(rows) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--renders", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        write_renders(args.renders, args.out)
        return 0
    except (ValueError, OSError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
