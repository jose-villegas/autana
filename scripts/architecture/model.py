"""The C4 model of a C or C++ tree, read from its tracked files: the boxes
around the system, one component per source folder, one module per source
and its header, the includes between them, and the words for each box.

Everything here takes the tree and its layout as arguments; draw.py reads
them from a project file, and a caller with more to say (file tags,
packages, deployment) adds to the data before likec4.emit() writes it.

  * a box whose words came from a file links to it. A file's words are its
    opening: a Markdown file's first prose paragraph, a C or C++ file's top
    comment, a Python module's docstring, else a script's leading comment,
    without a `name:` lead. A summary is its first sentence, cut at a word to
    150 characters;
  * every source folder under a scan root is a component, nested by its
    folder tree; files directly in the root form a component named for it.
    describe() gives the order a folder's words are looked for in;
  * a module is a source and its header: a source pairs with a same-stem
    header it directly includes in the same component, unless the header has
    a source of its own. Every module needs words;
  * each tracked docs/**/*.md heading section belongs to the deepest
    component or module holding more than half its resolved references
    (assign_docs).
"""
from fractions import Fraction
import os
import pathlib
import re
import subprocess
import sys

# Keep bytecode out of the checkout the tool runs from.
sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "gates"))
from c_comments import SOURCE_EXTENSIONS, file_header  # noqa: E402
from check_doc_index import heading_anchors, slugify  # noqa: E402
from check_generated_files import is_generated  # noqa: E402
from tracked import tracked_files  # noqa: E402

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
RUN_TEST = re.compile(r"\bRUN_TEST\((\w+)\)")
# Summaries are cut here to fit LikeC4's summary field.
SUMMARY_LENGTH = 150


class Undescribed(Exception):
    """A box the tree no longer backs, or one with nothing to read its words from."""


