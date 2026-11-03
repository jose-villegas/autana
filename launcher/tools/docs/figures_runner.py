#!/usr/bin/env python3
"""Make, check and look up the doc images that producers declare (see producer.py).

    figures_runner.py list [--area AREA] [--outputs]
    figures_runner.py which IMAGE ...
    figures_runner.py build [SELECTION] [--into DIR] [--changed] [--jobs N]
    figures_runner.py check [SELECTION] [--changed] [--jobs N]
    figures_runner.py index [--check]

    SELECTION: [--area AREA] [--needs NEED,...] [SCRIPT ...]

list prints each producer's script, area, needs and image count; --outputs
prints every declared image instead (a glob as the committed images it
matches). which names the producer of an image (a path under docs/images/,
with or without that prefix) and the command that makes it.

build and check select every producer, those of one area, those whose needs
are all among --needs (host,bakes: what one machine without a GPU or board
can run), or the named scripts. A producer with an `each` runs once per item.
With --changed, a run whose inputs are those recorded at its last good run
is skipped: its arguments and item, its code (import closure, the C and
requirements it counts, and the tracked files that code names), the C its
probes compiled, its `reads` files and its bake's lock row.

Before running any, they check what the runs need: Pillow, numpy and
matplotlib, a C compiler when a `run` uses {cc}, a file for each `reads`
pattern, an item for each `each`, and every bake drawn, fetched into the
bake cache; every problem is printed at once and nothing runs. The runs then
go in parallel, those needing a GPU or a board one at a time, each into its
own folder with its own log under launcher/tools/results/doc_images/work/figures/.
A run that fails, leaves out a declared image or writes one it does not
declare stops the command.

build copies the images into docs/images/, or into DIR with --into. check
compares each with the committed image by decoded pixels (compare_images.py)
and prints "same IMAGE", "changed IMAGE: how" or "changed IMAGE: new image",
or "orphan IMAGE: SCRIPT no longer makes it" for a committed image one of a
producer's globs claims but no run made or skipped. Both record each run's
inputs in launcher/tools/results/doc_images/fingerprints.json (--fingerprints
names another file): build every run's when it writes docs/images/, check
those whose images were all same and that claim no orphan. A command that
fails records nothing. The order is drawn in docs/tools/Doc-Figures.md.

index writes docs/tools/Figures.md, every image under docs/images/ with its
picture, the pages that show it and its producer, and the small GIF copies
it shows (figure_index.py); --check says what differs instead. An image no
page shows, and an image a page shows that does not exist, are warnings.

Exit status: 0 done (check: nothing changed), 1 check or index --check found a
change or an orphan, 2 the images could not be made or compared.
"""
import argparse
import concurrent.futures
import fnmatch
import glob
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
from typing import NamedTuple

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from producer import DEPENDS_FOLDER, EXCLUSIVE, NEEDS, ROOT, Found, find  # noqa: E402

sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.find_cc import host_cc  # noqa: E402

# compare_images needs Pillow, which preflight reports on rather than crash importing.
sys.path.insert(0, str(ROOT / "launcher/tools/render"))

IMAGES = ROOT / "docs/images"
RESULTS = ROOT / "launcher/tools/results/doc_images"
WORK = RESULTS / "work/figures"
FINGERPRINTS = RESULTS / "fingerprints.json"
# The modules a host producer imports beyond the standard library.
HOST_MODULES = ("PIL", "numpy", "matplotlib")
LOG_TAIL_LINES = 40


class Failed(Exception):
    """The images could not be made; the message says why."""


class Run(NamedTuple):
    """One run of a producer: once, or once per item of its `each`."""
    found: Found
    item: str = ""
    bake: object = None  # the bake.Bake an each="bakes:" run draws

    @property
    def name(self):
        return f"{self.found.name}[{self.item}]" if self.item else self.found.name

    @property
    def images(self):
        return self.found.images_of(self.item)


