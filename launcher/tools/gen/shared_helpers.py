"""Generate the shared-helper catalogue from declarations and their own prose."""
import argparse
import ast
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/gates"))
sys.path.insert(0, str(ROOT / "launcher/tools/render"))

from c_comments import blank_comments, file_header, scan
from check_doc_constants import sentences
from generated_blocks import replace_block
from idf_vocabulary import DECLARATION, FLOW, NOT_A_NAME, WORD, DEFINE
from tracked import tracked_files

PAGE = pathlib.Path("docs/Shared-Helpers.md")
DIRECTIVE_LINE = re.compile(r"^[ \t]*#[^\n]*", re.M)
INCLUDE = re.compile(r'^\s*#\s*include\s+([<"])([^>"\n]+)[>"]', re.M)


def c_helpers(path, text):
    """Declared functions and function-like macros with adjacent own comments."""
    comments = scan(path, text)
    header = file_header(path, text)
    code = blank_comments(text, "code")
    normalized = code.replace("##", "__")
    declarations = []
    for match in DEFINE.finditer(code):
        if match.group(2):
            declarations.append((match.group(1), match.start(), False))
    function_code = DIRECTIVE_LINE.sub(lambda m: " " * len(m[0]), normalized)
    for match in DECLARATION.finditer(function_code):
        prefix, name = match.groups()
        words = set(WORD.findall(prefix))
        if name in NOT_A_NAME or not FLOW.isdisjoint(words):
            continue
        if "static" in words and "inline" not in words:
            continue
        start = match.start() + len(prefix) - len(prefix.lstrip())
        name_start, name_end = match.span(2)
        declarations.append((text[name_start:name_end], start, "inline" in words))
    result = []
    for name, start, inline in sorted(declarations, key=lambda row: row[1]):
        own = next((comment for comment in reversed(comments)
                    if comment.spans[-1][1] <= start), None)
        prose = ""
        if own is not None and own is not header:
            gap = text[own.spans[-1][1]:start].replace("\\", "").strip()
            if not gap or (inline and re.fullmatch(r"#define\s+\w+\([^\n]*\)", gap)):
                prose = next(sentences(None, own.text), (0, ""))[1].strip()
        result.append((name, prose, start, inline))
    return result


def pure_gfx(path, texts, visiting=()):
    """Header-only arithmetic with scalar/math dependencies and no global state."""
    if path in visiting or path not in texts:
        return False
    text = texts[path]
    code = blank_comments(text, "code")
    if any(not inline for name, prose, start, inline in c_helpers(path, text)
           if not re.search(r"^\s*#define\s+" + re.escape(name) + r"\(", code, re.M)):
        return False
    if re.search(r"^[ \t]*static\s+(?!inline\b|const\b)[^;{]*[;=]", code, re.M):
        return False
    includes = INCLUDE.findall(blank_comments(text))
    if len(includes) != len(re.findall(r"^\s*#\s*include\b", code, re.M)):
        return False
    for delimiter, include in includes:
        if include.startswith(("util/scalar/", "util/math/")):
            continue
        if include.startswith("gfx/") and pure_gfx("launcher/main/" + include, texts, (*visiting, path)):
            continue
        if delimiter == "<" and "/" not in include and not include.startswith(("esp_", "sdkconfig")):
            continue
        return False
    return True


def catalogue(root):
    """Sorted source-owned helper rows and missing-description diagnostics."""
    root = pathlib.Path(root)
    paths = tracked_files(root, ("launcher/main/util/*.h", "launcher/main/gfx/*.h",
                                 "scripts/lib/*.py", "launcher/tools/device/*.py"))
    texts = {path: (root / path).read_text(encoding="utf-8") for path in paths}
    helpers = {}
    for path, text in texts.items():
        if path.endswith(".h"):
            if path.startswith("launcher/main/gfx/") and not pure_gfx(path, texts):
                continue
            declarations = [(name, prose, text.count("\n", 0, start) + 1)
                            for name, prose, start, _ in c_helpers(path, text)]
        elif pathlib.PurePosixPath(path).parent.as_posix() in ("scripts/lib", "launcher/tools/device"):
            tree = ast.parse(text, filename=path)
            declarations = []
            for node in tree.body:
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    prose = " ".join((ast.get_docstring(node) or "").split())
                    purpose = next(sentences(None, prose), (0, ""))[1].strip()
                    declarations.append((node.name, purpose, node.lineno))
        else:
            continue
        for name, prose, line in declarations:
            if not name.startswith("_"):
                key = (name, path)
                if key not in helpers or prose:
                    helpers[key] = (prose, line)
    rows = [(name, prose, path) for (name, path), (prose, _) in sorted(helpers.items())]
    errors = [f"{path}:{line}: {name}: missing own comment or docstring"
              for (name, path), (prose, line) in sorted(helpers.items()) if not prose]
    return rows, errors


def update(root, check=False):
    """Write the generated block, or fail on undocumented helpers and stale output."""
    root = pathlib.Path(root)
    rows, errors = catalogue(root)
    for error in errors:
        print(error)
    lines = ["| Helper | What it does | Owner |", "|---|---|---|"]
    for name, prose, path in rows:
        prose = prose or "**Missing own comment or docstring.**"
        prose = prose.replace("|", "&#124;").replace("<", "&lt;").replace(">", "&gt;")
        lines.append(f"| `{name}` | {prose} | [{path}](../{path}) |")
    changed = replace_block(root / PAGE, "shared-helpers", "\n".join(lines), check)
    print(f"shared helpers: {len(rows)} rows, {len(errors)} missing descriptions; "
          f"{'stale' if changed and check else 'current'}")
    return int(bool(errors) or (check and changed))


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
