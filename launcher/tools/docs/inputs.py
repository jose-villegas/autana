"""What a generated output is made from: the code a run executes, as {input: digest} entries whose digest
(bake.digest of the whole dict) names the run."""
import ast
import os
from pathlib import Path


def named_files(path, tracked, bases):
    """The tracked files a module names by a string, resolved as its code would: from each base in turn, then
    its own folder."""
    from bake import bake
    for node in ast.walk(bake.module_facts(path)[0]):
        if isinstance(node, ast.Constant) and isinstance(node.value, str) and node.value.strip():
            for base in (*bases, path.parent):
                # A lexical join, not resolve(): `tracked` holds resolved paths and the bases are resolved.
                candidate = Path(os.path.normpath(os.path.join(base, node.value)))
                if candidate in tracked:
                    yield candidate
                    break


def code_inputs(entries, root, tracked, bases):
    """{input: digest} of the Python that running `entries` as scripts executes: their import closure (a
    script's own folder is on its path) and that of each script the closure names, until none is new, with
    the C and requirements bake.py counts for them, and the other tracked files that code names. `tracked` is
    the set of resolved tracked paths; names resolve from `bases`."""
    from bake import bake
    from r3d.import_settings import content_checksum
    entries = {Path(entry).resolve() for entry in entries}
    while True:
        code = bake.closure(sorted(entries), consumers=True, roots=sorted({entry.parent for entry in entries}))
        named = {file for path in code for file in named_files(path, tracked, bases)}
        scripts = {file for file in named if file.suffix == ".py"} - entries
        if not scripts:
            break
        entries |= scripts

    def name(path):
        return path.resolve().relative_to(root).as_posix()

    return {**{f"code {name(path)}": bake.digest(bake.code_tokens(path)) for path in code},
            **{f"imports {name(path)}": bake.digest([
                (node.module if isinstance(node, ast.ImportFrom) else None,
                 node.level if isinstance(node, ast.ImportFrom) else 0, alias.name, alias.asname)
                for node in ast.walk(bake.module_facts(path)[0])
                if isinstance(node, (ast.Import, ast.ImportFrom)) for alias in node.names]) for path in code},
            **{f"native {path}": value for path, value in bake.native_inputs(code).items()},
            **{f"requirement {file} {line}": line for file, line in bake.pinned_requirements(code)},
            **{f"file {name(path)}": content_checksum(path).decode() for path in sorted(named - set(code))}}