def select(found, area, needs, scripts):
    names = list(dict.fromkeys(pathlib.Path(script).resolve().relative_to(ROOT).as_posix() for script in scripts))
    unknown = [name for name in names if name not in found]
    if unknown:
        raise Failed("no producer is declared in " + ", ".join(unknown))
    if needs is not None and needs - set(NEEDS):
        raise Failed(f"--needs takes {','.join(NEEDS)}")
    chosen = [found[name] for name in names] or list(found.values())
    chosen = [item for item in chosen if area in (None, item.producer.area)
              and (needs is None or set(item.producer.needs) <= needs)]
    if not chosen:
        raise Failed("no producer matches the selection")
    return chosen


def expand(chosen):
    """Every run of the chosen producers, and a problem for each `each` that names nothing or one item twice
    (two files of one stem)."""
    runs, problems, baked = [], [], None
    for found in chosen:
        kind, _, pattern = found.producer.each.partition(":")
        if kind == "bakes":
            if baked is None:
                from bake import bake
                baked = bake.bakes()
            matched = [Run(found, item.output.removesuffix(item.suffix), item) for item in baked
                       if fnmatch.fnmatchcase(item.output, pattern)]
        elif kind == "glob":
            matched = [Run(found, path.stem) for path in sorted(ROOT.glob(pattern))]
        else:
            matched = [Run(found)]
        if not matched:
            problems.append(f"{found.name}: each {found.producer.each} names nothing")
        items = [run.item for run in matched]
        problems += [f"{found.name}: each {found.producer.each} names item {item} twice"
                     for item in sorted({item for item in items if items.count(item) > 1})]
        runs += matched
    return runs, problems


def preflight(runs, problems=()):
    """The values a run fills in; every missing need is reported together before anything runs."""
    problems = list(problems)
    problems += [f"Python module {module} is missing ({sys.executable})"
                 for module in HOST_MODULES if importlib.util.find_spec(module) is None]
    cc = None
    if any("{cc}" in argument for run in runs for argument in run.found.producer.run):
        cc = host_cc()
        if cc is None:
            problems.append("no host C compiler: set CC, or put cc, gcc or clang on PATH")
    for found in {run.found.name: run.found for run in runs}.values():
        problems += [f"{found.name}: reads {pattern}, which matches no file"
                     for pattern in found.producer.reads if not any(ROOT.glob(pattern))]
    drawn = [run.bake for run in runs if run.bake is not None]
    if drawn:
        from bake import bake
        try:
            bake.fetch_all(drawn, bake.read_lock(), bake.default_cache())
        except bake.BakeMissing as error:
            problems.append(str(error))
    if problems:
        raise Failed("\n".join(problems))
    return {"cc": cc}


def fingerprint(run, probe, code):
    """The digest of a run's inputs. `probe`: the C files its probes compiled, by repository path; `code`: the
    code inputs by script, shared by a producer's runs and filled as needed."""
    from bake import bake
    from docs.inputs import bake_inputs, code_inputs
    from r3d.import_settings import content_checksum

    def checksum(name):
        return content_checksum(ROOT / name).decode() if (ROOT / name).is_file() else "missing"

    if run.found.name not in code:
        code[run.found.name] = code_inputs([run.found.script], ROOT)
    return bake.digest({"run": [run.item, *run.found.producer.run], **code[run.found.name],
                        **{f"probe {name}": checksum(name) for name in probe},
                        **{f"reads {path.relative_to(ROOT).as_posix()}": checksum(path.relative_to(ROOT))
                           for pattern in run.found.producer.reads for path in sorted(ROOT.glob(pattern))},
                        **(bake_inputs([run.bake], bake.read_lock()) if run.bake is not None else {})})


def read_records(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return {}


def write_records(path, records):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(records, indent=1, sort_keys=True) + "\n", encoding="utf-8", newline="\n")


def probe_sources(depends):
    """The repository C files the .d lists in `depends` name; outside files (system headers) are not counted."""
    from build.compile_db import parse_dependencies
    names = set()
    for listing in sorted(depends.glob("*.d")):
        for token in parse_dependencies(listing.read_text(encoding="utf-8"), directory=ROOT):
            path = pathlib.Path(token).resolve()
            if path.is_relative_to(ROOT):
                names.add(path.relative_to(ROOT).as_posix())
    return sorted(names)


