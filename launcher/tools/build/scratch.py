"""Fresh snapshots of tracked sources for isolated builds."""
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
from generated_blocks import tracked_files


def scratch_tree(work, root=ROOT, fresh=False):
    tree = work / "tree"
    if tree.exists() or tree.is_symlink():
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
