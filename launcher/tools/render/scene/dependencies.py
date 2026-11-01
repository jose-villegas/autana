"""Print a configured scene's flags and compiler dependencies from its CMake build."""
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "build"))
from compile_db import compile_flags


def main():
    build = Path(sys.argv[1])
    sources = {Path(name).resolve() for name in (build / "dependency_sources.txt").read_text().splitlines()}
    entries = [entry for entry in json.loads((build / "compile_commands.json").read_text())
               if Path(entry["file"]).resolve() in sources]
    scene = next(entry for entry in entries if "render_scene.dir" in entry["command"])
    flags = compile_flags(scene)[1:]
    # Keep include paths as separate tokens for doc_stages' repo-relative stamp.
    flags = [part for flag in flags for part in (["-I", flag[2:]] if flag.startswith("-I") else [flag])]
    links = (build / "dependency_links.txt").read_text().splitlines()
    print("flags " + " ".join([*flags, *links]), flush=True)
    for entry in entries:
        subprocess.run([*compile_flags(entry), "-MM", entry["file"]], cwd=entry["directory"], check=True)


if __name__ == "__main__":
    main()
