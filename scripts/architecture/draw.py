#!/usr/bin/env python3
"""Write a C or C++ project's C4 model folder from a project file.

    python scripts/architecture/draw.py --project <file.json> [--root <checkout>] -o <dir>

The checkout (default: the current folder) is read through git: only
tracked files count. site.py turns the written folder into the explorer.

The project file names what the code cannot say (the people, outside
systems, containers and what flows between them) and where the sources lie;
docs/tools/Architecture-Diagram.md lists its keys, every one required.

The system box is the README's title and opening paragraph.
"""
import argparse
import json
import pathlib
import re
import sys

import likec4
import model

KEYS = ("people", "systems", "containers", "relations", "detail", "scan", "include_roots",
        "headers", "sources", "vendor_includes")


def load(path):
    project = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    missing = [key for key in KEYS if key not in project]
    unknown = sorted(set(project) - set(KEYS))
    if missing or unknown:
        raise model.Undescribed(f"{path}: missing {missing}, unknown {unknown}")
    return project


def build_data(tree, project, packages=None, vendored=(), outside=()):
    """The model data for likec4.emit(). A caller that knows more passes
    `packages` (scan root -> name: a folder drawn as a container of its own,
    whose words are required; its headers sit under an include root inside
    it, its sources under src/), `vendored` (tracked files an outside include
    may name) and `outside` (more outside-system boxes), then adds to the
    result."""
    headers, sources = tuple(project["headers"]), tuple(project["sources"])
    scans, includes = tuple(project["scan"]), tuple(project["include_roots"])
    packages = packages or {}
    tracked = set(tree.files())
    paths = sorted(p for p in tree.files(*scans) if p.endswith(headers + sources))
    if not paths:
        raise model.Undescribed(f"no {headers + sources} source is tracked under {list(scans)}")
    folders, comp_of, parent_of = model.components_of(paths, scans, headers, packages, includes)
    texts = {rel: tree.read(rel) for rel in paths}
    files = model.source_files(texts, comp_of, tracked, includes, re.compile(project["vendor_includes"]), vendored)
    model.pair_modules(files, comp_of, headers, sources)
    docs = model.doc_references(tree, tracked, includes, headers + sources)
    doc_links = model.assign_docs(docs, [dict(id=cid, folder=folder) for cid, folder in folders.items()], files)
    components = []
    for cid, folder in sorted(folders.items()):
        desc, src = model.describe(tree, folder, tracked, headers, sources, required=folder in packages,
                                   doc=doc_links.get(cid))
        package = next((name for root, name in packages.items() if folder == root or folder.startswith(root + "/")), None)
        components.append({**({"package": package} if package else {}), "id": cid, "folder": folder,
                           "desc": desc, "src": src, "parent": parent_of[cid]})
    package_nodes = [{"id": c["id"], "folder": c["folder"], "desc": c["desc"], "src": c["src"]}
                     for c in components if c["id"] in packages.values() and c["folder"] in packages]
    system = model.readme_system(tree)
    levels = model.context(tree, tracked, system, project["people"],
                           list(outside) + model.outside_systems(tracked, project["systems"]),
                           project["containers"], project["relations"], project["detail"])
    return {"system": system, **levels, "doc_links": doc_links, "components": components,
            "packages": package_nodes, "files": files, "source": tree.source()}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--project", required=True, type=pathlib.Path)
    parser.add_argument("--root", default=".", type=pathlib.Path)
    parser.add_argument("-o", "--out", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        data = build_data(model.Tree(args.root), load(args.project))
    except model.Undescribed as missing:
        sys.exit(f"draw.py: {missing}")
    print(f"wrote {likec4.write(args.out, data, likec4.emit(data))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
