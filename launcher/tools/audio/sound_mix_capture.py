"""Capture sound-mix report windows from the flashed measurement image.

Run from the image's tree inside one autana lock hold, after flashing it.
python launcher/tools/audio/sound_mix_capture.py --tree . --board A308 --out capture.txt
"""
import argparse
from datetime import date
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
import tomllib

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools/build"))
from c_header import read_define

REPORT_WINDOWS = 2
US_PER_SECOND = 1_000_000
MS_PER_SECOND = 1000


def capture(args, sleep=time.sleep):
    def run(*command):
        return subprocess.run([shutil.which(args.autana) or args.autana, *map(str, command)], check=True,
                              capture_output=True, text=True).stdout

    sounds = args.sounds.split(",")
    voices = sorted(set(map(int, args.voices.split(","))))
    if not sounds or not voices or min(voices) < 0 or args.seconds <= 0:
        raise ValueError("sounds, nonnegative voices and positive seconds are required")
    formats = {name: tomllib.loads((args.tree / "launcher/main/engine/sounds" /
                                  f"{name}.sound.toml").read_text())["format"] for name in sounds}
    main = args.tree / "launcher/main"
    window = read_define(main / "shell/shell.c", "SHELL_REPORT_WINDOW_US") / US_PER_SECOND
    startup = read_define(main / "audio/audio_out_device.h", "AUDIO_OUT_AMP_STARTUP_MS") / MS_PER_SECOND
    idle = read_define(main / "sound/sound_mixer.h", "SOUND_AMP_IDLE_MS") / MS_PER_SECOND
    capacity = sum("SOUND slot=" in line for line in run("sound", "stats").splitlines())
    if max(voices) > capacity:
        raise ValueError(f"requested {max(voices)} voices exceeds board capacity {capacity}")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="\n") as output:
        def record(key, payload):
            output.write(f"{key}\t{payload}\n")
            output.flush()

        output.write(f"# autana sound-mix capture on {args.board}, {date.today()}, "
                     "one record per line: <record>\t<payload>\n")
        record("buildid", json.dumps(json.loads(run("buildid", "--json"))))
        record("capacity", capacity)
        record("voices", ",".join(map(str, voices)))
        for name, fmt in formats.items():
            record(f"sound {name}", fmt)
        silent = False
        try:
            for sound in sounds:
                for count in voices:
                    if count == 0 and silent:
                        continue
                    run("sound", "stop", "all")
                    for _ in range(count):
                        run("sound", "play", sound, "loop")
                    sleep((startup if count else idle) + REPORT_WINDOWS * window)
                    for line in run("monitor", args.seconds, "--stream").splitlines():
                        if "sound_mix" in line:
                            record(f"point {sound} {count}", line)
                    silent |= count == 0
            for line in run("sound", "stats").splitlines():
                record(f"stats {sounds[-1]} {voices[-1]}", line)
        finally:
            run("sound", "stop", "all")
        record("buildid", json.dumps(json.loads(run("buildid", "--json"))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--autana", default="autana")
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--board", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--sounds", default="tap,tap_pcm16")
    parser.add_argument("--voices", default="0,1,4,8,16,32")
    parser.add_argument("--seconds", type=int, default=15)
    args = parser.parse_args()
    try:
        capture(args)
    except (ValueError, KeyError, OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    main()
