"""Capture ADC HPF and EQ arms with scheduled acoustic bursts and speech tones."""
from pathlib import Path
import sys
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from audio.mic.eq_probe_sweep import (ROOT, REGISTERS, DEFAULTS, BUTTERWORTH_Q, codec, program,
                                     restore, tones, capture_parser, capture_records, autana_run)
from audio.mic.eq_design import eq_design_biquad

CAPTURE = ROOT / 'docs/audio/data/hpf_taps/mic_hpf_tap_capture.txt'
RUNS = 3
TAKE_MS = 4000
SETTLE_MS = 500
# 40 Hz is a spectral proxy for handling noise below 100 Hz; the speaker cannot shake the board at 40 Hz.
THUMP_HZ, THUMP_MS, THUMP_GAIN = 40, 80, 100
# 202 Hz is the haptic frequency chosen in pkwo8.3; 60 ms at 60% is the firm pulse.
HAPTIC_HZ, HAPTIC_MS, HAPTIC_GAIN = 202, 60, 60
THUMP_AT = (400, 1100, 1800, 2500, 3200)
HAPTIC_AT = (750, 1450, 2150, 2850, 3550)
SPEECH_HZ = (300, 500, 1000, 2000, 3400)
TONE_MS, TONE_GAIN = 500, 30
REFERENCE_CODE = 12
EQ_CODE = 10
CODES = (REFERENCE_CODE, 4, 5, 6, 7, 8, EQ_CODE)
EQ_CUTOFFS = (150, 100)


def parser():
    result = capture_parser(__doc__, CAPTURE)
    result.add_argument('--runs', type=int, default=RUNS)
    return result


def arms():
    return [dict(code=code, cutoff=0) for code in CODES] + [dict(code=EQ_CODE, cutoff=hz) for hz in EQ_CUTOFFS]


def schedule():
    return sorted([(at, THUMP_HZ, THUMP_MS, THUMP_GAIN) for at in THUMP_AT]
                  + [(at, HAPTIC_HZ, HAPTIC_MS, HAPTIC_GAIN) for at in HAPTIC_AT])


def capture(args):
    if args.runs <= 0:
        raise ValueError('runs must be positive')
    with capture_records(args, 'HPF tap capture',
                         lambda command, *words: autana_run(command, '--board', args.board, *words)) as record:
        saved = {}
        try:
            record('status')
            for reg in REGISTERS:
                saved[reg] = codec(record, reg)
            for name, value in DEFAULTS.items():
                record('tune', 'mic.' + name, value)
            record('tune', 'audio.drc', 0)
            program(record, saved, 'dac', None)
            record.note('runs', args.runs)
            for index, arm in enumerate(arms()):
                record.note('arm', dict(arm, index=index))
                record('tune', 'mic.hpf', 1)
                for stage in ('hpf_s1', 'hpf_s2'):
                    record('tune', 'mic.' + stage, arm['code'])
                program(record, saved, 'adc', eq_design_biquad('highpass', arm['cutoff'], BUTTERWORTH_Q)
                        if arm['cutoff'] else None)
                for run in range(args.runs):
                    path = args.out.with_name(f'hpf-{index:02d}-{run:02d}.wav')
                    words = [word for at, hz, ms, gain in schedule() for word in ('--at', at, 'tone', hz, ms, gain)]
                    record('mic', 'record', TAKE_MS, '--settle', SETTLE_MS, *words, '-o', str(path), '--json')
                    tones(record, SimpleNamespace(runs=1, ms=TONE_MS, volume=TONE_GAIN), SPEECH_HZ)
            record('buildid', '--json')
        finally:
            try:
                record('sound', 'stop', 'all')
            finally:
                restore(record, saved)


def main():
    args = parser().parse_args()
    capture(args)
    print(f'HPF tap capture: {args.out}')


if __name__ == '__main__':
    main()
