"""Put the face examples into the UI pages: each example file whole, and each
control's states table with the renders ui_examples_render_host.sh made.

    python launcher/tools/render/ui_examples_doc.py [--check]
    python launcher/tools/render/ui_examples_doc.py --renders DIR --out OUT

The first form writes block ui-<name>-example, wherever a document owns it,
from ui_examples/<name>.c or ui_examples/screens/<name>.c, with the name's
underscores as hyphens; --check only reports a stale block. The second turns
the scene's results in DIR into OUT/ui/*.png and OUT/tables/ui-<name>-states.md
for render_doc_images.sh.
A state render with the same pixels as rest is not kept: its row says so.
"""
import argparse
import pathlib
import sys

from generated_blocks import block_owners, replace_block

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
EXAMPLES = HERE / "ui_examples"
BASELINE = HERE / "scenes" / "ui_examples_render_baseline.txt"
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


def states(name):
    """Example `name`'s states in the order the scene renders them, read from
    its pinned labels: `name` is rest, `name-<state>` the others."""
    labels = [line.split()[0] for line in BASELINE.read_text(encoding="utf-8").splitlines()
              if line and not line.startswith("#")]
    found = [label[len(name) + 1:] for label in labels if label.startswith(f"{name}-")]
    return ["rest", *(state for state in found if state != "landscape")]


def fires(renders, label):
    said = (renders / f"{label}.out").read_text(encoding="utf-8").strip()
    if not said.startswith("fires "):
        raise ValueError(f"{label}.out reports no tap: {said!r}")
    return said[len("fires "):]


def write_renders(renders, out):
    from PIL import Image

    (out / "ui").mkdir(parents=True, exist_ok=True)
    (out / "tables").mkdir(parents=True, exist_ok=True)

    def save(label):
        Image.open(renders / f"{label}.bmp").save(out / "ui" / f"{label}.png")

    controls, screens = examples()
    for path in screens:
        save(path.stem)
        save(f"{path.stem}-landscape")
    for path in controls:
        name = path.stem
        save(name)
        save(f"{name}-landscape")
        rest = (renders / f"{name}.bmp").read_bytes()
        rows = ["| State | Render | Fires |", "|---|---|---|"]
        for state in states(name):
            label = name if state == "rest" else f"{name}-{state}"
            if state != "rest" and (renders / f"{label}.bmp").read_bytes() == rest:
                render = "looks like rest"
            else:
                if state != "rest":
                    save(label)
                render = f"![{name}, {state}](../images/ui/{label}.png)"
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
