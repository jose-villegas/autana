#!/usr/bin/env python3
"""Build the architecture explorer: a C4 model of the tree, as one HTML page.

    python launcher/tools/architecture/build.py [-o <dir>]   (default: build/architecture)

Levels 1 and 2 (context, containers) are drawn by hand in template.html,
since nothing in the tree states them. Levels 3 and 4 are read from the
tree at build time: one component per folder of launcher/main/ and per app,
modules from their .c/.h pairs, arrows from #include directives, folder
descriptions and hardware marks from docs/Firmware-Architecture.md's layer
diagram, and summaries from each file's header comment. Nothing here is
kept by hand, so a rebuild is always current.
"""
import argparse
import collections
import datetime
import json
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
MAIN = "launcher/main/"
SUITES = "launcher/test/suites/"
ROOT_HEADERS = ("app.h", "app_arena.h", "build_variant.h")
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
VENDOR = re.compile(r"^(esp_|freertos/|driver/|hal/|soc/|rom/|bsp/|nvs|sdkconfig|xtensa)")
LAYER_LABEL = re.compile(r'^\s*\w+\["(\w+)/<br/><i>([^<]+)</i>"\](:::hw)?', re.M)


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True, check=True).stdout


def component_of(rel):
    if rel.startswith(SUITES):
        return "suites"
    parts = rel[len(MAIN):].split("/")
    if parts[0] == "apps":
        return "apps/" + parts[1]
    if len(parts) == 1:
        return "contract" if parts[0].split(".")[0] in ("app", "app_arena", "build_variant") else "shell"
    return parts[0]


def summary(text):
    """The first sentence of a file's top comment, without its `name:` lead."""
    block = re.match(r"\s*/\*(.*?)\*/", text, re.S)
    if block:
        body = " ".join(line.strip().lstrip("*").strip() for line in block.group(1).splitlines())
    else:
        lines = re.match(r"\s*((?://[^\n]*\n)+)", text)
        if not lines:
            return ""
        body = " ".join(line.strip()[2:].strip() for line in lines.group(1).splitlines())
    body = re.sub(r"\s+", " ", body).strip()
    if "GENERATED FILE" in body[:200]:
        return "Generated file."
    body = re.sub(r"^[\w./*-]+(\s*\([^)]*\))?\s*[:,—-]\s*", "", body)
    sentence = re.split(r"(?<=\.)\s", body, maxsplit=1)[0]
    if len(sentence) > 200:
        sentence = sentence[:197].rsplit(" ", 1)[0] + "..."
    return sentence[:1].upper() + sentence[1:]


def resolve(inc, rel, layers):
    """What an #include reaches: ("ext", name), ("comp", id) or ("file", path)."""
    if inc.startswith("microui"):
        return ("ext", "microui")
    if VENDOR.match(inc):
        return ("ext", "ESP-IDF")
    head = inc.split("/")[0]
    if head in layers:
        return ("file", MAIN + inc) if (ROOT / MAIN / inc).exists() else ("comp", head)
    if head == "apps" and inc.count("/") >= 2:
        return ("file", MAIN + inc) if (ROOT / MAIN / inc).exists() else ("comp", "apps/" + inc.split("/")[1])
    if inc in ROOT_HEADERS:
        return ("file", MAIN + inc)
    here = (ROOT / rel).parent / inc
    if here.exists():
        return ("file", os.path.relpath(here.resolve(), ROOT))
    if rel.startswith(MAIN + "apps/"):
        app_dir = ROOT / MAIN / "apps" / rel.split("/")[3]
        found = sorted(app_dir.rglob(os.path.basename(inc)))
        if found:
            return ("file", os.path.relpath(found[0], ROOT))
    return None


def build_data():
    doc = (ROOT / "docs/Firmware-Architecture.md").read_text(encoding="utf-8")
    described = {m.group(1): (m.group(2), bool(m.group(3))) for m in LAYER_LABEL.finditer(doc)}
    layers = [d for d in described if d != "apps"]
    paths = [p for p in git("ls-files", MAIN, SUITES).split() if p.endswith((".c", ".h"))]

    comps = collections.OrderedDict()
    edges = collections.Counter()
    file_edges = collections.defaultdict(collections.Counter)
    for rel in sorted(paths):
        comp = component_of(rel)
        text = (ROOT / rel).read_text(encoding="utf-8", errors="replace")
        comps.setdefault(comp, []).append({"path": rel, "name": os.path.basename(rel),
                                           "lines": text.count("\n"), "summary": summary(text)})
        for inc in INCLUDE.findall(text):
            target = resolve(inc, rel, layers)
            if target is None:
                continue
            kind, value = target
            if kind == "ext":
                edges[(comp, "ext:" + value)] += 1
                file_edges[comp][(rel, "@ext:" + value)] += 1
                continue
            other = component_of(value) if kind == "file" else value
            if other == comp:
                if kind == "file":
                    file_edges[comp][(rel, value)] += 1
            else:
                edges[(comp, other)] += 1
                file_edges[comp][(rel, "@" + other)] += 1

    for files in comps.values():
        by_name = {f["name"]: f for f in files}
        for f in files:
            header = by_name.get(f["name"][:-2] + ".h") if f["name"].endswith(".c") else None
            if not f["summary"] and header and header["summary"]:
                f["summary"], f["fromHeader"] = header["summary"], True

    def describe(comp, files):
        if comp in described:
            return comp, described[comp][0] + ".", described[comp][1]
        if comp.startswith("apps/"):
            entry = next((f for f in files if re.match(r"app_\w+\.c$", f["name"])), None)
            return comp.split("/")[1], entry["summary"] if entry else "", False
        if comp == "shell":
            return "Shell", next((f["summary"] for f in files if f["name"] == "main.c"), ""), False
        if comp == "contract":
            return "Root headers", "app.h, the shell/app contract, with the app arena and build_variant.h: any layer may include them.", False
        return "engine suites", "Unity suites for the engine, run on the host and again on the board.", False

    components = []
    for comp, files in comps.items():
        name, desc, hw = describe(comp, files)
        components.append({"id": comp, "name": name, "desc": desc, "hw": hw, "files": files,
                           "lines": sum(f["lines"] for f in files)})
    return {
        "components": components,
        "edges": [{"from": a, "to": b, "n": n} for (a, b), n in sorted(edges.items())],
        "fileEdges": {c: [{"from": a, "to": b, "n": n} for (a, b), n in sorted(e.items())]
                      for c, e in file_edges.items()},
        "source": {"commit": git("rev-parse", "--short", "HEAD").strip(),
                   "date": git("log", "-1", "--format=%cs").strip()},
    }


def embed(data):
    """`data` as a script literal: compact JSON in which no "</" can end the
    <script> that holds it."""
    return json.dumps(data, separators=(",", ":")).replace("</", "<\\/")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-o", "--out", default=str(ROOT / "build/architecture"))
    args = parser.parse_args(argv)
    data = embed(build_data())
    page = (pathlib.Path(__file__).with_name("template.html")).read_text(encoding="utf-8")
    if page.count("__DATA__") != 1:
        sys.exit("template.html must hold __DATA__ exactly once")
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "index.html").write_text(page.replace("__DATA__", data), encoding="utf-8")
    print(f"wrote {out / 'index.html'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
