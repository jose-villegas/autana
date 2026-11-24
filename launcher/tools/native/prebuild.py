"""Builds every shared library the tools load, one after another, so processes
started afterwards find each one current and none builds it. A builder is a
module-level function under launcher/tools that takes no arguments and calls
build_shared; it is found in the source, so a new library needs no entry here.
A builder that fails is reported and the rest still build: the test that needs
that library says what is wrong.

    python launcher/tools/native/prebuild.py
"""

import ast
import importlib
import pathlib
import subprocess
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[1]
BUILD_CALL = "build_shared"


def calls_build(function):
    return any(isinstance(node, ast.Call) and getattr(node.func, "id", getattr(node.func, "attr", None)) == BUILD_CALL
               for node in ast.walk(function))


def builders(root, files):
    """(module, function) for each module-level function in `files` that takes no arguments and calls build_shared;
    module is dotted from `root`."""
    found = []
    for path in files:
        tree = ast.parse(path.read_bytes(), str(path))
        module = ".".join(path.relative_to(root).with_suffix("").parts)
        for node in tree.body:
            arguments = node.args if isinstance(node, ast.FunctionDef) else None
            if arguments and not (arguments.args or arguments.posonlyargs or arguments.kwonlyargs) and calls_build(node):
                found.append((module, node.name))
    return found


def tracked_sources(root):
    listed = subprocess.run(["git", "ls-files", "-z", "--", "*.py"], cwd=root, capture_output=True, text=True,
                            check=True).stdout.split("\0")
    return [root / name for name in listed if name and "tests" not in pathlib.PurePosixPath(name).parts]


def build_all(root, files, log=print):
    """Calls each builder; returns how many failed."""
    sys.path.insert(0, str(root))
    failed = 0
    for module, name in builders(root, files):
        try:
            getattr(importlib.import_module(module), name)()
            log(f"built {module}.{name}")
        except (Exception, SystemExit) as error:  # a missing submodule or compiler exits; its test reports it
            failed += 1
            log(f"could not build {module}.{name}: {error}")
    return failed


if __name__ == "__main__":
    build_all(TOOLS, tracked_sources(TOOLS))
