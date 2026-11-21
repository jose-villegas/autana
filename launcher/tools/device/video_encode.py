"""Shared raw RGB to H.264 command construction and platform ffmpeg pins."""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

PIN = Path(__file__).with_name("ffmpeg-pin.json")


def pinned():
    return json.loads(PIN.read_text())["windows" if sys.platform == "win32" else "linux"]


def version(binary):
    result = subprocess.run([binary, "-version"], capture_output=True, text=True, check=True)
    match = re.search(r"ffmpeg version (\d+(?:\.\d+)*)", result.stdout)
    if not match:
        raise RuntimeError("ffmpeg version could not be read")
    return match[1]


def resolve(log):
    binary = shutil.which("ffmpeg")
    if binary is None:
        raise RuntimeError(f"ffmpeg is missing; install pinned ffmpeg {pinned()['version']} (Framework-Versions.md)")
    found, expected = version(binary), pinned()["version"]
    if found != expected:
        message = f"ffmpeg found {found}; pinned {expected}"
        if found.split(".")[0] != expected.split(".")[0]:
            raise RuntimeError(message + "; different major refused")
        print("note: " + message, file=log)
    return binary


def command(binary, size, fps, out, live=False):
    options = ["-preset", "ultrafast", "-tune", "zerolatency", "-g", str(fps)] if live else ["-preset", "medium"]
    return ([binary, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
             "-s", "%dx%d" % size, "-r", "%g" % fps, "-i", "-", "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2",
             "-c:v", "libx264", "-crf", "16"] + options
            + ["-pix_fmt", "yuv420p", "-movflags", "+faststart", str(out)])


def open_encoder(binary, size, fps, out, live=False):
    """Start an encoder accepting raw RGB frames on stdin."""
    return subprocess.Popen(command(binary, size, fps, out, live), stdin=subprocess.PIPE)
