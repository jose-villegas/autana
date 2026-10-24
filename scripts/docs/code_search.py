#!/usr/bin/env python3
"""Find what already does a job before writing it again: the public names and purposes
of every tracked header and Python module, apart from vendored and generated files,
indexed afresh on every run. docs/tools/Code-Search.md has the matching and order.

    python scripts/docs/code_search.py <words...>            best names, then purposes
    python scripts/docs/code_search.py --shared <words...>   only files used from 2+ directories
    python scripts/docs/code_search.py --check               every indexed file states a purpose

Exits 1 when nothing matches.
"""
import argparse
import ast
import math
import pathlib
import posixpath
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts/gates"))
sys.path.insert(0, str(ROOT / "launcher/tools"))

from build.packages import include_roots
from c_comments import excluded, blank_comments, file_header
from c_constants import file_scope
from c_includes import resolve_include
from check_doc_constants import sentences
from docs_search import CAMEL, STOPWORDS, WORD, stem, tokens
from idf_vocabulary import c_declarations
from tracked import tracked_files

HEADERS = {".h", ".hh", ".hpp", ".hxx"}
SOURCES = HEADERS | {".c", ".cc", ".cpp", ".cxx", ".py"}
INCLUDE = re.compile(r'^\s*#\s*include\s+[<"]([^>"\n]+)[>"]', re.M)
# Directories that make a file a shared owner, the set --shared keeps.
SHARED_DIRECTORIES = 2
# Shortest stem that matches the start of a name: `lens` stems to `len`, which would match every `*_LEN`.
STEM_MIN = 4
# Rows one file shows before the next file's; the rest go after every file's first rows.
NAMES_PER_FILE = 3
# How many times a word counts when a public name matches it rather than the file's prose.
NAME_WEIGHT = 2
# Results printed before the count of the rest.
SHOWN = 20
# Longest declaration printed, in characters.
DECLARATION_MAX = 160


class Owner:
    """One indexed file: its one-line purpose, its words, the directories that use it and its public
    names as (name, line, declaration)."""

    def __init__(self, path, prose, names):
        self.path, self.names, self.users = path, names, set()
        self.purpose = " ".join(next(sentences(None, prose), (0, ""))[1].split())
        self.words = set(tokens(prose + " " + path.replace("/", " ").replace(".", " ")))


def statement_start(code, end):
    """Where the C statement ending at `end` begins: after the previous `;`, `{` or `}`, and past
    any directive line in between."""
    begin = max(code.rfind(mark, 0, end) for mark in ";{}") + 1
    directives = [line.end() for line in re.finditer(r"^[ \t]*#.*$", code[begin:end], re.M)]
    return begin + max(directives, default=0)


def declaration(code, start):
    """The C declaration naming the identifier at `start`, on one line: a directive's line, an
    enum value's line, or the statement around it, a type's braces shown as `{ ... }`."""
    end = code.find("\n", start)
    line = code[code.rfind("\n", 0, start) + 1:end if end >= 0 else len(code)]
    begin = statement_start(code, start)
    if line.lstrip().startswith("#") or code[begin - 1:begin] == "{":
        text = line
    else:
        ends = [at for at in (code.find(";", start), code.find("{", start)) if at >= 0]
        text = code[begin:min(ends, default=len(code))]
        if code[begin - 1:begin] == "}" and "\n" not in code[begin:start]:  # `} name_t;` closes a type
            depth, at = 0, begin - 1
            while at > 0 and (depth := depth + {"}": 1, "{": -1}.get(code[at], 0)):
                at -= 1
            text = code[statement_start(code, at):at] + "{ ... }" + text
    return clip(text)


def clip(text):
    """`text` on one line, cut to DECLARATION_MAX characters."""
    text = " ".join(text.split())
    return text if len(text) <= DECLARATION_MAX else text[:DECLARATION_MAX - 3] + "..."


