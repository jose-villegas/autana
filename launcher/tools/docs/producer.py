"""A doc-figure producer's declaration, and finding every one in the tree.

A producer is a script beside its area's code that makes images under
docs/images/. It says what it makes in one module-level literal:

    PRODUCER = Producer(area="math", needs="host", outputs=("math/stats-percentile-ranks.png",),
                        run=("--cc", "{cc}", "--out", "{out}/math"))

figures_runner.py reads that literal from the source without importing the
script, so listing every producer loads none of their modules.
"""
import ast
import pathlib
import sys
from typing import NamedTuple

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/gates"))
from tracked import tracked_files  # noqa: E402

# What a producer needs before it can run, and so which machine runs it.
# host: a C compiler and Python with Pillow, numpy and matplotlib.
NEEDS = ("host",)
# The arguments the runner fills in a `run` entry: the host C compiler and
# the folder, laid out like docs/images/, the outputs go into.
FIELDS = ("cc", "out")
DECLARATION = "PRODUCER"
SEARCHED = ("launcher/*.py",)


class Producer(NamedTuple):
    area: str
    needs: str
    outputs: tuple[str, ...]  # paths under docs/images/; a glob (fast-path-*.png) where the code names them
    run: tuple[str, ...] = ()  # the script's arguments; {cc} and {out} are filled in
    reads: tuple[str, ...] = ()  # data files the script reads (globs), checked before any run


class Found(NamedTuple):
    name: str  # the script's path from the repository root
    script: pathlib.Path
    producer: Producer


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
    if producer.needs not in NEEDS:
        raise ValueError(f"{name}: needs {producer.needs!r}, not one of {NEEDS}")
    if not producer.outputs:
        raise ValueError(f"{name}: declares no outputs")
    for argument in producer.run:
        try:
            argument.format(**{field: "" for field in FIELDS})
        except (KeyError, IndexError, ValueError) as error:
            raise ValueError(f"{name}: run argument {argument!r} fills only {FIELDS}: {error}") from None
    return producer


def find(root=ROOT):
    """Every declared producer under launcher/, by name; an output two of them claim is an error."""
    found = {}
    owners = {}
    for name in tracked_files(root, SEARCHED):
        producer = declaration((root / name).read_text(encoding="utf-8"), name)
        if producer is None:
            continue
        for output in producer.outputs:
            if output in owners:
                raise ValueError(f"{output}: made by both {owners[output]} and {name}")
            owners[output] = name
        found[name] = Found(name, root / name, producer)
    return found
