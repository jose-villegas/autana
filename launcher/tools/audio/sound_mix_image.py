"""Prepare a detached scratch engine image for sound-mix measurements.

Create it with git worktree add --detach SCRATCH HEAD, then run:
python launcher/tools/audio/sound_mix_image.py SCRATCH --voices 32
Hold one autana lock across autana --project SCRATCH flash dev and capture.
Run sound_mix_capture.py from SCRATCH with --tree SCRATCH in that hold.
Only the scratch image contains tap_pcm16 and the optional capacity override.
"""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
ASSET = Path("launcher/main/engine/sounds/tap_pcm16.sound.toml")
HEADER = Path("launcher/main/sound/sound_play.h")
VOICE_DEFINE = re.compile(rb"^#define[ \t]+SOUND_VOICE_COUNT\b[^\r\n]*", re.MULTILINE)


def prepare(tree, voices=None):
    tree = Path(tree).resolve()
    if tree == ROOT:
        raise ValueError("refusing this checkout; use a detached scratch tree")
    if voices is not None and voices <= 0:
        raise ValueError("voices must be positive")
    status = subprocess.run(["git", "-C", str(tree), "status", "--porcelain", "--untracked-files=all"],
                            check=True, capture_output=True, text=True).stdout
    allowed = {ASSET.as_posix(), HEADER.as_posix()}
    if any(line[3:] not in allowed for line in status.splitlines()):
        raise ValueError("scratch tree has changes outside measurement files")
    header = tree / HEADER
    raw = header.read_bytes()
    if len(VOICE_DEFINE.findall(raw)) != 1:
        raise ValueError("expected exactly one SOUND_VOICE_COUNT define")
    if voices is not None:
        raw = VOICE_DEFINE.sub(f"#define SOUND_VOICE_COUNT {voices}".encode(), raw)
    (tree / ASSET).write_bytes(b'source = "tap.wav"\nformat = "pcm16"\n')
    if voices is not None:
        header.write_bytes(raw)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tree", type=Path)
    parser.add_argument("--voices", type=int)
    args = parser.parse_args()
    try:
        prepare(args.tree, args.voices)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    main()
