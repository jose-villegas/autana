#!/usr/bin/env python3
"""Make, check and look up the doc images that producers declare (see producer.py).

    figures_runner.py list [--area AREA] [--outputs]
    figures_runner.py which IMAGE ...
    figures_runner.py build [--area AREA] [SCRIPT ...] [--into DIR] [--jobs N]
    figures_runner.py check [--area AREA] [SCRIPT ...] [--jobs N]

list prints each producer: its script, area, what it needs and how many images
it makes; --outputs prints every declared image instead, a glob as the
committed images it matches. which names the
producer of an image (a path under docs/images/, with or without that prefix)
and the command that makes it.

build and check select every producer, those of one area, or the named
scripts. Before running any, they check what the selection needs: a C
compiler, Pillow, numpy and matplotlib, and a file for each `reads` pattern;
every problem is printed at once and nothing runs. The producers then run in
parallel, each into its own folder with its own log under
launcher/tools/results/doc_images/work/figures/. A producer that fails, leaves
out a declared image or writes one it does not declare stops the run.

build copies the images into docs/images/, or into DIR with --into. check
compares each with the committed image by decoded pixels (compare_images.py)
and prints "same IMAGE", "changed IMAGE: how" or "changed IMAGE: new image".

Exit status: 0 done (check: nothing changed), 1 check found a change, 2 the
images could not be made or compared.
"""
import argparse
import concurrent.futures
import fnmatch
import glob
import importlib.util
import os
import pathlib
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from producer import ROOT, find  # noqa: E402

sys.path.insert(0, str(ROOT / "launcher/tools/render"))
from compare_images import compare, load_frames  # noqa: E402

IMAGES = ROOT / "docs/images"
WORK = ROOT / "launcher/tools/results/doc_images/work/figures"
FIND_CC = "launcher/tools/build/find_cc.sh"
# The modules a host producer imports beyond the standard library.
HOST_MODULES = ("PIL", "numpy", "matplotlib")
LOG_TAIL_LINES = 40


class Failed(Exception):
    """The images could not be made; the message says why."""


def select(found, area, scripts):
    names = [pathlib.Path(script).resolve().relative_to(ROOT).as_posix() for script in scripts]
    unknown = [name for name in names if name not in found]
    if unknown:
        raise Failed("no producer is declared in " + ", ".join(unknown))
    chosen = [found[name] for name in names] or list(found.values())
    chosen = [item for item in chosen if area in (None, item.producer.area)]
    if not chosen:
        raise Failed(f"no producer in area {area}")
    return chosen


def find_cc():
    result = subprocess.run(["sh", "-c", f'. "{FIND_CC}" && find_cc'], cwd=ROOT, capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def preflight(chosen):
    """The values a run fills in; every missing need is reported together before anything runs."""
    problems = [f"Python module {module} is missing ({sys.executable})"
                for module in HOST_MODULES if importlib.util.find_spec(module) is None]
    cc = None
    if any("{cc}" in argument for item in chosen for argument in item.producer.run):
        cc = find_cc()
        if cc is None:
            problems.append("no host C compiler: set CC, or put cc, gcc or clang on PATH")
    for item in chosen:
        problems += [f"{item.name}: reads {pattern}, which matches no file"
                     for pattern in item.producer.reads if not any(ROOT.glob(pattern))]
    if problems:
        raise Failed("\n".join(problems))
    return {"cc": cc}


def claims(item, name):
    """Whether `name`, a path under docs/images/, is one of the producer's outputs."""
    return any(fnmatch.fnmatchcase(name, output) for output in item.producer.outputs)


def made_files(folder):
    return {path.relative_to(folder).as_posix() for path in folder.rglob("*") if path.is_file()}


def run_one(item, values):
    """Run one producer into a fresh folder; returns the folder and the images in it, or raises Failed
    with its log's end."""
    work = WORK / item.name.replace("/", "-")
    out = work / "out"
    shutil.rmtree(work, ignore_errors=True)
    out.mkdir(parents=True)
    log = work / "run.log"
    command = [sys.executable, str(item.script),
               *(argument.format(**values, out=out.as_posix()) for argument in item.producer.run)]
    with log.open("w", encoding="utf-8") as stream:
        code = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT).returncode
    if code != 0:
        tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-LOG_TAIL_LINES:]
        raise Failed("\n".join([f"{item.name} failed (exit {code}); {log.as_posix()}:", *tail]))
    made = made_files(out)
    missing = [output for output in item.producer.outputs if not fnmatch.filter(made, output)]
    undeclared = sorted(name for name in made if not claims(item, name))
    if missing or undeclared:
        raise Failed("\n".join([f"{item.name} did not make what it declares:",
                                *(f"  missing {name}" for name in missing),
                                *(f"  undeclared {name}" for name in undeclared)]))
    return out, sorted(made)


