"""Print a configured scene's flags and what its build reads, or with --into DIR write the second as dependency
lists (.d) into DIR: each source's headers, then one rule naming the CMake files the configure read (CMake's file
API; render_scene.sh places the cmakeFiles query before configuring) and each FILE.

    python dependencies.py BUILD [FILE ...]
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


def inputs_rule(build, files):
    """The make rule naming the CMake files the configure read and `files`."""
    names = [Path(name).resolve().as_posix().replace(" ", ESCAPED_SPACE) for name in (*cmake_inputs(build), *files)]
    return "host: " + " ".join(names) + chr(10)


def write_lists(build, into, files):
    """The lists --into writes."""
    _, found = entries(build)
    for number, entry in enumerate(found):
        listing = into / f"host-{number}.d"
        subprocess.run([*compile_flags(entry), "-MM", "-MF", str(listing.resolve()), entry["file"]],
                       cwd=entry["directory"], check=True)
    (into / "host-inputs.d").write_text(inputs_rule(build, files), newline=chr(10))


def main():
    build, files = Path(sys.argv[1]), sys.argv[2:]
    if files[:1] == ["--into"]:
        write_lists(build, Path(files[1]), files[2:])
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
    print(inputs_rule(build, files), end="", flush=True)


if __name__ == "__main__":
    main()
