"""Shared Markdown formatting and tracked scratch snapshots."""
import pathlib
import os
import shlex
import shutil
from generated_blocks import tracked_files

ROOT = pathlib.Path(__file__).resolve().parents[3]


def markdown(headers, rows):
    cell = lambda value: str(value).replace("|", "\\|").replace("\n", " ")
    headers = [cell(value) for value in headers]
    lines = ["| " + " | ".join(headers) + " |", "|" + "|".join("---" for _ in headers) + "|"]
    return "\n".join(lines + ["| " + " | ".join(map(cell, row)) + " |" for row in rows]) + "\n"


def scratch_tree(work, root=ROOT, fresh=False):
    tree = work / "tree"
    if tree.exists():
        if not fresh:
            raise ValueError(f"{tree} already exists; use a fresh --work directory")
        if not tree.resolve().is_relative_to(work.resolve()) or tree.is_symlink():
            raise ValueError(f"{tree} is outside the scratch directory")
        shutil.rmtree(tree)
    for name in tracked_files(root):
        source, target = root / name, tree / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_dir():
            shutil.copytree(source, target, ignore=shutil.ignore_patterns(".git", "build", "__pycache__"))
        else:
            shutil.copyfile(source, target)
    return tree


def compile_flags(entry):
    command = entry.get("arguments") or shlex.split(entry["command"], posix=os.name != "nt")
    filtered = []
    skip = False
    for arg in command:
        if os.name == "nt":
            if arg.startswith('@"'):
                arg = "@" + arg[2:-1]
            elif arg.startswith('"') and arg.endswith('"'):
                arg = arg[1:-1]
            else:
                arg = arg.replace('\\"', '"')
        if skip:
            skip = False
        elif arg in {"-o", "-MF", "-MT", "-MQ"}:
            skip = True
        elif arg not in {"-c", "-MD", "-MMD", "-MP"} and arg.replace("\\", "/") != entry["file"].replace("\\", "/"):
            filtered.append(arg)
    return filtered
