"""Print a configured scene's flags and compiler dependencies from its CMake build, or, with --into DIR, write
what the build read into DIR as dependency lists (.d): each source's headers, the CMake files the configure
read (CMake's file API; render_scene.sh places the cmakeFiles query before configuring) and each FILE.

    python dependencies.py BUILD
    python dependencies.py BUILD --into DIR [FILE ...]
"""
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "build"))
from compile_db import compile_flags

ESCAPED_SPACE = "\\ "  # a space inside a .d name, as compilers write it


def entries(build):
    names = [Path(name).resolve() for name in (build / "dependency_sources.txt").read_text().splitlines()]
    found = [entry for entry in json.loads((build / "compile_commands.json").read_text())
             if Path(entry["file"]).resolve() in set(names)]
    return names, found


def cmake_inputs(build):
    """The CMake files the last configure of `build` read, CMake's own modules and generated files left out."""
    reply = build / ".cmake/api/v1/reply"
    index = json.loads(max(reply.glob("index-*.json")).read_text())
    listing = next(json.loads((reply / item["jsonFile"]).read_text())
                   for item in index["objects"] if item["kind"] == "cmakeFiles")
    source = Path(listing["paths"]["source"])
    return [str(source / item["path"]) for item in listing["inputs"]
            if not item.get("isCMake") and not item.get("isGenerated")]


def write_lists(build, into, files):
    """The lists --into writes."""
    _, found = entries(build)
    for number, entry in enumerate(found):
        listing = into / f"host-{number}.d"
        subprocess.run([*compile_flags(entry), "-MM", "-MF", str(listing.resolve()), entry["file"]],
                       cwd=entry["directory"], check=True)
    names = [Path(name).resolve().as_posix().replace(" ", ESCAPED_SPACE) for name in (*cmake_inputs(build), *files)]
    (into / "host-inputs.d").write_text("host: " + " ".join(names) + chr(10), newline=chr(10))


def main():
    build = Path(sys.argv[1])
    if sys.argv[2:3] == ["--into"]:
        write_lists(build, Path(sys.argv[3]), sys.argv[4:])
        return
    names, found = entries(build)
    # render_scene's sources come first (scene/CMakeLists.txt), all compiled with the scene's one flag set.
    scene = next(entry for entry in found if Path(entry["file"]).resolve() == names[0])
    flags = compile_flags(scene)[1:]
    # Keep include paths as separate tokens for doc_stages' repo-relative stamp.
    flags = [part for flag in flags for part in (["-I", flag[2:]] if flag.startswith("-I") else [flag])]
    print("flags " + " ".join(flags), flush=True)
    for entry in found:
        subprocess.run([*compile_flags(entry), "-MM", entry["file"]], cwd=entry["directory"], check=True)


if __name__ == "__main__":
    main()
