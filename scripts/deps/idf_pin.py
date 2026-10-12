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
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "launcher/tools/build"))
from espressif import idf_path, idf_pin  # noqa: E402

PIN = ROOT / "launcher" / "idf-pin.json"

ESP_CLANG = re.compile(r"esp-(\d+\.\d+\.\d+)")
RELEASE_LINE = re.compile(r"release-v(\d+)\.(\d+)")
DERIVED = ("commit", "esp_clang")


def esp_clang(tools_json):
    """The LLVM release of the esp-clang ESP-IDF's tools.json recommends: esp-19.1.2_20250312 is 19.1.2."""
    tools = json.loads(Path(tools_json).read_text(encoding="utf-8"))["tools"]
    tool = next(t for t in tools if t["name"] == "esp-clang")
    name = next(v["name"] for v in tool["versions"] if v.get("status") == "recommended")
    return ESP_CLANG.match(name).group(1)


def facts(idf):
    """What the pin records about the ESP-IDF at `idf`."""
    commit = subprocess.run(["git", "-C", str(idf), "rev-parse", "HEAD"], check=True,
                            capture_output=True, text=True).stdout.strip()
    return {"commit": commit, "esp_clang": esp_clang(Path(idf) / "tools" / "tools.json")}


def mismatches(pin, found):
    return [f"{PIN.name} records {key} {pin.get(key) or '(none)'}, the image has {found[key]}"
            for key in DERIVED if pin.get(key) != found[key]]


def check(pin, found):
    wrong = mismatches(pin, found)
    for line in wrong:
        print(line, file=sys.stderr)
    if wrong:
        print("Run the ESP-IDF refresh workflow, or `python3 scripts/deps/idf_pin.py write` inside the "
              "pinned image; docs/tools/Framework-Versions.md says how.", file=sys.stderr)
    return 1 if wrong else 0


def write(pin, found, image, date, path=PIN):
    pin = {**pin, "image": image, "image_date": date, **found}
    path.write_text(json.dumps(pin, indent=2) + "\n", encoding="utf-8", newline="\n")
    return pin


def tag(image):
    """The tag of an image reference: espressif/idf:release-v5.5@sha256:... is release-v5.5."""
    return image.split("@")[0].rpartition(":")[2]


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
        print(tag(pin["image"]) if args.field == "tag" else pin[args.field])
        return 0
    if args.command == "newer-line":
        line = newer_line(tag(idf_pin()["image"]), json.load(sys.stdin))
        if line:
            print(line)
        return 0
    found = facts(os.environ.get("IDF_PATH") or idf_path())
    if args.command == "check":
        return check(idf_pin(), found)
    write(idf_pin(), found, args.image, args.date)
    return 0


if __name__ == "__main__":
    sys.exit(main())