def c_owner(path, text, comments):
    """`comments` is `text` with its comments blanked."""
    header = file_header(path, text)
    spelled = blank_comments(text, "code")
    pasted = {name.replace("##", "__"): name for name in re.findall(r"\b\w+(?:##\w+)+", spelled)}
    code = blank_comments(text.replace("##", "__").replace("\\\n", "\n"), "code")
    declarations = c_declarations(file_scope(code))
    declarations[2].update(re.findall(r"\bclass\s+([A-Za-z_]\w*)", code))
    guards = {name for name in re.findall(r"^\s*#\s*ifndef\s+(\w+)\s*\n\s*#\s*define\s+\1[ \t]*$", comments, re.M)
              if ("_" + re.sub(r"\W", "_", path).upper()).endswith("_" + name)}
    names = []
    for name in sorted(pasted.get(name, name) for name in set().union(*declarations)
                       if not name.startswith("_") and name not in guards):
        found = re.search(rf"(?<!\w){re.escape(name)}(?!\w)", spelled)
        at = found.start() if found else 0
        names.append((name, spelled.count("\n", 0, at) + 1, declaration(spelled, at) if found else name))
    return Owner(path, header.text if header else "", names)


def python_owner(path, text, tree):
    names = []
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            names.append((node.name, node.lineno, f"def {node.name}({ast.unparse(node.args)})"))
        elif isinstance(node, ast.ClassDef):
            bases = ", ".join(ast.unparse(base) for base in node.bases)
            names.append((node.name, node.lineno, f"class {node.name}" + (f"({bases})" if bases else "")))
        elif isinstance(node, (ast.Assign, ast.AnnAssign)):
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            line = clip(ast.get_source_segment(text, node))
            names.extend((child.id, node.lineno, line) for target in targets
                         for child in ast.walk(target) if isinstance(child, ast.Name))
    first = {}
    for entry in sorted(names, key=lambda entry: entry[1]):
        first.setdefault(entry[0], entry)
    names = sorted(entry for name, entry in first.items() if not name.startswith("_"))
    return Owner(path, ast.get_docstring(tree) or "", names)


def describe(path, text):
    """One header or module as an Owner, without the directories that use it."""
    if path.endswith(".py"):
        return python_owner(path, text, ast.parse(text, filename=path))
    return c_owner(path, text, blank_comments(text))


def statements(nodes):
    """Every statement, nested ones too, without descending into expressions."""
    for node in nodes:
        yield node
        for field in ("body", "orelse", "finalbody", "handlers", "cases"):
            inner = getattr(node, field, ())
            if isinstance(inner, list):
                yield from statements(inner)


