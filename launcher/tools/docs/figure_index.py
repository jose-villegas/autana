"""The figure index: docs/tools/Figures.md, a section per area of docs/images/ with every image's picture,
what it shows (the first alt text a page gives it), the pages that show it and the producer that
makes it. A still is shown scaled to THUMBNAIL_WIDTH; a GIF by a small animated copy under THUMBNAILS,
made from the committed GIF, so the page does not load every full GIF. figures_runner.py index runs it.
"""
import html
import io
import pathlib
import posixpath
import re
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
sys.path.insert(0, str(ROOT / "scripts/gates"))
from check_doc_index import blank_fences, resolved_link  # noqa: E402
from generated_blocks import replace_block  # noqa: E402
from tracked import revision_contents, tracked_files  # noqa: E402

# Pillow is imported inside the functions that use it: the app-name gate imports BLOCK from here and
# runs without Pillow.

PAGE = "docs/tools/Figures.md"
BLOCK = "figure-index"
WRITE = "python launcher/tools/docs/figures_runner.py index"
CHECK = f"{WRITE} --check"
IMAGES = "docs/images"
THUMBNAILS = "docs/tools/figure-thumbnails"
THUMBNAIL_WIDTH = 160
THUMBNAIL_FRAME_MS = 200  # a copy shows a frame at least this long, which keeps it small
PICTURES = (".png", ".gif")
NOTHING = "—"
MARKDOWN_IMAGE = re.compile(r"!\[([^\]]*)\]\(\s*(?:<([^>]*)>|([^)\s]+))(?:\s+(?:\"[^\"]*\"|'[^']*'|\([^)]*\)))?\s*\)")
# ![alt][label], ![alt][] and ![alt], each naming a `[label]: target` definition.
REFERENCE_IMAGE = re.compile(r"!\[([^\]]*)\](?:\[([^\]]*)\])?(?!\()")
DEFINITION = re.compile(r"^ {0,3}\[([^\]]+)\]:\s*(?:<([^>]*)>|(\S+))", re.M)
HTML_IMAGE = re.compile(r"<(?:img|source)\b[^>]*>", re.I)
ATTRIBUTE = re.compile(r"([\w-]+)\s*=\s*\"([^\"]*)\"")


def embedded(page, text):
    """(image, alt) for each picture `page` shows outside code fences, in order."""
    text = "\n".join(blank_fences(text.splitlines()))
    found = [(match.start(), match.group(2) or match.group(3), match.group(1))
             for match in MARKDOWN_IMAGE.finditer(text)]
    definitions = {label.lower(): angled or bare for label, angled, bare in DEFINITION.findall(text)}
    for match in REFERENCE_IMAGE.finditer(text):
        target = definitions.get((match.group(2) or match.group(1)).lower())
        if target:
            found.append((match.start(), target, match.group(1)))
    for match in HTML_IMAGE.finditer(text):
        attributes = {name.lower(): html.unescape(value) for name, value in ATTRIBUTE.findall(match.group(0))}
        targets = [attributes.get("src", "")] + [part.split()[0] for part in attributes.get("srcset", "").split(",")
                                                 if part.strip()]
        found += [(match.start(), target, attributes.get("alt", "")) for target in targets]
    links = [(resolved_link(page, target), alt.strip()) for _, target, alt in sorted(found)]
    return [(image, alt) for image, alt in links if image]


def showings(root):
    """{image: [(page, alt), ...]} over every tracked page but the index itself; images outside docs/images/
    are included."""
    shown, seen = {}, set()
    for page in tracked_files(root, ["*.md"]):
        if page == PAGE:
            continue
        for image, alt in embedded(page, (root / page).read_text(encoding="utf-8")):
            if (image, page) not in seen:
                seen.add((image, page))
                shown.setdefault(image, []).append((page, alt))
    return shown


