#!/usr/bin/env python3
"""Run the C++ layout baker from editor/build on Linux or Windows."""
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[3]
build = root / "editor/build"
candidates = [build / "editor_layout_bake", build / "editor_layout_bake.exe",
              build / "Debug/editor_layout_bake.exe", build / "Release/editor_layout_bake.exe"]
baker = next((path for path in candidates if path.is_file()), None)
if baker is None:
    sys.exit("Build editor_layout_bake in editor/build first (see editor/README.md)")
sys.exit(subprocess.call([str(baker), *sys.argv[1:]]))
