#!/usr/bin/env python3
"""Keep launcher/idf-pin.json true to the ESP-IDF image it names. Run inside that image.

    python3 scripts/deps/idf_pin.py check                         # commit and esp_clang match the image
    python3 scripts/deps/idf_pin.py write --image REF --date DAY  # pin this image, reading the rest from it
    python3 scripts/deps/idf_pin.py newer-line < TAGS_JSON        # a release line newer than the pin's tag
    python3 scripts/deps/idf_pin.py show tag|image|commit         # one field, for a workflow

The image is named by digest; the commit and the esp-clang release are read from the
ESP-IDF inside it ($IDF_PATH), so a person never types them. `write` is what the ESP-IDF
refresh workflow (.github/workflows/idf-refresh.yml) runs after it pulls the newest image of
the pinned tag.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "launcher/tools/build"))
from espressif import IDF_PIN, idf_drift, idf_facts, idf_image_tag, idf_path, idf_pin  # noqa: E402

RELEASE_LINE = re.compile(r"release-v(\d+)\.(\d+)")


def check(pin, found):
    wrong = idf_drift(found, pin)
    for line in wrong:
        print(f"{IDF_PIN}: {line}", file=sys.stderr)
    if wrong:
        print("Run the ESP-IDF refresh workflow, or `python3 scripts/deps/idf_pin.py write` inside the "
              "pinned image; docs/tools/Framework-Versions.md says how.", file=sys.stderr)
    return 1 if wrong else 0


def write(pin, found, image, date, path=ROOT / IDF_PIN):
    pin = {**pin, "image": image, "image_date": date, **found}
    path.write_text(json.dumps(pin, indent=2) + "\n", encoding="utf-8", newline="\n")
    return pin


def newer_line(tag, tags):
    """The newest release line among Docker Hub's `tags` listing when it is newer than `tag`, else None."""
    lines = {tuple(map(int, m.groups())): name for name in (t["name"] for t in tags["results"])
             if (m := RELEASE_LINE.fullmatch(name))}
    newest = max(lines, default=None)
    current = RELEASE_LINE.fullmatch(tag)
    return lines[newest] if newest and (not current or newest > tuple(map(int, current.groups()))) else None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("check")
    writer = sub.add_parser("write")
    writer.add_argument("--image", required=True, help="the image by tag and digest, repository:tag@sha256:...")
    writer.add_argument("--date", required=True, help="the day the image was built, YYYY-MM-DD")
    sub.add_parser("newer-line")
    sub.add_parser("show").add_argument("field", choices=("tag", "image", "commit"))
    args = parser.parse_args(argv)
    if args.command == "show":
        pin = idf_pin()
        print(idf_image_tag(pin["image"]) if args.field == "tag" else pin[args.field])
        return 0
    if args.command == "newer-line":
        line = newer_line(idf_image_tag(idf_pin()["image"]), json.load(sys.stdin))
        if line:
            print(line)
        return 0
    found = idf_facts(idf_path())
    if args.command == "check":
        return check(idf_pin(), found)
    write(idf_pin(), found, args.image, args.date)
    return 0


if __name__ == "__main__":
    sys.exit(main())