def readable(root, paths):
    """{path: the file, or where it is not on disk (the commit hook's snapshot holds no binaries) its
    bytes from git's index}; opened() turns either into what Image.open takes."""
    absent = [path for path in paths if not (root / path).is_file()]
    blobs = dict(revision_contents(root, "", absent)) if absent else {}
    return {path: blobs.get(path, root / path) for path in paths}


def opened(source):
    """A readable() entry as Image.open takes it, each call from the start."""
    return io.BytesIO(source) if isinstance(source, bytes) else source


def thumbnail_of(image):
    """Where the index's copy of a GIF under docs/images/ goes."""
    return f"{THUMBNAILS}/{image.removeprefix(IMAGES + '/')}"


def link(path):
    return posixpath.relpath(path, posixpath.dirname(PAGE))


def cell(text):
    return text.replace("|", "\\|").replace("\n", " ") or NOTHING


def picture(image, source):
    """The Picture cell: the thumbnail, linking to the image; `source` opens a still."""
    from PIL import Image
    if image.endswith(".gif"):
        shown, width = thumbnail_of(image), THUMBNAIL_WIDTH
    else:
        with Image.open(source) as still:
            shown, width = image, min(THUMBNAIL_WIDTH, still.width)
    return f'<a href="{link(image)}"><img src="{link(shown)}" width="{width}" alt=""></a>'


def page_name(page):
    return page.removeprefix("docs/").removesuffix(".md")


def body(images, sources, shown, owner):
    """The block: a summary row per area, then a section per area with a row per image; `sources` is
    readable()'s."""
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
            lines.append(f"| {picture(image, opened(sources[image]))} | {cell(alt)} | "
                         f"{', '.join(f'[{page_name(page)}]({link(page)})' for page, _ in pages) or NOTHING} | {made} |")
    return "\n".join(lines)


def index(owner, check=False, root=ROOT):
    """Write the page and the GIF thumbnails, or with `check` say what differs; returns the differences.
    `owner(image)` is the script that declares an image under docs/images/, or None. An image no page
    shows is a difference: show it or stop making it; so is a picture a page shows that is not a tracked file."""
    from compare_images import compare, load_frames
    from docs.figures import gif_delays, shrink_gif
    images = sorted(name for name in tracked_files(root, [IMAGES]) if name.endswith(PICTURES))
    shown = showings(root)
    differences = [f"shown nowhere {image}" for image in images if image not in shown]
    tracked = set(tracked_files(root))
    differences += [f"missing {image}, shown in {', '.join(page for page, _ in pages)}"
                    for image, pages in sorted(shown.items()) if image not in tracked]
    wanted = {thumbnail_of(image): image for image in images if image.endswith(".gif")}
    kept = set(tracked_files(root, [THUMBNAILS])) | {thumbnail for thumbnail in wanted if (root / thumbnail).is_file()}
    sources = readable(root, [*images, *sorted(kept & set(wanted))])
    with tempfile.TemporaryDirectory() as work:
        for thumbnail, image in wanted.items():
            made = pathlib.Path(work) / thumbnail
            shrink_gif(opened(sources[image]), made, THUMBNAIL_WIDTH, THUMBNAIL_FRAME_MS)
            how = "new thumbnail" if thumbnail not in kept else (
                compare(load_frames(made), load_frames(opened(sources[thumbnail])))
                or (gif_delays(made) != gif_delays(opened(sources[thumbnail])) and "frame delays"))
            if how:
                differences.append(f"changed {thumbnail}: {how}")
                if not check:
                    (root / thumbnail).parent.mkdir(parents=True, exist_ok=True)
                    (root / thumbnail).write_bytes(made.read_bytes())
    for stale in sorted(kept - set(wanted)):
        differences.append(f"orphan {stale}: its GIF under {IMAGES}/ is gone")
        if not check:
            (root / stale).unlink(missing_ok=True)
    if replace_block(root / PAGE, BLOCK, body(images, sources, shown, owner), check, CHECK):
        differences.append(f"changed {PAGE}")
    return differences
