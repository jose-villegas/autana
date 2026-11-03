"""The figure index: docs/tools/Figures.md, a section per area of docs/images/ with every image's picture,
what it shows (the alt text of the first page showing it), the pages that show it and the producer that
makes it. A still is shown scaled to THUMBNAIL_WIDTH; a GIF by a small animated copy under THUMBNAILS,
made from the committed GIF, so the page does not load every full GIF. figures_runner.py index runs it.
"""
import html
import io
import pathlib
import posixpath
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
sys.path.insert(0, str(ROOT / "scripts/gates"))
from generated_blocks import replace_block  # noqa: E402
from tracked import tracked_files  # noqa: E402

# PAGE and BLOCK are read by the app-name gate, which runs without Pillow, so the picture code imports
# Pillow where it uses it.

PAGE = "docs/tools/Figures.md"
BLOCK = "figure-index"
CHECK = "python launcher/tools/docs/figures_runner.py index --check"
IMAGES = "docs/images"
THUMBNAILS = "docs/tools/figure-thumbnails"
THUMBNAIL_WIDTH = 160
THUMBNAIL_FRAME_MS = 200  # a copy shows a frame at least this long, which keeps it small
PICTURES = (".png", ".gif")
NOTHING = "—"
MARKDOWN_IMAGE = re.compile(r"!\[([^\]]*)\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
HTML_IMAGE = re.compile(r"<(?:img|source)\b[^>]*>", re.I)
ATTRIBUTE = re.compile(r"([\w-]+)\s*=\s*\"([^\"]*)\"")


def resolved(page, target):
    """The repository path `target`, a link on `page`, names; None for a link off the repository."""
    target = target.split("#")[0].split("?")[0]
    if not target or re.match(r"[a-z][a-z0-9+.-]*:", target, re.I):
        return None
    return posixpath.normpath(posixpath.join(posixpath.dirname(page), target))


def embedded(page, text):
    """(image, alt) for each picture `page` shows, in order."""
    found = [(match.start(), match.group(2), match.group(1)) for match in MARKDOWN_IMAGE.finditer(text)]
    for match in HTML_IMAGE.finditer(text):
        attributes = {name.lower(): value for name, value in ATTRIBUTE.findall(match.group(0))}
        targets = [attributes.get("src", "")] + [part.split()[0] for part in attributes.get("srcset", "").split(",")
                                                 if part.strip()]
        found += [(match.start(), target, html.unescape(attributes.get("alt", ""))) for target in targets]
    return [(resolved(page, target), alt.strip()) for _, target, alt in sorted(found) if resolved(page, target)]


def showings(root):
    """{image: [(page, alt), ...]} over every tracked page but the index itself."""
    shown = {}
    for page in tracked_files(root, ["*.md"]):
        if page == PAGE:
            continue
        for image, alt in embedded(page, (root / page).read_text(encoding="utf-8")):
            if image.startswith(IMAGES + "/") and page not in [seen for seen, _ in shown.get(image, [])]:
                shown.setdefault(image, []).append((page, alt))
    return shown


def opened(root, path):
    """`path` to hand Image.open: the file, or where it is not on disk (the commit hook's snapshot holds
    no binaries) its blob in git's index."""
    if (root / path).is_file():
        return root / path
    return io.BytesIO(subprocess.run(["git", "cat-file", "blob", f":{path}"], cwd=root, check=True,
                                     capture_output=True).stdout)


def thumbnail_of(image):
    """Where the index's copy of a GIF under docs/images/ goes."""
    return f"{THUMBNAILS}/{image.removeprefix(IMAGES + '/')}"


def write_thumbnail(source, out):
    """A THUMBNAIL_WIDTH copy of the GIF `source` as long as it: each frame shown at least
    THUMBNAIL_FRAME_MS, the frames it then covers dropped."""
    from PIL import Image, ImageSequence
    from docs.figures import save_gif
    frames, delays = [], []
    with Image.open(source) as gif:
        for frame in ImageSequence.Iterator(gif):
            if delays and delays[-1] < THUMBNAIL_FRAME_MS:
                delays[-1] += frame.info["duration"]
                continue
            picture = frame.convert("RGB")
            if picture.width > THUMBNAIL_WIDTH:
                height = max(1, round(picture.height * THUMBNAIL_WIDTH / picture.width))
                picture = picture.resize((THUMBNAIL_WIDTH, height), Image.Resampling.LANCZOS)
            frames.append(picture)
            delays.append(frame.info["duration"])
    save_gif(frames, out, delays)


def delays_of(path):
    from PIL import Image, ImageSequence
    with Image.open(path) as gif:
        return [frame.info["duration"] for frame in ImageSequence.Iterator(gif)]


def link(path):
    return posixpath.relpath(path, posixpath.dirname(PAGE))


def cell(text):
    return text.replace("|", "\\|").replace("\n", " ") or NOTHING


def picture(root, image):
    """The Picture cell: the thumbnail, linking to the image."""
    from PIL import Image
    if image.endswith(".gif"):
        source, width = thumbnail_of(image), THUMBNAIL_WIDTH
    else:
        with Image.open(opened(root, image)) as still:
            source, width = image, min(THUMBNAIL_WIDTH, still.width)
    return f'<a href="{link(image)}"><img src="{link(source)}" width="{width}" alt=""></a>'


def page_name(page):
    return page.removeprefix("docs/").removesuffix(".md")


def body(root, images, shown, owner):
    """The block: a summary row per area, then a section per area with a row per image."""
    areas = {}
    for image in images:
        areas.setdefault(image.split("/")[2], []).append(image)
    lines = ["| Area | Images | Declared by a producer | Shown nowhere |", "|---|---|---|---|"]
    for area, named in areas.items():
        lines.append(f"| [{area}](#{area}) | {len(named)} | {sum(owner(image) is not None for image in named)} | "
                     f"{sum(image not in shown for image in named)} |")
    for area, named in areas.items():
        lines += ["", f"## {area}", "", "| Picture | What it shows | Shown in | Made by |", "|---|---|---|---|"]
        for image in named:
            pages = shown.get(image, [])
            alt = next((alt for _, alt in pages if alt), "")
            maker = owner(image)
            made = f"[{posixpath.basename(maker)}]({link(maker)})" if maker else "not declared yet"
            lines.append(f"| {picture(root, image)} | {cell(alt)} | "
                         f"{', '.join(f'[{page_name(page)}]({link(page)})' for page, _ in pages) or NOTHING} | {made} |")
    return "\n".join(lines)


def index(owner, check=False, root=ROOT):
    """Write the page and the GIF thumbnails, or with `check` say what differs; returns the differences.
    `owner(image)` is the script that declares an image under docs/images/, or None. An image no page
    shows, and an image a page shows that does not exist, are warnings on stderr."""
    from compare_images import compare, load_frames
    images = sorted(name for name in tracked_files(root, [IMAGES]) if name.endswith(PICTURES))
    shown = showings(root)
    for image, pages in sorted(shown.items()):
        if image not in images and not (root / image).is_file():
            print(f"missing {image}: shown by {', '.join(page for page, _ in pages)}", file=sys.stderr)
    for image in images:
        if image not in shown:
            print(f"shown nowhere {image}", file=sys.stderr)
    differences = []
    wanted = {thumbnail_of(image): image for image in images if image.endswith(".gif")}
    kept = set(tracked_files(root, [THUMBNAILS])) | {thumbnail for thumbnail in wanted if (root / thumbnail).is_file()}
    with tempfile.TemporaryDirectory() as work:
        for thumbnail, image in wanted.items():
            made = pathlib.Path(work) / thumbnail
            write_thumbnail(opened(root, image), made)
            committed = root / thumbnail
            how = "new thumbnail" if thumbnail not in kept else (
                compare(load_frames(made), load_frames(opened(root, thumbnail)))
                or (delays_of(made) != delays_of(opened(root, thumbnail)) and "frame delays"))
            if how:
                differences.append(f"changed {thumbnail}: {how}")
                if not check:
                    committed.parent.mkdir(parents=True, exist_ok=True)
                    committed.write_bytes(made.read_bytes())
    for stale in sorted(kept - set(wanted)):
        differences.append(f"orphan {stale}: its GIF under {IMAGES}/ is gone")
        if not check:
            (root / stale).unlink(missing_ok=True)
    if replace_block(root / PAGE, BLOCK, body(root, images, shown, owner), check, CHECK):
        differences.append(f"changed {PAGE}")
    return differences
