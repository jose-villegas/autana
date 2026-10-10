"""track_host.c built once, and run over a clip.

The program is compiled into launcher/tools/anim/build/ under a name that
carries a hash of every file the compiler reads to build it (the compiler's
own list, from -MM), the compiler and the flags, so it is built again only
when one of them changes. Each build goes to a name of its own and is then
renamed into place, so runs started together on a cold cache never see a
half-written program. Older builds are left in place: another run may be
about to start one.

A clip is handed to it as a pack: either a pack and an id, as track_host
takes them, or a NAME.anim.toml, baked into a scratch pack of just that clip
by tracks_asset.py. That is the same TRCK entry the device reads, so the same
samples.

    python tools/anim/track_host.py CLIP.anim.toml [track_host's options]
    python tools/anim/track_host.py --pack PACK --clip ID [track_host's options]

Standard library only.
"""

import hashlib
import os
import pathlib
import re
import subprocess
import sys
import tempfile

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from anim import tracks_asset  # noqa: E402
from asset.asset_pack import build_pack  # noqa: E402

HERE = TOOLS / "anim"
MAIN = TOOLS.parent / "main"
BUILD = HERE / "build"
SOURCES = (HERE / "track_host.c", MAIN / "anim" / "anim_track.c", MAIN / "anim" / "anim_tracks.c",
           MAIN / "asset" / "asset_pack.c", MAIN / "asset" / "asset_file.c")
FLAGS = ("-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-O2")
EXE = ".exe" if sys.platform == "win32" else ""


class TrackHostError(RuntimeError):
    """No compiler, a failed build, or a run that failed."""


def compiler():
    """The C compiler tools/build/find_cc.sh picks."""
    find_cc = TOOLS / "build" / "find_cc.sh"
    try:
        found = subprocess.run(["sh", "-c", f'. "{find_cc.as_posix()}"; find_cc'], capture_output=True, text=True)
    except OSError as error:
        raise TrackHostError(f"no sh to run {find_cc.name}: {error}") from error
    if found.returncode != 0 or not found.stdout.strip():
        raise TrackHostError("no C compiler found; set CC")
    return found.stdout.strip()


def compile_args(cc):
    """The compile, without its output: what both the build and the key's
    file list run, so the key always sees the files the build reads."""
    return [cc, *FLAGS, "-I", str(MAIN), *map(str, SOURCES)]


def inputs(cc):
    """Every file the compiler reads to build the program, sources first."""
    listed = subprocess.run([*compile_args(cc), "-MM", "-MT", "x"], capture_output=True, text=True)
    if listed.returncode != 0:
        raise TrackHostError(f"listing track_host's headers with {cc} failed:\n{listed.stderr}")
    files = []
    for rule in listed.stdout.replace("\\\n", " ").splitlines():
        for name in re.split(r"(?<!\\) +", rule.strip().removeprefix("x:").strip()):
            path = pathlib.Path(name.replace("\\ ", " ")).resolve()
            if name and path not in files:
                files.append(path)
    return files


def build_key(cc):
    """A hash of everything the program is built from."""
    digest = hashlib.sha256()
    for path in inputs(cc):
        digest.update(path.name.encode() + b"\0" + path.read_bytes())
    digest.update("\0".join((cc, *FLAGS)).encode())
    return digest.hexdigest()[:16]


def program():
    """The built program's path, building it if this source set has none."""
    cc = compiler()
    target = BUILD / f"track_host-{build_key(cc)}{EXE}"
    if target.is_file():
        return target
    BUILD.mkdir(parents=True, exist_ok=True)
    handle, scratch = tempfile.mkstemp(prefix=f"{target.stem}.", suffix=f".tmp{EXE}", dir=BUILD)
    os.close(handle)
    scratch = pathlib.Path(scratch)
    built = subprocess.run([*compile_args(cc), "-lm", "-o", str(scratch)], capture_output=True, text=True)
    if built.returncode != 0:
        scratch.unlink(missing_ok=True)
        raise TrackHostError(f"building track_host with {cc} failed:\n{built.stderr}")
    try:
        os.replace(scratch, target)
    except OSError:
        # Another run renamed its build in first and may be running it; Windows refuses to replace that.
        scratch.unlink(missing_ok=True)
        if not target.is_file():
            raise
    return target


def run(args):
    """track_host's output for `args`, as text."""
    done = subprocess.run([str(program()), *map(str, args)], capture_output=True, text=True)
    if done.returncode != 0:
        raise TrackHostError(f"track_host {' '.join(map(str, args))}: {done.stderr.strip()}")
    return done.stdout


def clip_pack(animation):
    """A pack of just the clip a NAME.anim.toml names, under id NAME."""
    return build_pack([(tracks_asset.clip_id(animation), tracks_asset.TYPE, tracks_asset.bake(animation))])


def sample(animation, args):
    """track_host's output for `args` over the clip a NAME.anim.toml names."""
    with tempfile.TemporaryDirectory() as directory:
        pack = pathlib.Path(directory) / "clip.apak"
        pack.write_bytes(clip_pack(animation))
        return run(["--pack", pack, "--clip", tracks_asset.clip_id(animation), *args])


def poses(animation, node, every_ms, width, height, lens, near, until_ms=None, number_format=""):
    """The poses file text for camera node `node` of the clip a NAME.anim.toml
    names, every `every_ms` over the clip, stopping before `until_ms` when given;
    lens and near are written with `number_format`."""
    bounds = [] if until_ms is None else ["--until", until_ms]
    return sample(animation, ["--every", every_ms, *bounds, "--poses", node, width, height,
                              format(lens, number_format), format(near, number_format)])


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    try:
        if argv and argv[0].endswith(tracks_asset.SUFFIX):
            text = sample(argv[0], argv[1:])
        else:
            text = run(argv)
    except (TrackHostError, tracks_asset.TracksError) as error:
        print(f"track_host: {error}", file=sys.stderr)
        return 2
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