def claims(images, name):
    """Whether `name`, a path under docs/images/, matches one of `images`."""
    return any(fnmatch.fnmatchcase(name, image) for image in images)


def made_files(folder):
    return {path.relative_to(folder).as_posix() for path in folder.rglob("*") if path.is_file()}


def run_one(run, values):
    """Make one run into a fresh folder; returns the folder, the images in it and the C its probes compiled,
    or raises Failed with its log's end."""
    # Readable, and unique though two names read alike once cleaned.
    work = WORK / f"{re.sub(r'[^\w.-]', '-', run.name)}-{hashlib.sha256(run.name.encode()).hexdigest()[:12]}"
    out, depends = work / "out", work / "depends"
    shutil.rmtree(work, ignore_errors=True)
    out.mkdir(parents=True)
    depends.mkdir()
    log = work / "run.log"
    filled = {**values, "item": run.item, "out": (out / run.found.producer.area).as_posix()}
    command = [sys.executable, str(run.found.script), *(argument.format(**filled) for argument in run.found.producer.run)]
    with log.open("w", encoding="utf-8") as stream:
        code = subprocess.run(command, cwd=ROOT, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
                              env={**os.environ, DEPENDS_FOLDER: str(depends)}).returncode
    if code != 0:
        tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-LOG_TAIL_LINES:]
        raise Failed("\n".join([f"{run.name} failed (exit {code}); {log.as_posix()}:", *tail]))
    made = made_files(out)
    missing = [image for image in run.images if not fnmatch.filter(made, image)]
    undeclared = sorted(name for name in made if not claims(run.images, name))
    if missing or undeclared:
        raise Failed("\n".join([f"{run.name} did not make what it declares:",
                                *(f"  missing {name}" for name in missing),
                                *(f"  undeclared {name}" for name in undeclared)]))
    if any("{cc}" in argument for argument in run.found.producer.run) and not any(depends.iterdir()):
        raise Failed(f"{run.name} compiled C the runner could not see: build it with docs.figures.build_probe, "
                     "which lists what it compiled")
    return out, sorted(made), probe_sources(depends)


def run_all(runs, values, jobs):
    """Each run's result by name; every failure is reported once all have finished. Runs needing a GPU or a
    board go one at a time, beside the others."""
    results, failures = {}, []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool, \
            concurrent.futures.ThreadPoolExecutor(max_workers=1) as alone:
        running = {(alone if set(run.found.producer.needs) & set(EXCLUSIVE) else pool).submit(run_one, run, values): run
                   for run in runs}
        for future in concurrent.futures.as_completed(running):
            try:
                results[running[future].name] = future.result()
            except Failed as error:
                failures.append(str(error))
    makers = {}
    for name, (_, made, _) in sorted(results.items()):
        for image in made:
            if image in makers:
                failures.append(f"{image}: made by both {makers[image]} and {name}")
            makers[image] = name
    if failures:
        raise Failed("\n".join(failures))
    return results


