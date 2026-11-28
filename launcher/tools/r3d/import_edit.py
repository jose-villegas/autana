"""Prints a mesh's import file as JSON for another program: its tables as written, each key's line, and whether
import_settings accepts it.

    python launcher/tools/r3d/import_edit.py read MESH.import.toml [--out OUT.json]

The JSON holds `file`, `valid`, `error` (import_settings' refusal, or null when it accepts the file), `keys` (those
before the first table header) and `tables`, in the order the file first names them. A table is
`{"name", "line", "keys"}`, each key `{"key", "value", "line"}`; its name is dotted (`geometry.simplify`) and its line
is its header's, or that of the key that opens it: an inline table (`simplify = { ... }`) or a dotted key
(`alpha_mask.keep_alpha = 0.5`). An array of tables (`[[variants]]`) is `{"name", "line", "array": true, "items"}`, each
item `{"line", "keys", "tables"}` with its tables named from the item. A table only implied by a longer name
(`objects` in `[[objects.meshes]]`) is not listed. A file that is not TOML reads as not valid, with no tables; a file
that cannot be read, or that is not a .import.toml, exits 1 and says why on stderr.
"""

import argparse
import json
import pathlib
import re
import sys
import tomllib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d import import_settings  # noqa: E402
from r3d.settings_cli import Refused, add_reader, emit, read_text  # noqa: E402

IMPORT_SUFFIX = ".import.toml"
BARE_KEY = re.compile(r"[A-Za-z0-9_-]+")
MULTILINE_QUOTES = ('"""', "'''")
BLANK = " \t\r"


def import_path(text):
    """`text` as a path, refused unless it names a .import.toml."""
    path = pathlib.Path(text)
    if not path.name.endswith(IMPORT_SUFFIX):
        raise Refused(f"{path}: only a {IMPORT_SUFFIX} is read")
    return path


def string_end(text, start):
    """One past the end of the TOML string opening at `start`."""
    for quote in MULTILINE_QUOTES:
        if text.startswith(quote, start):
            end = start + len(quote)
            while not text.startswith(quote, end):
                end += 2 if quote[0] == '"' and text[end] == "\\" else 1
            end += len(quote)
            while end < len(text) and text[end] == quote[0]:  # up to two quotes may end the string's content
                end += 1
            return end
    quote, end = text[start], start + 1
    while text[end] != quote:
        end += 2 if quote == '"' and text[end] == "\\" else 1
    return end + 1


def key_path(text, start):
    """The dotted key at `start` as its parts, and where it ends."""
    parts, at = [], start
    while True:
        while text[at] in BLANK:
            at += 1
        if text[at] in "\"'":
            end = string_end(text, at)
            parts.append(tomllib.loads(f"k = {text[at:end]}")["k"])
        else:
            end = BARE_KEY.match(text, at).end()
            parts.append(text[at:end])
        at = end
        while text[at] in BLANK:
            at += 1
        if text[at] != ".":
            return parts, at
        at += 1


def value_end(text, start, line):
    """Where the value at `start` ends (its line end, or the file's) and the line it ends on: a value spans lines
    only inside brackets or a multi-line string."""
    depth, at = 0, start
    while at < len(text):
        char = text[at]
        if char in "\"'":
            end = string_end(text, at)
            line += text.count("\n", at, end)
            at = end
            continue
        if char == "#":
            end = text.find("\n", at)
            at = len(text) if end < 0 else end
            continue
        if char == "\n":
            if depth == 0:
                return at, line
            line += 1
        depth += (char in "[{") - (char in "]}")
        at += 1
    return at, line


def statements(text):
    """Each header and key of the file in order, as (line, kind, key parts); kind is "table", "array" or "key"."""
    found, at, line = [], 0, 1
    while at < len(text):
        char = text[at]
        if char == "\n":
            line += 1
            at += 1
        elif char in BLANK:
            at += 1
        elif char == "#":
            end = text.find("\n", at)
            at = len(text) if end < 0 else end
        elif char == "[":
            array = text.startswith("[[", at)
            parts, at = key_path(text, at + 1 + array)
            at += 1 + array
            found.append((line, "array" if array else "table", parts))
        else:
            parts, at = key_path(text, at)
            found.append((line, "key", parts))
            at, line = value_end(text, at + 1, line)
    return found