def run_all(chosen, values, jobs):
    """Each producer's folder by name; every failure is reported once all have finished."""
    folders, failures = {}, []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        running = {pool.submit(run_one, item, values): item for item in chosen}
        for future in concurrent.futures.as_completed(running):
            try:
                folders[running[future].name] = future.result()
            except Failed as error:
                failures.append(str(error))
    if failures:
        raise Failed("\n".join(failures))
    return folders


def deliver(folders, target):
    for out, made in folders.values():
        for name in made:
            (target / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(out / name, target / name)
    print(f"wrote {sum(len(made) for _, made in folders.values())} images to {target}")


def check(chosen, folders):
    """Print same or changed for each made image, and orphan for each kept one a producer claims but no longer makes."""
    changed = False
    kept = made_files(IMAGES)
    for item in chosen:
        out, made = folders[item.name]
        for name in made:
            if name not in kept:
                print(f"changed {name}: new image")
                changed = True
                continue
            try:
                difference = compare(load_frames(IMAGES / name), load_frames(out / name))
            except OSError as error:
                raise Failed(f"unreadable {name}: {error}") from None
            print(f"changed {name}: {difference}" if difference else f"same {name}")
            changed |= difference is not None
        for name in sorted(kept - set(made)):
            if claims(item, name):
                print(f"orphan {name}: {item.name} no longer makes it")
                changed = True
    return changed


def which(found, images):
    status = 0
    for image in images:
        name = pathlib.Path(image).as_posix().removeprefix("docs/images/")
        item = next((item for item in found.values() if claims(item, name)), None)
        if item is None:
            print(f"{name}: no producer declares it")
            status = 1
            continue
        arguments = " ".join(item.producer.run)
        print(f"{name}: {item.name} (area {item.producer.area}, needs {item.producer.needs})\n"
              f"  python {item.name} {arguments}".rstrip())
        for pattern in item.producer.reads:
            print(f"  reads {pattern}")
    return status


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    listing = commands.add_parser("list")
    listing.add_argument("--area")
    listing.add_argument("--outputs", action="store_true")
    commands.add_parser("which").add_argument("images", nargs="+")
    for name in ("build", "check"):
        command = commands.add_parser(name)
        command.add_argument("scripts", nargs="*")
        command.add_argument("--area")
        command.add_argument("--jobs", type=int, default=os.cpu_count())
        if name == "build":
            command.add_argument("--into", type=pathlib.Path, default=IMAGES)
    args = parser.parse_args(argv)
    try:
        found = find()
        if args.command == "which":
            return which(found, args.images)
        if args.command == "list":
            kept = made_files(IMAGES) if args.outputs else set()
            for item in select(found, args.area, []):
                if args.outputs:
                    print("\n".join(sorted({name for name in kept if claims(item, name)} |
                                           {output for output in item.producer.outputs if not glob.has_magic(output)})))
                else:
                    print(f"{item.name}\t{item.producer.area}\t{item.producer.needs}\t{len(item.producer.outputs)}")
            return 0
        chosen = select(found, args.area, args.scripts)
        folders = run_all(chosen, preflight(chosen), args.jobs)
        if args.command == "build":
            deliver(folders, args.into)
            return 0
        return int(check(chosen, folders))
    except (Failed, ValueError, OSError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
