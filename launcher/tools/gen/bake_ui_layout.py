#!/usr/bin/env python3
"""Build and run the headless C++ layout baker on Linux or Windows."""
import pathlib
import shutil
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[3]
build = root / "editor/build-bake"
if not (build / "CMakeCache.txt").is_file():
    subprocess.run(["cmake", "-S", str(root / "editor"), "-B", str(build),
                    "-DEDITOR_BUILD_GUI=OFF", "-DBUILD_TESTING=OFF",
                    "-DCMAKE_BUILD_TYPE=Release"], check=True)
subprocess.run(["cmake", "--build", str(build), "--target", "editor_layout_bake",
                "--config", "Release"], check=True)
baker = shutil.which("editor_layout_bake", path=str(build))
if baker is None:
    sys.exit(f"CMake did not produce editor_layout_bake in {build}")
sys.exit(subprocess.call([baker, *sys.argv[1:]]))
