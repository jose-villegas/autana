"""Runs render_lab_render_host.sh for the tests in this folder and names the renderer it builds."""
import os
import pathlib
import shutil
import subprocess

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "render_lab_render_host.sh"


def run(out, *args):
    """(the script's run into `out`, the renderer binary there). The pinned hashes belong to the compiler that
    pinned them, and these tests compare renders with each other or count their pixels, so no baseline is read."""
    env = dict(os.environ, scene_baseline=str(pathlib.Path(out) / "no-baseline.txt"))
    shell = shutil.which("sh") or shutil.which("bash")
    result = subprocess.run([shell, str(SCRIPT), "-o", str(out), *args], capture_output=True, text=True, timeout=900,
                            env=env)
    return result, pathlib.Path(out) / ("render_lab_render.exe" if os.name == "nt" else "render_lab_render")