def imports(source, tree):
    """The dotted names a module imports, each `from` name also as module.name."""
    for node in statements(tree.body):
        if isinstance(node, ast.Import):
            yield from (alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            prefix = ".".join(source.parts[:-node.level]) if node.level else ""
            module = ".".join(part for part in (prefix, node.module) if part)
            yield module
            yield from (module + "." + alias.name for alias in node.names)


def index(root):
    """Every indexed file as an Owner, with the directories that include or import it."""
    root = pathlib.Path(root).resolve()
    paths = [path for path in tracked_files(root) if pathlib.PurePosixPath(path).suffix in SOURCES]
    tracked, skipped = set(paths), excluded()
    owners, references = {}, {}
    for path in paths:
        source = pathlib.PurePosixPath(path)
        text = (root / path).read_text(encoding="utf-8")
        indexed = source.suffix in HEADERS | {".py"} and not path.startswith(skipped)
        if source.suffix == ".py":
            tree = ast.parse(text, filename=path)
            references[path] = list(imports(source, tree))
            if indexed:
                owners[path] = python_owner(path, text, tree)
        elif indexed:
            comments = blank_comments(text)
            references[path] = INCLUDE.findall(comments)
            owners[path] = c_owner(path, text, comments)
        else:
            # Only the text up to the last include line needs its comments blanked.
            last = [match.end() for match in INCLUDE.finditer(text)][-1:]
            references[path] = INCLUDE.findall(blank_comments(text[:last[0]])) if last else []
    by_tail = {}
    for path in owners:
        parts = path.removesuffix(".py").split("/")
        for at in range(len(parts)):
            tail = ("." if path.endswith(".py") else "/").join(parts[at:])
            by_tail.setdefault(tail, []).append(path)
    roots = tuple(path.relative_to(root).as_posix() for path in include_roots(root / "launcher"))
    for path, names in references.items():
        source = pathlib.PurePosixPath(path)
        for name in names:
            if source.suffix == ".py":
                local = "/".join((*source.parts[:-1], *name.split("."))) + ".py"
                matches = [local] if local in owners else by_tail.get(name, [])
            else:
                resolved = resolve_include(path, name, roots, tracked.__contains__)
                matches = [resolved] if resolved else by_tail.get(posixpath.normpath(name), [])
            if len(matches) == 1 and matches[0] in owners and matches[0].endswith(".py") == (source.suffix == ".py"):
                owners[matches[0]].users.add(source.parent.as_posix())
    return [owners[path] for path in sorted(owners)]


def query_terms(words):
    """(word, stem) for each word of a query and each part of an identifier in it, as
    docs_search.tokens() splits them."""
    terms = {}
    for word in WORD.findall(" ".join(words)):
        parts = [part for chunk in word.split("_") for part in CAMEL.findall(chunk)]
        for raw in [word.lower()] + ([part.lower() for part in parts] if len(parts) > 1 else []):
            if len(raw) > 1 and raw not in STOPWORDS:
                terms.setdefault(stem(raw), raw)
    return [(raw, stemmed) for stemmed, raw in terms.items()]


def name_hit(term, name):
    """A public name starts with the word, or with its stem when the stem is long enough to
    mean the same thing, either whole or at one of its parts."""
    raw, stemmed = term
    starts = (raw, stemmed) if len(stemmed) >= STEM_MIN else (raw,)
    parts = [name.lower()] + [part.lower() for chunk in name.split("_") for part in CAMEL.findall(chunk)]
    return any(part.startswith(starts) for part in parts)


def search(owners, words, shared=False):
    """(path, line, name, declaration, purpose) rows, best first. A matched word scores its
    rarity, log(files / files it matches), counted NAME_WEIGHT times when the name matches it;
    ties go to files used from two or more directories, then path and line."""
    terms = query_terms(words)
    candidates, seen = [], {term: 0 for term in terms}
    for owner in owners:
        in_file = [stemmed in owner.words for _, stemmed in terms]
        named = [(entry, [name_hit(term, entry[0]) for term in terms]) for entry in owner.names]
        named = [(entry, hits) for entry, hits in named if any(hits)]
        for at, term in enumerate(terms):
            seen[term] += in_file[at] or any(hits[at] for _, hits in named)
        candidates.append((owner, in_file, named))
    rarity = {term: math.log((len(owners) + 1) / (count + 1)) for term, count in seen.items()}
    ranked = []
    for owner, in_file, named in candidates:
        is_shared = len(owner.users) >= SHARED_DIRECTORIES
        if shared and not is_shared:
            continue
        for (name, line, text), hits in named:
            score = sum(rarity[term] * (NAME_WEIGHT if hit else found)
                        for term, hit, found in zip(terms, hits, in_file))
            ranked.append(((-score, not is_shared, owner.path, line), (owner.path, line, name, text, owner.purpose)))
        if not named and any(in_file):
            score = sum(rarity[term] for term, found in zip(terms, in_file) if found)
            ranked.append(((-score, not is_shared, owner.path, 1),
                           (owner.path, 1, pathlib.PurePosixPath(owner.path).name,
                            f"{len(owner.names)} public names", owner.purpose)))
    shown, rest, per_file = [], [], {}
    for _, row in sorted(ranked):
        per_file[row[0]] = per_file.get(row[0], 0) + 1
        (shown if per_file[row[0]] <= NAMES_PER_FILE else rest).append(row)
    return shown + rest


def report(rows, shown=SHOWN):
    lines = []
    for path, line, name, text, purpose in rows[:shown]:
        lines += [f"{path}:{line}  {name}", f"    {text}", f"    {purpose}"]
    if len(rows) > shown:
        lines.append(f"{len(rows) - shown} more; add a word to narrow.")
    return "\n".join(lines)


def unexplained(owners):
    """Indexed files with no purpose sentence."""
    return [owner.path for owner in owners if not owner.purpose]


def main(argv=None, root=ROOT):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("words", nargs="*")
    parser.add_argument("--shared", action="store_true", help="only files used from two or more directories")
    parser.add_argument("--check", action="store_true", help="fail when an indexed file states no purpose")
    args = parser.parse_args(argv)
    if not args.check and not args.words:
        parser.error("give words to search for, or --check")
    owners = index(root)
    if args.check:
        missing = unexplained(owners)
        for path in missing:
            print(f"{path}: no purpose; open it with a banner comment or docstring that says what it is for")
        print(f"code search: {len(owners)} files indexed; {len(missing)} without a purpose")
        return int(bool(missing))
    rows = search(owners, args.words, args.shared)
    print(report(rows) if rows else "no match")
    return 0 if rows else 1


if __name__ == "__main__":
    raise SystemExit(main())