class Tree:
    """The git checkout a model is read from."""

    def __init__(self, root):
        self.root = pathlib.Path(root)

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.root), *args],
                              capture_output=True, text=True, check=True).stdout

    def files(self, *paths):
        """Tracked paths, under `paths` when given."""
        return list(tracked_files(self.root, paths))

    def read(self, rel):
        return (self.root / rel).read_text(encoding="utf-8", errors="replace")

    def published_commit(self):
        """The commit the page's links point at: the newest one HEAD holds that
        the remote's default branch (origin/HEAD, else origin/main) has too. A
        build on a merge made only in the runner would otherwise link to a
        commit GitHub never saw, and every source link would 404."""
        for ref in ("origin/HEAD", "origin/main"):
            if subprocess.run(["git", "-C", str(self.root), "rev-parse", "--verify", "-q", ref],
                              capture_output=True).returncode == 0:
                return self.git("merge-base", "HEAD", ref).strip()
        return self.git("rev-parse", "HEAD").strip()

    def repository(self):
        """The GitHub page of `origin`, or "" without one."""
        found = subprocess.run(["git", "-C", str(self.root), "remote", "get-url", "origin"],
                               capture_output=True, text=True)
        url = found.stdout.strip() if found.returncode == 0 else ""
        m = re.search(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?$", url)
        return f"https://github.com/{m.group(1)}" if m else ""

    def source(self):
        """What every link names: the repository and the commit, with its date."""
        commit = self.published_commit()
        return {"commit": commit, "date": self.git("log", "-1", "--format=%cs", commit).strip(),
                "repo": self.repository()}


def summary(path, text=None):
    """The opening's first sentence, bounded for LikeC4's summary field."""
    body = opening(path, text) if text is not None else path
    sentence = re.split(r"(?<=\.)\s", body, maxsplit=1)[0]
    if len(sentence) <= SUMMARY_LENGTH:
        return sentence
    return sentence[:SUMMARY_LENGTH - 3].rsplit(" ", 1)[0] + "..."


def paragraph(markdown):
    """The first prose paragraph of a Markdown file, links reduced to their text."""
    markdown = re.sub(r"^\s*(`{3,}|~{3,})[^\n]*\n.*?^\s*\1\s*$", "", markdown, flags=re.M | re.S)
    for block in re.split(r"\n\s*\n", markdown):
        block = block.strip()
        if block and not block.startswith(("#", "[!", "<", "|", "```", "!")):
            text = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", " ".join(block.split()))
            return text.replace("**", "")
    return ""


def opening(rel, text):
    """The first paragraph a file opens with, without a `name:` or `name -`
    lead: a Markdown file's first paragraph whole, a C or C++ file's top
    comment, a Python module's docstring, else a script's leading # comment."""
    if rel.endswith(".md"):
        return paragraph(text)
    if rel.endswith(SOURCE_EXTENSIONS):
        if is_generated(text):
            return "Generated file."
        header = file_header(rel, text)
        if header is None:
            tests = RUN_TEST.findall(text) if pathlib.PurePosixPath(rel).name.startswith("suite_") else []
            return ("; ".join(t.removeprefix("test_").replace("_", " ") for t in tests) + ".").capitalize() if tests else ""
        body = re.split(r"\n\s*\n", header.text, maxsplit=1)[0]
    elif rel.endswith(".py"):
        found = re.search(r'^(?:#[^\n]*\n|\s)*"""(.*?)(?:\n\s*\n|""")', text, re.S)
        body = found.group(1) if found else ""
    else:
        lines = []
        for line in text.splitlines()[1 if text.startswith("#!") else 0:]:
            if not line.startswith("#"):
                break
            lines.append(line.lstrip("#").strip())
        body = re.split(r"\n\s*\n", "\n".join(lines).strip(), maxsplit=1)[0]
    body = re.sub(r"^[\w./*-]+(\s*\([^)]*\))?\s*[:,—-]\s*", "", " ".join(body.split()))
    return body[:1].upper() + body[1:]


def identifier(prefix, name):
    return prefix + re.sub(r'[^A-Za-z0-9_]', '_', name)


def readme_system(tree):
    """The system box: the README's title and opening paragraph."""
    readme = tree.read("README.md")
    title = re.search(r"^#\s+(.+)$", readme, re.M)
    if not title or not paragraph(readme):
        raise Undescribed("README.md names the system: open it with a # title and a paragraph")
    return {"name": title.group(1).strip(), "desc": paragraph(readme), "src": "README.md"}


def outside_systems(tracked, systems):
    """Software systems around this one, each {id, name, description,
    folder} (folder: what it stands for, or null); a folder the tree no
    longer holds fails. A submodule's folder is tracked as the one path."""
    nodes = []
    for s in systems:
        src = s["folder"]
        if src and not any(p == src or p.startswith(src + "/") for p in tracked):
            raise Undescribed(f"{s['name']} stands for {src}/, which the tree no longer holds")
        nodes.append({"id": s["id"], "name": s["name"], "type": "ext", "tech": "Software system",
                      "desc": s["description"], "src": src})
    return nodes


def context(tree, tracked, system, people, outside, containers, relations, detail):
    """Levels 1 and 2: their boxes and what flows between them. `people` are
    {id, name, description}; `containers` {id, name, technology, file whose
    opening describes it}; `relations` {from, to, label}; `detail` is the
    container whose components level 3 draws."""
    people = [{"id": p["id"], "name": p["name"], "type": "person", "tech": "Person",
               "desc": p["description"], "src": None} for p in people]
    nodes = []
    for c in containers:
        src = c["file"]
        desc = opening(src, tree.read(src)) if src in tracked else ""
        if not desc:
            raise Undescribed(f"container {c['name']} reads its description from {src}: give it an opening sentence")
        nodes.append({"id": c["id"], "name": c["name"], "type": "container", "tech": c["technology"],
                      "desc": desc, "src": src, "drill": c["id"] if c["id"] == detail else None})
    if not any(n["drill"] for n in nodes):
        raise Undescribed(f"the detail container {detail} is not among the containers")
    ids = {b["id"] for b in people + outside + nodes}
    for r in relations:
        if r["from"] not in ids or r["to"] not in ids:
            raise Undescribed(f"relation {r['from']} -> {r['to']} names a box that does not exist")
    name = identifier("", system["name"].lower())
    top = dict(system, id=name if name[:1].isalpha() else "s_" + name, type="system",
               tech="Software system", drill="containers")
    return {
        "context": {"nodes": people + [top] + outside},
        "containers": {"nodes": people + nodes + outside,
                       "edges": [[r["from"], r["to"], r["label"]] for r in relations]},
    }


def kind(rel):
    """What a file is for, by where it sits and what it is called: "test" (a
    test/ or tests/ folder, a test_ or suite_ file, a _test or Test name),
    "tool" (a tools/ folder: host programs the product never links), or "src"."""
    path = pathlib.PurePosixPath(rel)
    folders = set(path.parts[:-1])
    if (folders & {"test", "tests"} or path.name.startswith(("test_", "suite_"))
            or path.stem.endswith(("_test", "Test"))):
        return "test"
    if "tools" in folders:
        return "tool"
    return "src"


def components_of(paths, scans, headers, packages=None, includes=()):
    """Return component folders, file ownership and parents, following the
    folder tree. A package (scan root -> name) gathers its public headers
    (relative to an include root inside it) and its sources (relative to
    src/) under one component tree."""
    folders, comp_of, parent_of, origins = {}, {}, {}, {}
    for rel in sorted(paths, key=lambda p: (not p.endswith(headers), p)):
        scan = next(s for s in scans if rel.startswith(s + "/"))
        package = (packages or {}).get(scan)
        base, prefix = scan, ()
        if package:
            prefix = (package,)
            base = next((inc for inc in sorted(includes, key=len, reverse=True)
                         if rel.endswith(headers) and inc.startswith(scan + '/') and rel.startswith(inc + '/')),
                        scan + '/src' if rel.startswith(scan + '/src/') else scan)
        dirs = pathlib.PurePosixPath(rel[len(base) + 1:]).parts[:-1]
        if package and base != scan and base != scan + '/src' and dirs[:1] == (package,):
            base, dirs = base + '/' + package, dirs[1:]
        nodes = [(package, scan)] if package else []
        nodes += [("/".join(prefix + dirs[:depth]), "/".join((base,) + dirs[:depth]))
                  for depth in range(1, len(dirs) + 1)]
        for index, (cid, folder) in enumerate(nodes or [(scan.rsplit('/', 1)[-1], scan)]):
            if cid in folders and folders[cid] != folder and not (package and origins[cid] == scan):
                raise Undescribed(f"component id {cid} collides: {folder} and {folders[cid]}")
            folders.setdefault(cid, folder)
            origins[cid] = scan
            parent_of[cid] = nodes[index - 1][0] if index else ""
        comp_of[rel] = cid
    return folders, comp_of, parent_of


def describe(tree, folder, tracked, headers, sources, required=True, doc=None):
    """(description, source path) for a component folder: the opening of the
    first of README.md, <name> with each header then each source suffix,
    *_<name> likewise inside the folder, then <name> beside it, that has
    one; else its doc section's words."""
    name = folder.rsplit("/", 1)[-1]
    parent = folder.rsplit("/", 1)[0]
    inside = sorted(p for p in tracked if p.rsplit("/", 1)[0] == folder)
    suffixes = headers + sources
    candidates = [f"{folder}/README.md"] + [f"{folder}/{name}{s}" for s in suffixes]
    candidates += [p for s in suffixes for p in inside if p.endswith(f"_{name}{s}")]
    candidates += [f"{parent}/{name}{s}" for s in suffixes]
    for rel in candidates:
        if rel in tracked:
            text = opening(rel, tree.read(rel))
            if text:
                return text, rel
    if doc:
        return doc['desc'] or paragraph(tree.read(doc['path'])), doc['path']
    if not required:
        return "", folder
    raise Undescribed(f"{folder}/ has no description: give it a README.md whose first paragraph "
                      f"says what the folder is for")


def resolve(inc, rel, tracked, includes):
    """The tracked file an #include reaches, or None: beside the including
    file first, then under each include root."""
    for base in (str(pathlib.PurePosixPath(rel).parent),) + tuple(includes):
        candidate = os.path.normpath(f"{base}/{inc}").replace("\\", "/")
        if candidate in tracked:
            return candidate
    return None


def doc_references(tree, tracked, includes, suffixes):
    """Resolved references grouped by their Markdown heading, in source order."""
    sections = []
    basenames = {}
    for source in sorted(p for p in tracked if p.endswith(suffixes)):
        basenames.setdefault(source.rsplit('/', 1)[-1], []).append(source)
    for path in sorted(p for p in tracked if p.startswith('docs/') and p.endswith('.md')):
        lines, section = tree.read(path).splitlines(), None
        headings = {index: (title, anchor) for index, title, anchor in heading_anchors(lines)}
        for index, line in enumerate(lines):
            heading = headings.get(index)
            if heading or section is None:
                stem = pathlib.PurePosixPath(path).stem
                title, anchor = heading or (stem, slugify(stem))
                section = dict(path=path, heading=title, anchor=anchor, refs=[], text=[])
                sections.append(section)
            if not heading:
                section['text'].append(line)
            references = [(m[1].split('#')[0], True) for m in re.finditer(r'\[[^\]]*\]\(([^)\s]+)(?:\s+[^)]*)?\)', line)]
            references += [(m[1], False) for m in re.finditer(r'(?<!`)`([^`\n]+)`(?!`)', line)]
            references += [(name, False) for name in INCLUDE.findall(line)]
            for name, link in references:
                if not link and '/' not in name:
                    matches = basenames.get(name, [])
                    target = matches[0] if len(matches) == 1 else None
                else:
                    target = resolve(name, path, tracked, () if link else tuple(includes) + ('.',))
                if target:
                    section['refs'].append(target)
        for section in (s for s in sections if s['path'] == path):
            section['desc'] = paragraph('\n'.join(section.pop('text')))
    return sections


def assign_docs(docs, components, files):
    """Rank owned sections per element, including direct modules for components."""
    folders = {c['id']: c['folder'] for c in components}
    elements = {cid: (folder.count('/') * 2, folder + '/', None)
                for cid, folder in folders.items()}
    for f in files:
        stem = f.get('module', f['path'].rsplit('.', 1)[0])
        if stem not in elements:
            folder = folders.get(f['comp'], f['path'].rpartition('/')[0])
            elements[stem] = (folder.count('/') * 2 + 1, None, set())
        elements[stem][2].add(f['path'])
    module_components = {f.get('module', f['path'].rsplit('.', 1)[0]): f['comp'] for f in files}
    chosen, ranks = {}, {}
    for index, doc in enumerate(docs):
        candidates = []
        for owner, (depth, prefix, paths) in elements.items():
            count = sum(ref.startswith(prefix) if prefix else ref in paths for ref in doc['refs'])
            if count * 2 > len(doc['refs']):
                candidates.append((depth, owner, count))
        if not candidates:
            continue
        _, owner, _ = max(candidates)
        recipients = [owner]
        if owner in module_components and module_components[owner] in folders:
            recipients.append(module_components[owner])
        for recipient in recipients:
            _, prefix, paths = elements[recipient]
            count = sum(ref.startswith(prefix) if prefix else ref in paths for ref in doc['refs'])
            rank = (-count, -Fraction(count, len(doc['refs'])), doc['path'], index)
            if recipient not in ranks or rank < ranks[recipient]:
                chosen[recipient], ranks[recipient] = doc, rank
    return chosen


def source_files(texts, comp_of, tracked, includes, vendor, vendored=()):
    """One record per source: where it sits, its words, the scanned files it
    includes and the outside headers matching `vendor`, each with the tracked
    file in `vendored` it names when exactly one does."""
    by_name = {}
    for p in vendored:
        by_name.setdefault(p.rsplit("/", 1)[-1], []).append(p)

    def named(e):
        found = [p for p in by_name.get(e.rsplit("/", 1)[-1], ()) if p.endswith("/" + e)]
        return found[0] if len(found) == 1 else None
    files = []
    for rel, text in texts.items():
        inc, ext = {}, {}
        for name in INCLUDE.findall(text):
            target = resolve(name, rel, tracked, includes)
            if target in comp_of and target != rel:
                inc[target] = inc.get(target, 0) + 1
            elif target is None and vendor.match(name):
                ext[name] = ext.get(name, 0) + 1
        files.append({"path": rel, "comp": comp_of[rel], "kind": kind(rel), "lines": text.count("\n"),
                      "summary": summary(rel, text), "desc": opening(rel, text),
                      **({"gen": True} if is_generated(text) else {}),
                      "inc": [[t, n] for t, n in sorted(inc.items())],
                      "ext": [[e, n, named(e)]
                              for e, n in sorted(ext.items())]})
    return files


def pair_modules(files, comp_of, headers, sources):
    """Give each source its module: the same-stem header it directly includes
    in its own component, unless that header has a source of its own. Every
    module needs words."""
    paths = {f['path'] for f in files}
    for f in files:
        rel = f['path']
        if rel.endswith(sources):
            stem = pathlib.PurePosixPath(rel).stem
            found = []
            for target, _ in f['inc']:
                base = target.rsplit('.', 1)[0]
                own = {base + s for s in sources} & paths
                if (target.endswith(headers) and pathlib.PurePosixPath(target).stem == stem
                        and comp_of[target] == f['comp'] and (rel in own or not own)):
                    found.append(target)
            if len(found) > 1:
                raise Undescribed(f"{rel}: more than one directly included same-stem header: {found}")
            if found:
                f['module'] = found[0].rsplit('.', 1)[0]
    modules = {}
    for f in files:
        modules.setdefault(f.get('module', f["path"].rsplit(".", 1)[0]), []).append(f)
    for stem, group in sorted(modules.items()):
        if not any(f["summary"] for f in group):
            raise Undescribed(f"{group[0]['path']} has no description: open it with a top comment "
                              f"whose first sentence says what it is")
