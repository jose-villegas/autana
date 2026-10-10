#!/usr/bin/env python3
"""Fail on a generated file that its own banner's command no longer makes.

    python scripts/gates/check_generated_files.py [--jobs N] [path ...]
    python scripts/gates/check_generated_files.py --write-table | --check-table

A tracked file is generated when one of its first five lines carries
MARKER. The first non-blank line after the marker, stripped of comment
decoration, is the command that regenerates it, written the way a person
types it:

    python tools/gen/gen_zeta_curve.py > main/boot/boot_anim_curve.h
    python tools/gen/gen_ridge_curve.py ../design/boot/ridge.png main/ui/ridge_curve_generated.h

The command runs from the nearest folder above the file in which its script
(the first word after an interpreter) exists. It names the file itself as
its output, either as the target of a trailing `> path` or as an argument;
the gate captures stdout for the first and swaps the argument for a path in
a temporary folder for the second, so the tracked file is never written.
The result must equal the tracked bytes; the CRLF a Windows console writes
for a newline counts as LF, as git stores it. A banner that echoes the
temporary output path is compared with the original output argument restored;
all other command arguments and the generated payload must still match.

A banner whose command names a <placeholder> input, finds no script, or
does not name its own file as output fails: a file nobody can regenerate is
a committed fixture, and carries no banner.

--check-table checks the engine and app tables of generated outputs against
what --write-table would write, without running any banner; each table's
marker names it, so the generated-document gate runs it.
"""
import argparse
import concurrent.futures
import difflib
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile

MARKER = "GENERATED FILE - do not edit."
REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "launcher/tools/render"))
from generated_blocks import replace_block  # noqa: E402

INTERPRETERS = {"python": sys.executable, "python3": sys.executable, "sh": "sh"}
DIFF_LINES = 20
# The engine's table is kept beside the generators' rules.
TABLE_DOC = "launcher/tools/gen/README.md"
TABLE_BLOCK = "generated-files"
TABLE_CHECK = "python scripts/gates/check_generated_files.py --check-table"


def is_generated(text):
    """True when `text` opens with the marker within its first five lines."""
    return any(MARKER in line for line in text.splitlines()[:5])


def generated_files(root=REPO):
    """Every tracked file in the git tree at `root` whose opening carries the
    marker, as posix paths relative to it. A generator that writes the
    marker carries it too, but further down."""
    result = subprocess.run(["git", "grep", "-lzF", MARKER], cwd=root, capture_output=True, check=False)
    if result.returncode > 1:
        raise RuntimeError(f"git grep in {root} failed: {result.stderr.decode().strip()}")
    names = [name for name in result.stdout.decode("utf-8").split("\0") if name]
    return sorted(name for name in names
                  if is_generated((pathlib.Path(root) / name).read_text(encoding="utf-8", errors="replace")))


def banner_command(text):
    """The command line under the marker, without comment decoration, or
    None when the banner has none."""
    lines = text.splitlines()[:40]
    start = next(i for i, line in enumerate(lines) if MARKER in line)
    for line in lines[start + 1:]:
        command = line.strip().lstrip("/*#").strip()
        if command:
            return command
    return None


def plan(root, name, command):
    """(argv, cwd, output, script) to regenerate `name` from `command`:
    output is None when stdout is the result, else the argument index to
    redirect; script is the generator's path.
    Raises ValueError saying why the banner cannot be run."""
    words = shlex.split(command)
    placeholders = [word for word in words if word.startswith("<") and word.endswith(">")]
    if placeholders:
        raise ValueError(f"the banner's input {placeholders[0]} is not in the repository; "
                         "make it reproducible, or drop the banner and keep the file as a fixture")
    stdout = len(words) >= 2 and words[-2] == ">"
    target = words[-1] if stdout else None
    words = words[:-2] if stdout else words
    if words and words[0] in INTERPRETERS:
        interpreter, words = INTERPRETERS[words[0]], words[1:]
    else:
        interpreter = {".py": sys.executable, ".sh": "sh"}.get(pathlib.PurePosixPath(words[0]).suffix if words else "")
    script = words[0] if words else ""
    path = root / name
    folders = [folder for folder in path.parents if folder == root or root in folder.parents]
    cwd = next((folder for folder in folders if script and (folder / script).is_file()), None)
    if cwd is None:
        raise ValueError(f"the banner's script {script or '(none)'} is in no folder above the file")
    argv = ([shutil.which(interpreter) or interpreter] if interpreter else []) + words

    def is_self(word):
        return (cwd / word).resolve() == path.resolve()

    output = None
    if stdout:
        if not is_self(target):
            raise ValueError(f"the banner writes to {target}, not to this file")
    else:
        output = next((i for i, word in enumerate(argv) if word != script and is_self(word)), None)
        if output is None:
            raise ValueError("the banner's command names neither `> this file` nor this file as an argument")
    return argv, cwd, output, cwd / script


