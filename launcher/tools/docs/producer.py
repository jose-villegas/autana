"""A doc-figure producer's declaration, and finding every one in the tree.

A producer is a script beside its area's code that makes images under
docs/images/. It states them in one module-level `PRODUCER = Producer(...)`
literal (fields and an example: docs/tools/Doc-Figures.md), which
figures_runner.py reads from the source without importing the script.
"""
import ast
import fnmatch
import pathlib
import sys
from typing import NamedTuple

ROOT = pathlib.Path(__file__).resolve().parents[3]

# What a producer needs before it can run, and so which machine runs it.
# host: the developer's machine, with the Python modules Pillow, numpy and
# matplotlib; a C compiler too when `run` uses {cc}.
NEEDS = ("host",)
# The arguments the runner fills in a `run` entry: the host C compiler and
# the folder the area's images go into, docs/images/<area>/ or its stand-in.
FIELDS = ("cc", "out")
DECLARATION = "PRODUCER"
SEARCHED = ("launcher/*.py",)


class Producer(NamedTuple):
    area: str  # also the folder under docs/images/ its images go in
    needs: tuple[str, ...]
    outputs: tuple[str, ...]  # names in the area's folder; a glob (fast-path-*.png) where the code names them
    run: tuple[str, ...] = ()  # the script's arguments; {cc} and {out} are filled in
    reads: tuple[str, ...] = ()  # data files the script reads (globs), checked before any run


class Found(NamedTuple):
    name: str  # the script's path from the repository root
    script: pathlib.Path
    producer: Producer

    @property
    def images(self):
        """Its outputs as paths under docs/images/."""
        return tuple(f"{self.producer.area}/{output}" for output in self.producer.outputs)


def declaration(source, name):
    """The Producer a script's source declares, or None when it declares none."""
    if DECLARATION not in source:
        return None
    for node in ast.parse(source, name).body:
        if not (isinstance(node, ast.Assign) and [getattr(target, "id", None) for target in node.targets] == [DECLARATION]):
            continue
        call = node.value
        if not (isinstance(call, ast.Call) and getattr(call.func, "id", None) == "Producer" and not call.args):
            raise ValueError(f"{name}: {DECLARATION} must be Producer(...) with keyword arguments")
        try:
            fields = {keyword.arg: ast.literal_eval(keyword.value) for keyword in call.keywords}
            producer = Producer(**fields)
        except (ValueError, TypeError) as error:
            raise ValueError(f"{name}: {DECLARATION} is not a literal Producer: {error}") from None
        return validate(producer, name)
    return None


def validate(producer, name):
    unknown = set(producer.needs) - set(NEEDS)
    if not producer.needs or unknown:
        raise ValueError(f"{name}: needs {producer.needs!r}, each one of {NEEDS}")
    if not producer.outputs:
        raise ValueError(f"{name}: declares no outputs")
    for argument in producer.run:
        try:
            argument.format(**{field: "" for field in FIELDS})
        except (KeyError, IndexError, ValueError) as error:
            raise ValueError(f"{name}: run argument {argument!r} fills only {FIELDS}: {error}") from None
    return producer


def find(root=ROOT):
    """Every declared producer under launcher/, by name. Two producers whose outputs
    can name one image (equal, or one a glob matching the other) are an error; two
    globs that only overlap are caught when a run makes that image twice."""
    sys.path.insert(0, str(ROOT / "scripts/gates"))
    from tracked import tracked_files
    found = {}
    owners = []
    for name in tracked_files(root, SEARCHED):
        producer = declaration((root / name).read_text(encoding="utf-8"), name)
        if producer is None:
            continue
        item = Found(name, root / name, producer)
        for image in item.images:
            for other, owner in owners:
                if fnmatch.fnmatchcase(image, other) or fnmatch.fnmatchcase(other, image):
                    raise ValueError(f"{image}: made by both {owner} ({other}) and {name}")
        owners += [(image, name) for image in item.images]
        found[name] = item
    return found
