"""Capture the DAC limiter sweep on a flashed development image.

The first tone record's Hz is the primary frequency for the generated table.

Run the entire capture inside one autana lock run:
autana --wait 3600 lock run -- python launcher/tools/audio/audio_drc_sweep.py --board <id>
"""
import itertools
from pathlib import Path
import math
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.c_header import read_define
sys.path.insert(0, str(Path(__file__).resolve().parent))
from autana_runner import autana_run, capture_parser, capture_records, reset_tunables

AUDIO = ROOT / "launcher/main/audio"
VOLUME_MAX = read_define(AUDIO / "audio_out.h", "AUDIO_OUT_VOLUME_MAX_PCT")
MAX_MS = read_define(AUDIO / "audio_out.h", "AUDIO_OUT_MAX_MS")
NYQUIST_HZ = read_define(AUDIO / "audio_out.h", "AUDIO_OUT_SAMPLE_RATE_HZ") // 2
DRC_FIELD_MAX = read_define(AUDIO / "audio_codec_drc.h", "AUDIO_CODEC_DRC_FIELD_MAX")
DB_AMPLITUDE_SCALE = read_define(ROOT / "launcher/packages/math/include/math/scalar/math_db.h", "MATH_DB_AMPLITUDE_SCALE")
DRC_LEVEL_DENOMINATOR = 2 * (DRC_FIELD_MAX + 1)
DRC_LEVEL_DB = tuple(DB_AMPLITUDE_SCALE * math.log10((n + 1) / DRC_LEVEL_DENOMINATOR) for n in range(DRC_FIELD_MAX + 1))
CAPTURE = ROOT / "docs/audio/data/audio_drc_capture.txt"
PRIMARY_HZ = 1000
TUNABLES = ("codec_volume", "drc", "drc_max", "drc_min", "drc_win", "drc_ramp")


def numbers(text):
    return tuple(int(value) for value in text.split(",") if value)


def parser():
    result = capture_parser(__doc__, CAPTURE)
    for option, default in (("baseline", "30,40,50,60,70,85,100"), ("codec", "70,100"),
                            ("max-levels", "1,2,3,4"), ("winsize", "4"), ("ramp", "2")):
        result.add_argument("--" + option, type=numbers, default=numbers(default))
    result.add_argument("--hz", type=int, default=PRIMARY_HZ)
    result.add_argument("--extra-hz", type=numbers, default=numbers(""))
    result.add_argument("--volumes", type=numbers, default=numbers("10,30,100"))
    result.add_argument("--ms", type=int, default=500)
    result.add_argument("--runs", type=int, default=3)
    return result


def configurations(args):
    if (not args.baseline or not args.codec or not args.max_levels or not args.winsize or not args.ramp
            or any(not 0 <= value <= VOLUME_MAX for value in (*args.baseline, *args.codec, *args.volumes))
            or any(not 0 <= value <= DRC_FIELD_MAX for value in (*args.max_levels, *args.winsize, *args.ramp))):
        raise ValueError("invalid codec volumes or DRC fields")
    for volume in args.baseline:
        yield dict(codec_volume=volume, drc=0, drc_max=0, drc_min=0, drc_win=args.winsize[0], drc_ramp=args.ramp[0])
    for volume, maximum, window, ramp in itertools.product(args.codec, args.max_levels, args.winsize, args.ramp):
        for minimum in dict.fromkeys((maximum, 0)):
            yield dict(codec_volume=volume, drc=1, drc_max=maximum, drc_min=minimum,
                       drc_win=window, drc_ramp=ramp)


def tones(args):
    yield from ((args.hz, volume) for volume in args.volumes)
    yield from ((hz, VOLUME_MAX) for hz in args.extra_hz)


def capture(args):
    configs = list(configurations(args))
    points = list(tones(args))
    if args.runs <= 0 or not 0 < args.ms <= MAX_MS or not points or any(not 0 < hz < NYQUIST_HZ for hz, _ in points):
        raise ValueError("positive runs, duration and tone frequencies are required")
    with capture_records(args, "DRC sweep", autana_run) as record:
        try:
            for config in configs:
                for name, value in config.items():
                    record("tune", "audio." + name, value)
                for hz, volume in points:
                    for _ in range(args.runs):
                        record("tone", hz, args.ms, volume, "--json")
        finally:
            reset_tunables(record, "audio", TUNABLES)



def main():
    args = parser().parse_args()
    capture(args)
    print(f"DRC capture: {args.out}")


if __name__ == "__main__":
    main()
