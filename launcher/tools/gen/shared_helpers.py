"""Catalogue every tracked C/C++ header or Python module outside apps/, test/
and tests/ folders and c_comments.EXCLUDED paths that is included or imported
from more than one directory, using its banner sentence and public names.

    python launcher/tools/gen/shared_helpers.py [--check]
"""
import argparse
import ast
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/gates"))
sys.path.insert(0, str(ROOT / "launcher/tools/render"))

from c_comments import EXCLUDED, blank_comments, file_header
from check_style_audit import resolve_include
from check_doc_constants import sentences
from generated_blocks import replace_block
from idf_vocabulary import c_declarations
from tracked import tracked_files

PAGE = pathlib.Path("docs/Shared-Helpers.md")
INCLUDE = re.compile(r'^\s*#\s*include\s+([<"])([^>"\n]+)[>"]', re.M)


def catalogue(root):
    root = pathlib.Path(root).resolve()
    paths = tracked_files(root)
    texts = {path: (root / path).read_text(encoding="utf-8") for path in paths
             if pathlib.PurePosixPath(path).suffix in {".h", ".hh", ".hpp", ".hxx", ".c", ".cc", ".cpp", ".cxx", ".py"}}
    owners = {path for path, text in texts.items()
              if pathlib.PurePosixPath(path).suffix in {".h", ".hh", ".hpp", ".hxx", ".py"}
              and not {"apps", "test", "tests"}.intersection(pathlib.PurePosixPath(path).parts)
              and not path.startswith(EXCLUDED)}
    users = {path: set() for path in owners}
    modules = {path[:-3].replace("/", "."): path for path in owners if path.endswith(".py")}
    for path, text in texts.items():
        source = pathlib.PurePosixPath(path)
        references = []
        if source.suffix == ".py":
            for node in ast.walk(ast.parse(text, filename=path)):
                if isinstance(node, ast.Import):
                    references.extend(alias.name for alias in node.names)
                elif isinstance(node, ast.ImportFrom):
                    prefix = ".".join(source.parts[:-node.level]) if node.level else ""
                    module = ".".join(part for part in (prefix, node.module) if part)
                    references.append(module)
                    references.extend(module + "." + alias.name for alias in node.names)
            for module in references:
                local = ".".join((*source.parts[:-1], module))
                matches = [modules[local]] if local in modules else [owner for name, owner in modules.items()
                           if name == module or name.endswith("." + module)]
                if len(matches) == 1:
                    users[matches[0]].add(source.parent.as_posix())
        else:
            for _, include in INCLUDE.findall(blank_comments(text)):
                resolved = resolve_include(root, root / path, include)
                if resolved is None:
                    matches = [owner for owner in owners if owner == include or owner.endswith("/" + include)]
                else:
                    matches = [resolved.relative_to(root).as_posix()] if resolved.is_relative_to(root) else []
                if len(matches) == 1 and matches[0] in users:
                    users[matches[0]].add(source.parent.as_posix())
    rows = []
    for path in sorted(owners):
        if len(users[path]) < 2:
            continue
        text = texts[path]
        if path.endswith(".py"):
            tree = ast.parse(text, filename=path)
            prose = ast.get_docstring(tree) or ""
            names = set()
            for node in tree.body:
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                    names.add(node.name)
                elif isinstance(node, (ast.Assign, ast.AnnAssign)):
                    targets = node.targets if isinstance(node, ast.Assign) else [node.target]
                    names.update(child.id for target in targets for child in ast.walk(target)
                                 if isinstance(child, ast.Name))
            names = sorted(name for name in names if not name.startswith("_"))
        else:
            header = file_header(path, text)
            prose = header.text if header else ""
            pasted = {name.replace("##", "__"): name
                      for name in re.findall(r"\b\w+(?:##\w+)+", blank_comments(text, "code"))}
            declarations = c_declarations(text.replace("##", "__").replace("\\\n", "\n"))
            declarations[2].update(re.findall(r"\bclass\s+([A-Za-z_]\w*)", blank_comments(text, "code")))
            guards = {name for name in re.findall(
                r"^\s*#\s*ifndef\s+(\w+)\s*\n\s*#\s*define\s+\1[ \t]*$",
                blank_comments(text), re.M)
                if ("_" + re.sub(r"\W", "_", path).upper()).endswith("_" + name)}
            names = sorted(pasted.get(name, name) for name in set().union(*declarations)
                           if not name.startswith("_") and name not in guards)
        purpose = next(sentences(None, prose), (0, ""))[1].strip()
        rows.append((path, " ".join(purpose.split()), ", ".join(names)))
    return rows


def update(root, check=False):
    root = pathlib.Path(root)
    rows = catalogue(root)
    lines = ["| Owner | Purpose | Public names |", "|---|---|---|"]
    for path, prose, names in rows:
        prose = prose.replace("|", "&#124;").replace("<", "&lt;").replace(">", "&gt;")
        lines.append(f"| [{path}](../{path}) | {prose} | `{names}` |")
    changed = replace_block(root / PAGE, "shared-helpers", "\n".join(lines), check)
    print(f"shared helpers: {len(rows)} owner files; {'stale' if changed and check else 'current'}")
    return int(check and changed)


def main():
    """The catalogue command, rooted in this checkout unless a fixture is supplied."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        return update(args.root, args.check)
    except (ValueError, OSError, SyntaxError) as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
