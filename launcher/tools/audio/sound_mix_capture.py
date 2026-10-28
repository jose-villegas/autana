"""Capture sound-mix report windows from the flashed measurement image.

Run from the image's tree inside one autana lock hold, after flashing it.
python launcher/tools/audio/sound_mix_capture.py --tree . --board A308 --sounds NAME,NAME_FORMAT --out capture.txt
"""
import argparse
from datetime import date
import json
import re
from pathlib import Path
import subprocess
import sys
import time
import tomllib

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from autana_runner import autana_run, record_line
sys.path.insert(0, str(ROOT / "launcher/tools/build"))
from c_header import read_define
sys.path.insert(0, str(ROOT / "launcher/tools/perf"))
from frame_cost import US_PER_SECOND, MS_PER_SECOND

sys.path.insert(0, str(ROOT / "launcher/tools"))
from r3d.build_pack import pack_files

REPORT_WINDOWS = 2
STOP_ATTEMPTS = 3


def mix_stage_names(tree):
    main = tree / "launcher/main"
    mixer = read_define(main / "sound/sound_engine.h", "SOUND_MIXER_FRAME_COST_NAME")
    owner = main / "shell/shell_apps.c"
    matches = re.findall(r'FRAME_COST_END\(\s*began,\s*"([^"]+)"', owner.read_text())
    if len(matches) != 1:
        raise ValueError(f"{owner}: expected exactly one frame-cost stage")
    return [mixer, matches[0]]


def playing_count(stats):
    return sum(bool(re.search(r"\bplaying=1\b", line))
               for line in stats.splitlines() if "SOUND slot=" in line)


def dropped_count(stats):
    matches = re.findall(r"SOUND ready=[^\n]*\bdropped=(\d+)", stats)
    if len(matches) != 1:
        raise ValueError("expected exactly one SOUND ready dropped count")
    return int(matches[0])


def sound_formats(tree, names):
    packs = pack_files([tree / "launcher/main"])
    formats = {}
    for name in names:
        pack, separator, entry = name.partition("/")
        if not separator:
            pack, entry = read_define(tree / "launcher/main/asset/asset_store.h", "ASSET_ENGINE_PACK"), name
        source = packs.get(pack, {}).get(entry)
        if source is None or not source.name.endswith(".sound.toml"):
            raise ValueError(f"unknown sound {name!r}")
        formats[name] = tomllib.loads(source.read_text(encoding="utf-8"))["format"]
    return formats


def capture(args, sleep=time.sleep):
    def run(*words):
        return autana_run(args.autana, *words)

    sounds = args.sounds.split(",")
    voices = sorted(set(map(int, args.voices.split(","))))
    if not sounds or not voices or min(voices) < 0 or args.seconds <= 0:
        raise ValueError("sounds, nonnegative voices and positive seconds are required")
    fill = getattr(args, "fill_sound", None)
    formats = sound_formats(args.tree, sounds)
    fill_format = sound_formats(args.tree, [fill])[fill] if fill else None
    main = args.tree / "launcher/main"
    window = read_define(main / "shell/shell.c", "SHELL_REPORT_WINDOW_US") / US_PER_SECOND
    startup = read_define(main / "audio/audio_out_device.h", "AUDIO_OUT_AMP_STARTUP_MS") / MS_PER_SECOND
    engine = (main / "sound/sound_engine.h").read_text(encoding="utf-8")
    alias = re.search(r"^#define[ \t]+SOUND_MIXER_BLOCK_FRAMES[ \t]+([A-Za-z_]\w*)[ \t]*$",
                      engine, re.MULTILINE)
    if alias is None:
        raise ValueError("SOUND_MIXER_BLOCK_FRAMES must be one identifier alias")
    frames = read_define(main / "audio/audio_codec.h", alias[1])
    period_us = frames * US_PER_SECOND / read_define(main / "audio/audio_out.h", "AUDIO_OUT_SAMPLE_RATE_HZ")
    block_period = period_us / US_PER_SECOND
    mix_stage, present_stage = mix_stage_names(args.tree)
    capacity = sum("SOUND slot=" in line for line in run("sound", "stats").splitlines())
    if max(voices) > capacity:
        raise ValueError(f"requested {max(voices)} voices exceeds board capacity {capacity}")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="\n") as output:
        def record(key, payload):
            record_line(output, key, payload)

        output.write(f"# autana sound-mix capture on {args.board}, {date.today()}, "
                     "one record per line: <record>\t<payload>\n")
        record("buildid", json.dumps(json.loads(run("buildid", "--json"))))
        record("stage mix", mix_stage)
        record("stage present", present_stage)
        record("period_us", period_us)
        record("window_seconds", window)
        record("capacity", capacity)
        record("voices", ",".join(map(str, voices)))
        if fill:
            record("fill_sound", fill)
            record("fill_format", fill_format)
        for name, fmt in formats.items():
            record(f"sound {name}", fmt)
        silent = False
        try:
            for sound in sounds:
                for count in voices:
                    if count == 0 and silent:
                        continue
                    for _ in range(STOP_ATTEMPTS):
                        run("sound", "stop", "all")
                        sleep(block_period)
                        stats = run("sound", "stats")
                        if playing_count(stats) == 0:
                            break
                    else:
                        raise ValueError("stop all left playing voices")
                    record(f"dropped {sound} {count}", dropped_count(stats))
                    for index in range(count):
                        run("sound", "play", fill if fill and index else sound, "loop")
                    sleep(startup + REPORT_WINDOWS * window)
                    stats = run("sound", "stats")
                    active = playing_count(stats)
                    record(f"playing {sound} {count}", active)
                    if active != count:
                        raise ValueError(f"playing mismatch: expected {count}, got {active}")
                    for line in run("monitor", args.seconds, "--stream").splitlines():
                        if mix_stage in line:
                            record(f"point {sound} {count}", line)
                    record(f"dropped {sound} {count}", dropped_count(run("sound", "stats")))
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
    parser.add_argument("--sounds", required=True, help="comma-separated pack/name sounds; bare names select engine")
    parser.add_argument("--fill-sound", help="after the first voice, play this pack/name in the remaining voices")
    parser.add_argument("--voices", default="0,1,4,8,16,32")
    parser.add_argument("--seconds", type=int, default=15)
    args = parser.parse_args()
    try:
        capture(args)
    except (ValueError, KeyError, OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"{error}\n")


if __name__ == "__main__":
    main()
