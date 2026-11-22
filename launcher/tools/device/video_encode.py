"""Shared raw RGB to H.264 command construction and platform ffmpeg pins."""
import argparse
import hashlib
import io
import json
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build"))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts/lib"))
from espressif import pinned_tools_root
from http_get import get

FETCH_TIMEOUT_SECONDS = 120
PIN = Path(__file__).with_name("ffmpeg-pin.json")


def pinned():
    pin = json.loads(PIN.read_text())
    return {"version": pin["version"], **pin["windows" if sys.platform == "win32" else "linux"]}


def binary_name(tool):
    return tool + (".exe" if sys.platform == "win32" else "")


def install_dir():
    return pinned_tools_root() / "tools" / "ffmpeg" / pinned()["version"]


def install():
    """Verify the release archive and install its ffmpeg and ffprobe executables."""
    pin = pinned()
    target = install_dir()
    names = [binary_name(tool) for tool in ("ffprobe", "ffmpeg")]
    if all((target / name).is_file() for name in names):
        return target
    data = get(pin["url"], timeout=FETCH_TIMEOUT_SECONDS)
    actual = hashlib.sha256(data).hexdigest()
    if actual != pin["sha256"]:
        raise RuntimeError(f"ffmpeg archive SHA-256 {actual}, expected {pin['sha256']}")
    target.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=target) as directory:
        scratch = Path(directory)
        if sys.platform == "win32":
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                for name in names:
                    member = next(path for path in archive.namelist() if path.endswith("/bin/" + name))
                    with archive.open(member) as source, (scratch / name).open("wb") as out:
                        shutil.copyfileobj(source, out)
        else:
            with tarfile.open(fileobj=io.BytesIO(data), mode="r|xz") as archive:
                for member in archive:
                    path = Path(member.name)
                    if path.name in names and path.parent.name == "bin" and member.isfile():
                        with archive.extractfile(member) as source, (scratch / path.name).open("wb") as out:
                            shutil.copyfileobj(source, out)
        for name in names:
            binary = scratch / name
            binary.chmod(0o755)
            binary.replace(target / name)
    return target


def find():
    installed = install_dir() / binary_name("ffmpeg")
    return str(installed) if installed.is_file() else shutil.which("ffmpeg")


def version(binary):
    result = subprocess.run([binary, "-version"], capture_output=True, text=True, check=True)
    match = re.search(r"ffmpeg version n?(\d+(?:\.\d+)*(?:-\d+-g[0-9a-f]+)?)", result.stdout)
    if not match:
        raise RuntimeError("ffmpeg version could not be read")
    return match[1]


def resolve(log=sys.stderr):
    binary = find()
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
    group = ({"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if sys.platform == "win32"
             else {"start_new_session": True})
    return subprocess.Popen(command(binary, size, fps, out, live), stdin=subprocess.PIPE, **group)


class VideoWriter:
    """Write RGB frames of one size to an H.264 MP4."""

    def __init__(self, out, fps, live=False):
        self.out, self.fps, self.live = out, fps, live
        self.binary = resolve()
        self.process = None

    def write(self, size, rgb):
        if self.process is None:
            self.process = open_encoder(self.binary, size, self.fps, self.out, self.live)
        self.process.stdin.write(rgb)

    def add(self, picture):
        self.write(picture.size, picture.convert("RGB").tobytes())

    def close(self):
        if self.process is None:
            return
        try:
            self.process.stdin.close()
        except BrokenPipeError:
            pass
        status = self.process.wait()
        if status:
            raise RuntimeError(f"ffmpeg exited with status {status} writing {self.out}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("install",))
    parser.parse_args()
    print(install())
