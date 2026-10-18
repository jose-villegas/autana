"""Prepare a detached scratch engine image for sound-mix measurements.

Create it with git worktree add --detach SCRATCH HEAD, then run:
python launcher/tools/audio/sound_mix_image.py SCRATCH --sound tap --format pcm16 --voices 32
Hold one autana lock across autana --project SCRATCH flash dev and capture.
Run sound_mix_capture.py from SCRATCH with --tree SCRATCH in that hold.
Only the scratch image contains the format variant and optional capacity override.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[3]
SOUNDS = Path("launcher/main/engine/sounds")
HEADER = Path("launcher/main/sound/sound_play.h")
VOICE_DEFINE = re.compile(rb"^#define[ \t]+SOUND_VOICE_COUNT\b[^\r\n]*", re.MULTILINE)


def prepare(tree, sound, fmt, voices=None):
    tree = Path(tree).resolve()
    if tree == ROOT:
        raise ValueError("refusing this checkout; use a detached scratch tree")
    if voices is not None and voices <= 0:
        raise ValueError("voices must be positive")
    if not re.fullmatch(r"[a-zA-Z0-9_-]+", sound) or not re.fullmatch(r"[a-zA-Z0-9_-]+", fmt):
        raise ValueError("invalid sound or format name")
    source = tree / SOUNDS / f"{sound}.sound.toml"
    if not source.is_file():
        raise ValueError(f"missing sound source: {source}")
    raw_asset = source.read_bytes()
    if tomllib.loads(raw_asset.decode("utf-8"))["format"] == fmt:
        raise ValueError(f"sound already uses {fmt}")
    asset = SOUNDS / f"{sound}_{fmt}.sound.toml"
    status = subprocess.run(["git", "-C", str(tree), "status", "--porcelain", "--untracked-files=all"],
                            check=True, capture_output=True, text=True).stdout
    allowed = {asset.as_posix()}
    if voices is not None:
        allowed.add(HEADER.as_posix())
    if any(line[3:] not in allowed for line in status.splitlines()):
        raise ValueError("scratch tree has changes outside measurement files")
    pattern = rb'(?m)^(format[ \t]*=[ \t]*)"[^"\r\n]*"'
    replaced, count = re.subn(pattern, lambda match: match[1] + f'"{fmt}"'.encode(), raw_asset)
    if count != 1:
        raise ValueError("expected exactly one format field")
    if voices is not None:
        header = tree / HEADER
        raw = header.read_bytes()
        if len(VOICE_DEFINE.findall(raw)) != 1:
            raise ValueError("expected exactly one SOUND_VOICE_COUNT define")
        header.write_bytes(VOICE_DEFINE.sub(f"#define SOUND_VOICE_COUNT {voices}".encode(), raw))
    (tree / asset).write_bytes(replaced)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tree", type=Path)
    parser.add_argument("--sound", required=True)
    parser.add_argument("--format", required=True)
    parser.add_argument("--voices", type=int)
    args = parser.parse_args()
    try:
        prepare(args.tree, args.sound, args.format, args.voices)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    main()