def regenerate(root, name):
    """The bytes `name`'s banner command produces, or raises ValueError."""
    text = (root / name).read_text(encoding="utf-8", errors="replace")
    command = banner_command(text)
    if command is None:
        raise ValueError("the banner names no command")
    argv, cwd, output, _ = plan(root, name, command)
    with tempfile.TemporaryDirectory() as scratch:
        if output is not None:
            original_output = argv[output]
            argv[output] = str(pathlib.Path(scratch) / pathlib.PurePosixPath(name).name)
        result = subprocess.run(argv, cwd=cwd, capture_output=True, check=False)
        if result.returncode != 0:
            stderr = result.stderr.decode("utf-8", "replace").strip().splitlines()[-5:]
            raise ValueError(f"`{command}` failed ({result.returncode}): " + " | ".join(stderr))
        made = result.stdout if output is None else pathlib.Path(argv[output]).read_bytes()
        if output is not None:
            text = made.decode("utf-8", "replace")
            if is_generated(text):
                emitted = banner_command(text)
                if emitted:
                    words = shlex.split(emitted)
                    redirected = {argv[output], pathlib.Path(argv[output]).as_posix()}
                    words = [original_output if word in redirected else word for word in words]
                    if words == shlex.split(command):
                        made = made.replace(emitted.encode(), command.encode(), 1)
    return made.replace(b"\r\n", b"\n")


def check(root, name):
    """None when `name` matches its banner's output, else the reason."""
    try:
        made = regenerate(root, name)
    except ValueError as error:
        return str(error)
    tracked = (root / name).read_bytes()
    if made == tracked:
        return None
    diff = difflib.unified_diff(tracked.decode("utf-8", "replace").splitlines(),
                                made.decode("utf-8", "replace").splitlines(),
                                "tracked", "regenerated", lineterm="", n=1)
    lines = list(diff)
    shown = "\n".join(lines[:DIFF_LINES]) + ("\n..." if len(lines) > DIFF_LINES else "")
    return "differs from what its banner's command makes; rerun that command\n" + shown


def table_documents(names):
    """Generated outputs belong to their engine or app owner's tool index."""
    documents = {TABLE_DOC: (TABLE_BLOCK, [])}
    for name in names:
        parts = pathlib.PurePosixPath(name).parts
        doc = "/".join((*parts[:4], "tools", "README.md")) if parts[:3] == ("launcher", "main", "apps") else TABLE_DOC
        block = TABLE_BLOCK if doc == TABLE_DOC else f"{TABLE_BLOCK}-{parts[3].replace('_', '-')}"
        documents.setdefault(doc, (block, []))[1].append(name)
    return documents


def table(root, names, doc=TABLE_DOC):
    """The Markdown table of `names` for `doc`: each output, the script
    its banner runs, the folder it runs in and the banner's command, linked
    from that document."""
    here = (root / doc).parent

    def link(path):
        return f"[{path.name}]({os.path.relpath(path, here).replace(os.sep, '/')})"

    rows = ["| Output | Generator | Run in | Command |", "|---|---|---|---|"]
    for name in names:
        command = banner_command((root / name).read_text(encoding="utf-8", errors="replace")) or ""
        try:
            _, cwd, _, script = plan(root, name, command)
            generator, folder = link(script), f"`{cwd.relative_to(root).as_posix()}/`"
        except ValueError:
            generator, folder = "-", "-"
        cell = command.replace("|", "\\|")
        rows.append(f"| {link(root / name)} | {generator} | {folder} | `{cell}` |")
    return "\n".join(rows) + "\n"


def tables(root, names, check=False):
    """Write each document's table of `names`, or with `check` count the
    stale ones; a document that cannot take its table counts as failed."""
    failed = 0
    for doc, (block, outputs) in table_documents(names).items():
        try:
            stale = replace_block(root / doc, block, table(root, outputs, doc), check, TABLE_CHECK)
        except (ValueError, OSError) as error:
            print(f"FAIL {doc}: {error}")
            failed += 1
            continue
        if stale and check:
            print(f"FAIL {doc}: its table of generated files is stale; run this gate with --write-table")
            failed += 1
    return failed


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paths", nargs="*", help="generated files to check (default: every one)")
    parser.add_argument("--root", type=pathlib.Path, default=REPO)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--write-table", action="store_true", help=f"rewrite the table in {TABLE_DOC} and exit")
    parser.add_argument("--check-table", action="store_true", help="check only the tables, running no banner")
    args = parser.parse_args()
    root = args.root.resolve()
    if args.write_table or args.check_table:
        return int(tables(root, generated_files(root), args.check_table) > 0)
    names = [pathlib.Path(p).resolve().relative_to(root).as_posix() for p in args.paths] or generated_files(root)
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(lambda name: check(root, name), names))
    for name, problem in zip(names, results):
        print(f"ok   {name}" if problem is None else f"FAIL {name}: {problem}")
    failed = sum(problem is not None for problem in results)
    print(f"generated files: {len(names)} checked, {failed} failed")
    return int(failed > 0)


if __name__ == "__main__":
    sys.exit(main())