class Container:
    """The file's top level or one item of an array of tables: its own keys and its tables, in file order."""

    def __init__(self, out):
        self.out = out
        self.tables = {}  # a table's key parts, as a tuple, to its entry in out["tables"]
        self.items = {}  # an array of tables' key parts to its items' Containers

    def table(self, parts, line):
        """The key list of the table `parts` names, listed at `line` when this is its first mention."""
        if not parts:
            return self.out["keys"]
        if tuple(parts) not in self.tables:
            self.tables[tuple(parts)] = dict(name=".".join(parts), line=line, keys=[])
            self.out["tables"].append(self.tables[tuple(parts)])
        return self.tables[tuple(parts)]["keys"]

    def item(self, parts, line):
        """A new item of the array of tables `parts` names, the array listed at `line` when this is its first."""
        if tuple(parts) not in self.items:
            self.tables[tuple(parts)] = dict(name=".".join(parts), line=line, array=True, items=[])
            self.out["tables"].append(self.tables[tuple(parts)])
            self.items[tuple(parts)] = []
        item = Container(dict(line=line, keys=[], tables=[]))
        self.tables[tuple(parts)]["items"].append(item.out)
        self.items[tuple(parts)].append(item)
        return item

    def add(self, parts, key, value, line):
        """`key` = `value` into the table `parts` names; an inline table becomes a table of its own."""
        if isinstance(value, dict):
            self.table(parts + [key], line)
            for name, held in value.items():
                self.add(parts + [key], name, held, line)
        else:
            self.table(parts, line).append(dict(key=key, value=value, line=line))

    def values(self):
        """The values this container's entries hold, as tomllib reads them; Refused on a key listed twice."""
        values = {key["key"]: key["value"] for key in self.out["keys"]}
        for parts, entry in self.tables.items():
            node = values
            for part in parts[:-1]:
                node = node.setdefault(part, {})
            if parts in self.items:
                node[parts[-1]] = [item.values() for item in self.items[parts]]
                continue
            held = node.setdefault(parts[-1], {})
            for key in entry["keys"]:
                if key["key"] in held:
                    raise Refused(f"{key['key']} at line {key['line']} is listed twice")
                held[key["key"]] = key["value"]
        if len(values) < len(self.out["keys"]):
            raise Refused("a top-level key is listed twice")
        return values


def written(text, values):
    """The file's top-level keys and tables, each key with its value from `values` (tomllib's reading of `text`)."""
    top = Container(dict(keys=[], tables=[]))

    def resolve(parts):
        """The container, the parts left within it and the values the header `parts` names: a name through an
        array of tables reaches into its last item."""
        container, inner, node = top, [], values
        for part in parts:
            node, inner = node[part], inner + [part]
            if isinstance(node, list):
                container, inner, node = container.items[tuple(inner)][-1], [], node[-1]
        return container, inner, node

    container, inner, node = top, [], values
    for line, kind, parts in statements(text):
        if kind == "table":
            container, inner, node = resolve(parts)
            container.table(inner, line)
        elif kind == "array":
            owner, inner, node = resolve(parts[:-1])
            container = owner.item(inner + parts[-1:], line)
            node = node[parts[-1]][len(owner.items[tuple(inner + parts[-1:])]) - 1]
            inner = []
        else:
            held, path = node, list(inner)
            for part in parts[:-1]:
                held, path = held[part], path + [part]
                container.table(path, line)
            container.add(path, parts[-1], held[parts[-1]], line)
    return top


def describe(path):
    """read's JSON for the import at `path`."""
    text = read_text(path)
    try:
        values = tomllib.loads(text)
    except tomllib.TOMLDecodeError as error:
        return dict(file=str(path), valid=False, error=str(error), keys=[], tables=[])
    try:
        top = written(text, values)
    except (KeyError, IndexError, TypeError, AttributeError) as error:
        raise Refused(f"{path}: its keys' lines could not be found ({error!r})") from error
    if top.values() != values:
        raise Refused(f"{path}: the keys found on its lines do not hold what the file does")
    try:
        import_settings.load_import_settings(path)
        error = None
    except ValueError as refusal:  # a SettingsError is one
        error = str(refusal)
    return dict(file=str(path), valid=error is None, error=error, **top.out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    add_reader(commands, "read", "file", "print the import's tables, each key's line, and whether it is valid, as JSON")
    args = parser.parse_args(argv)
    try:
        emit(json.dumps(describe(import_path(args.file))), args.out)
    except (Refused, OSError) as error:
        print(f"import_edit: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