def deliver(results, target):
    for out, made, _ in results.values():
        for name in made:
            (target / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(out / name, target / name)
    print(f"wrote {sum(len(made) for _, made, _ in results.values())} images to {target}")


def check(chosen, runs, skipped, results):
    """Print same or changed for each made image, and orphan for each kept one a producer claims but no run
    made or skipped; returns the names of the runs whose images all were same, and whether any was not."""
    from compare_images import compare, load_frames
    kept = made_files(IMAGES)
    same, changed = set(), False
    for run in runs:
        out, made, _ = results[run.name]
        run_changed = False
        for name in made:
            if name not in kept:
                print(f"changed {name}: new image")
                run_changed = True
                continue
            try:
                difference = compare(load_frames(IMAGES / name), load_frames(out / name))
            except OSError as error:
                raise Failed(f"unreadable {name}: {error}") from None
            print(f"changed {name}: {difference}" if difference else f"same {name}")
            run_changed |= difference is not None
        if not run_changed:
            same.add(run.name)
        changed |= run_changed
    for found in chosen:
        made = {name for run in runs if run.found is found for name in results[run.name][1]}
        left = [image for run in skipped if run.found is found for image in run.images]
        for name in sorted(kept - made):
            if claims(found.images, name) and not claims(left, name):
                print(f"orphan {name}: {found.name} no longer makes it")
                changed = True
                # A run that claims it stays unrecorded, or --changed would skip it and its glob would hide this.
                same -= {run.name for run in runs if run.found is found and claims(run.images, name)}
    return same, changed


def which(found, images):
    status = 0
    for image in images:
        name = pathlib.Path(image).as_posix().removeprefix("docs/images/")
        item = next((item for item in found.values() if claims(item.images, name)), None)
        if item is None:
            print(f"{name}: no producer declares it")
            status = 1
            continue
        print(f"{name}: {item.name} (area {item.producer.area}, needs {', '.join(item.producer.needs)})\n"
              f"  python launcher/tools/docs/figures_runner.py build {item.name}")
        if item.producer.each:
            print(f"  each {item.producer.each}")
        for pattern in item.producer.reads:
            print(f"  reads {pattern}")
    return status


def make(args, found):
    """build or check; returns the exit status."""
    chosen = select(found, args.area, args.needs, args.scripts)
    runs, problems = expand(chosen)
    records, code = read_records(args.fingerprints), {}
    # Before any run, so code the fingerprint cannot follow fails in seconds.
    before = {run.name: fingerprint(run, records.get(run.name, {}).get("probe", ()), code) for run in runs}
    skipped = []
    if args.changed:
        skipped = [run for run in runs if records.get(run.name, {}).get("fingerprint") == before[run.name]]
        for run in skipped:
            print(f"unchanged {run.name}")
        runs = [run for run in runs if run not in skipped]
    results = run_all(runs, preflight(runs, problems), args.jobs)
    if args.command == "build":
        deliver(results, args.into)
        # A record says docs/images/ holds what the inputs make; images put elsewhere do not.
        recorded = {run.name for run in runs} if args.into.resolve() == IMAGES.resolve() else set()
        status = 0
    else:
        recorded, changed = check(chosen, runs, skipped, results)
        status = int(changed)
    for run in runs:
        if run.name in recorded:
            probe = results[run.name][2]
            records[run.name] = {"fingerprint": fingerprint(run, probe, code), "probe": probe}
    write_records(args.fingerprints, records)
    return status


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    listing = commands.add_parser("list")
    listing.add_argument("--area")
    listing.add_argument("--outputs", action="store_true")
    commands.add_parser("which").add_argument("images", nargs="+")
    commands.add_parser("index").add_argument("--check", action="store_true")
    for name in ("build", "check"):
        command = commands.add_parser(name)
        command.add_argument("scripts", nargs="*")
        command.add_argument("--area")
        command.add_argument("--needs", type=lambda value: set(value.split(",")))
        command.add_argument("--changed", action="store_true")
        command.add_argument("--fingerprints", type=pathlib.Path, default=FINGERPRINTS)
        command.add_argument("--jobs", type=int, default=os.cpu_count())
        if name == "build":
            command.add_argument("--into", type=pathlib.Path, default=IMAGES)
    args = parser.parse_args(argv)
    try:
        found = find()
        if args.command == "which":
            return which(found, args.images)
        if args.command == "index":
            from figure_index import index
            differences = index(lambda image: next((item.name for item in found.values()
                                                    if claims(item.images, image.removeprefix("docs/images/"))), None),
                                args.check)
            if differences:
                print("\n".join(differences))
            return int(bool(differences) and args.check)
        if args.command == "list":
            kept = made_files(IMAGES) if args.outputs else set()
            for item in (item for item in found.values() if args.area in (None, item.producer.area)):
                if args.outputs:
                    print("\n".join(sorted({name for name in kept if claims(item.images, name)} |
                                           {image for image in item.images if not glob.has_magic(image)})))
                else:
                    print(f"{item.name}\t{item.producer.area}\t{','.join(item.producer.needs)}\t{len(item.images)}")
            return 0
        return make(args, found)
    except (Failed, ValueError, OSError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
