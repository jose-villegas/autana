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
sys.path.insert(0, str(ROOT / "scripts/gates"))

# What a producer needs before it can run, and so which machine runs it.
# host: the developer's machine, with the Python modules Pillow, numpy and
# matplotlib; a C compiler too when `run` uses {cc}. bakes: the baked assets
# it draws, fetched before any run. gpu, board: a GPU or a board.
NEEDS = ("host", "bakes", "gpu", "board")
# Needs whose producers run one at a time.
EXCLUSIVE = ("gpu", "board")
# The arguments the runner fills in a `run` entry: the host C compiler, the
# folder the area's images go into (docs/images/<area>/ or its stand-in), and
# the item of an `each` producer's run.
FIELDS = ("cc", "out", "item")
# Where an `each` producer's items come from: "bakes:<glob>" every bake whose
# output matches (the item is that output less its suffix), "glob:<pattern>"
# every file that matches (the item is its stem).
EACH = ("bakes", "glob")
# The environment variable the runner sets for a run: the folder a probe build (figures.py) writes a compiler
# dependency list (.d) per source into, which tells the runner what C the images were made from.
DEPENDS_FOLDER = "AUTANA_FIGURES_DEPENDS"
DECLARATION = "PRODUCER"
SEARCHED = ("launcher/*.py",)


class Producer(NamedTuple):
    area: str  # also the folder under docs/images/ its images go in
    needs: tuple[str, ...]
    outputs: tuple[str, ...]  # names in the area's folder; a glob (fast-path-*.png) where the code names them
    run: tuple[str, ...] = ()  # the script's arguments; {cc}, {out} and, with an each, {item} are filled in
    reads: tuple[str, ...] = ()  # data files the script reads (globs), checked before any run
    each: str = ""  # one run per item: "bakes:<glob>" or "glob:<pattern>"; outputs name {item}


class Found(NamedTuple):
    name: str  # the script's path from the repository root
    script: pathlib.Path
    producer: Producer

    @property
    def images(self):
        """Its outputs as paths under docs/images/, {item} as a glob."""
        return self.images_of("*")

    def images_of(self, item):
        """Its outputs as paths under docs/images/ for one item."""
        return tuple(f"{self.producer.area}/{output.replace('{item}', item)}" for output in self.producer.outputs)


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
    kind, _, query = producer.each.partition(":")
    if producer.each and (kind not in EACH or not query):
        raise ValueError(f"{name}: each {producer.each!r} is not " + " or ".join(f"{kind}:<pattern>" for kind in EACH))
    # Each item's run makes its own images, so they all name it; without items none may.
    unmatched = [output for output in producer.outputs if ("{item}" in output) != bool(producer.each)]
    if unmatched:
        raise ValueError(f"{name}: outputs {unmatched!r} " + ("must name {item}" if producer.each
                                                               else "name {item} without an each"))
    fields = FIELDS if producer.each else tuple(field for field in FIELDS if field != "item")
    for argument in producer.run:
        try:
            argument.format(**{field: "" for field in fields})
        except (KeyError, IndexError, ValueError) as error:
            raise ValueError(f"{name}: run argument {argument!r} fills only {fields}: {error}") from None
    return producer


def find(root=ROOT):
    """Every declared producer under launcher/, by name. Two producers whose outputs
    can name one image (equal, or one a glob matching the other) are an error; two
    globs that only overlap are caught when a run makes that image twice."""
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
