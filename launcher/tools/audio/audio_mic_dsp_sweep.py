"""Capture microphone DSP settings on a flashed development image.

Hold the board for the complete sweep:
autana --wait 3600 lock run -- python launcher/tools/audio/audio_mic_dsp_sweep.py --board <id>
"""
import argparse
import itertools
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.c_header import read_define
sys.path.insert(0, str(Path(__file__).resolve().parent))
from autana_runner import autana_run, capture_parser, capture_records, reset_tunables
from audio_drc_sweep import numbers, MAX_MS, NYQUIST_HZ, VOLUME_MAX

HEADER = ROOT / "launcher/main/audio/audio_codec_mic.h"
CAPTURE = ROOT / "docs/audio/data/audio_mic_dsp_capture.txt"
PRIMARY_HZ = 1000
HPF_VOLUME = 30
FIELDS = {
    "eq": ("OFF", 1), "alc": ("OFF", 1), "alc_max": ("FIELD", "NIBBLE"), "alc_min": ("FIELD", "NIBBLE"),
    "alc_win": ("FIELD", "NIBBLE"), "alc_gain": ("VOLUME", "GAIN"), "gate": ("OFF", 1),
    "gate_ng": ("FIELD", "NIBBLE"), "gate_win": ("FIELD", "NIBBLE"), "gate_depth": ("FIELD", "DEPTH"),
    "hpf": ("DYNAMIC", 1), "hpf_s1": ("HPF", "HPF"), "hpf_s2": ("HPF", "HPF"),
}
DEFAULTS = {name: read_define(HEADER, "AUDIO_CODEC_MIC_" + default + "_DEFAULT")
            for name, (default, _) in FIELDS.items()}
LIMITS = {name: limit if isinstance(limit, int) else read_define(HEADER, "AUDIO_CODEC_MIC_" + limit + "_MAX")
          for name, (_, limit) in FIELDS.items()}
LEVEL_DB = read_define(HEADER, "AUDIO_CODEC_MIC_LEVEL_DB")
GATE_DB = read_define(HEADER, "AUDIO_CODEC_MIC_GATE_DB")


def pairs(text):
    values = tuple(numbers(pair.replace(":", ",")) for pair in text.split(";"))
    if any(len(pair) != 2 for pair in values):
        raise argparse.ArgumentTypeError("pairs use max:min;max:min")
    return values


def parser():
    result = capture_parser(__doc__, CAPTURE)
    for name, default in (("volumes", "2,5,10,20,30,50,70,100"), ("hpf-coefficients", "4,10,16,24,31"),
                          ("hpf-hz", "50,100,200,1000"), ("gate-thresholds", "2,6,10"),
                          ("gate-windows", "0,4"), ("alc-windows", "2,6"), ("alc-gains", "191,215,239")):
        result.add_argument("--" + name, type=numbers, default=numbers(default))
    result.add_argument("--alc-targets", type=pairs, default=pairs("4:2;8:6;12:10"))
    result.add_argument("--gate-depth", type=int, default=LIMITS["gate_depth"])
    result.add_argument("--hpf-volume", type=int, default=HPF_VOLUME)
    result.add_argument("--hz", type=int, default=PRIMARY_HZ)
    result.add_argument("--ms", type=int, default=500)
    result.add_argument("--runs", type=int, default=3)
    return result


def validate_config(config):
    if config.keys() != DEFAULTS.keys() or any(not 0 <= value <= LIMITS[name] for name, value in config.items()):
        raise ValueError("invalid microphone DSP configuration")


def configurations(args):
    yield dict(DEFAULTS), False
    yield dict(DEFAULTS, hpf=0), False
    for coefficient in args.hpf_coefficients:
        yield dict(DEFAULTS, hpf_s1=coefficient, hpf_s2=coefficient), True
    for threshold, window in itertools.product(args.gate_thresholds, args.gate_windows):
        yield dict(DEFAULTS, gate=1, gate_ng=threshold, gate_win=window, gate_depth=args.gate_depth), False
    for (maximum, minimum), window, gain in itertools.product(args.alc_targets, args.alc_windows, args.alc_gains):
        yield dict(DEFAULTS, alc=1, alc_max=maximum, alc_min=minimum, alc_win=window, alc_gain=gain), False


def capture(args):
    configs = list(configurations(args))
    for config, _ in configs:
        validate_config(config)
    if (args.runs <= 0 or not 0 < args.ms <= MAX_MS or not args.volumes or not args.hpf_hz
            or any(not 0 < hz < NYQUIST_HZ for hz in (args.hz, *args.hpf_hz))
            or any(not 0 <= volume <= VOLUME_MAX for volume in (*args.volumes, args.hpf_volume))):
        raise ValueError("invalid tone frequency, volume, duration or run count")
    with capture_records(args, "microphone DSP sweep", autana_run) as record:
        try:
            for config, frequency_sweep in configs:
                for name, value in config.items():
                    record("tune", "mic." + name, value)
                points = ((hz, args.hpf_volume) for hz in args.hpf_hz) if frequency_sweep else (
                    (args.hz, volume) for volume in args.volumes)
                for hz, volume in points:
                    for _ in range(args.runs):
                        record("tone", hz, args.ms, volume, "--json")
        finally:
            reset_tunables(record, "mic", DEFAULTS)



def main():
    args = parser().parse_args()
    capture(args)
    print(f"Microphone DSP capture: {args.out}")


if __name__ == "__main__":
    main()
