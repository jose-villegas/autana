"""Put the face examples into the UI pages: each example file whole, and each
control's states table with the renders ui_examples_render_host.sh made.

    python launcher/tools/render/ui_examples_doc.py [--check]
    python launcher/tools/render/ui_examples_doc.py --renders DIR --out OUT

The first form writes block ui-<name>-example, wherever a document owns it,
from ui_examples/<name>.c or ui_examples/screens/<name>.c, with the name's
underscores as hyphens; --check only reports a stale block. The second turns
the scene's results in DIR into OUT/ui/*.png, and OUT/tables/ui-<name>-renders.md
(a column per orientation, named by the pinned label) and ui-<name>-states.md,
for render_doc_images.sh.
A state render with the same pixels as rest is not kept: its row says so.
"""
import argparse
import os
import pathlib
import sys

from generated_blocks import block_owners, replace_block

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
EXAMPLES = HERE / "ui_examples"
BASELINE = HERE / "scenes" / "ui_examples_render_baseline.txt"
IMAGES = ROOT / "docs" / "images" / "ui"
# The scene's rest renders by label suffix, and the column each one heads.
ORIENTATIONS = {"": "Portrait", "-landscape": "Landscape"}
CHECK = "python launcher/tools/render/ui_examples_doc.py --check"


def block(name, kind):
    """A block name for example `name`: block names take hyphens, not underscores."""
    return f"ui-{name.replace('_', '-')}-{kind}"


def examples():
    """Control examples and screen examples, by name."""
    return sorted(EXAMPLES.glob("*.c")), sorted((EXAMPLES / "screens").glob("*.c"))


def write_examples(check):
    owners = block_owners(ROOT)
    controls, screens = examples()
    changed = False
    for path in controls + screens:
        name = block(path.stem, "example")
        if name not in owners:
            raise ValueError(f"no document owns generated block {name} for {path.relative_to(ROOT).as_posix()}")
        body = "```c\n" + path.read_text(encoding="utf-8").replace("\r\n", "\n").rstrip("\n") + "\n```"
        different = replace_block(ROOT / owners[name], name, body, check, CHECK)
        print(f"{'changed' if different else 'same'} {owners[name]}#{name}")
        changed |= different
    return changed


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
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--renders", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args()
    try:
        if args.renders:
            if not args.out:
                parser.error("--renders needs --out")
            write_renders(args.renders, args.out)
            return 0
        return int(write_examples(args.check) and args.check)
    except (ValueError, OSError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
