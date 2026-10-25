"""Probe ADC/DAC EQ encoding and the microphone HPF code map.

autana --wait 3600 lock run -- python launcher/tools/audio/mic/eq_probe_sweep.py --board <id>
"""
import math
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'launcher/tools/audio'))
from autana_runner import autana_run, capture_parser, capture_records, reset_tunables
from audio_mic_dsp_sweep import DEFAULTS, LIMITS
from audio_drc_sweep import numbers, MAX_MS, NYQUIST_HZ, VOLUME_MAX
from build.c_header import read_define
from audio.mic.mic_metrics import eq_design_biquad, eq_encode, SAMPLE_RATE_HZ
from scipy.signal import butter

HEADER = ROOT / 'launcher/main/audio/audio_codec_eq.h'
ADC_REGS = tuple(read_define(HEADER, 'AUDIO_CODEC_EQ_ADC_' + name + '_REG')
                 for name in ('B0', 'B1', 'B2', 'A1', 'A2'))
DAC_REGS = tuple(read_define(HEADER, 'AUDIO_CODEC_EQ_DAC_' + name + '_REG') for name in ('B0', 'B1', 'A1'))
ADC_CONTROL = read_define(HEADER, 'AUDIO_CODEC_EQ_ADC_CONTROL_REG')
DAC_CONTROL = read_define(HEADER, 'AUDIO_CODEC_EQ_DAC_CONTROL_REG')
ADC_BYPASS = read_define(HEADER, 'AUDIO_CODEC_EQ_ADC_BYPASS')
DAC_BYPASS = read_define(HEADER, 'AUDIO_CODEC_EQ_DAC_BYPASS')
COEFF_BYTES = read_define(HEADER, 'AUDIO_CODEC_EQ_BYTES')
REGISTERS = tuple(sorted({read_define(HEADER, 'AUDIO_CODEC_EQ_ADC_HPF_REG'), ADC_CONTROL, DAC_CONTROL,
                          *(reg + offset for reg in (*ADC_REGS, *DAC_REGS) for offset in range(COEFF_BYTES))}))
TUNABLES = tuple(DEFAULTS)
CAPTURE = ROOT / 'docs/audio/data/mic_eq_probe_capture.txt'
TAKE = ROOT / 'docs/audio/data/mic_eq_probe_take.wav'
DEFAULT_HZ = (50, 100, 150, 200, 400, 1000, 2000, 4000)
DEFAULT_HPF_HZ = (50, 100, 200, 500, 1000)
DEFAULT_CODES = tuple(range(4, 13))
DEFAULT_VOLUME = 30
DEFAULT_RUNS = 2
DEFAULT_INTERNAL_RUNS = 1
DEFAULT_MS = 500
DEFAULT_TAKE_MS = 2000
HPF_HZ = 150
LPF_HZ = 2000
DAC_HPF_HZ = 300
FLAT_GAIN = 0.5
BUTTERWORTH_Q = math.sqrt(0.5)
FIRST_ORDER = 1


def designs():
    b, a = butter(FIRST_ORDER, DAC_HPF_HZ, 'highpass', fs=SAMPLE_RATE_HZ)
    return {'adc': {'bypass': None, 'half': (FLAT_GAIN, 0, 0, 0, 0),
                    'highpass': eq_design_biquad('highpass', HPF_HZ, BUTTERWORTH_Q),
                    'lowpass': eq_design_biquad('lowpass', LPF_HZ, BUTTERWORTH_Q)},
            'dac': {'bypass': None, 'half': (FLAT_GAIN, 0, 0), 'highpass': (*b, a[1])}}


def parser():
    result = capture_parser(__doc__, CAPTURE)
    for name, default in (('hz', DEFAULT_HZ), ('hpf-hz', DEFAULT_HPF_HZ), ('codes', DEFAULT_CODES)):
        result.add_argument('--' + name, type=numbers, default=default)
    for name, default in (('volume', DEFAULT_VOLUME), ('runs', DEFAULT_RUNS),
                          ('internal-runs', DEFAULT_INTERNAL_RUNS), ('ms', DEFAULT_MS), ('take-ms', DEFAULT_TAKE_MS)):
        result.add_argument('--' + name, type=int, default=default)
    result.add_argument('--take', type=Path, default=TAKE)
    return result


def codec(record, reg, value=None):
    words = ('codec', hex(reg)) + (() if value is None else (hex(value),))
    reply = record(*words)
    match = re.fullmatch(r'CODEC reg=0x([0-9A-Fa-f]{2}) value=0x([0-9A-Fa-f]{2})', reply)
    if match is None or int(match[1], 16) != reg:
        raise ValueError('invalid codec reply: ' + reply)
    actual = int(match[2], 16)
    if value is not None and actual != value:
        raise ValueError('codec write read-back mismatch')
    return actual


def program(record, saved, owner, coefficients):
    control, bypass, registers = ((ADC_CONTROL, ADC_BYPASS, ADC_REGS) if owner == 'adc'
                                   else (DAC_CONTROL, DAC_BYPASS, DAC_REGS))
    codec(record, control, saved[control] | bypass)
    if owner == 'adc':
        record('tune', 'mic.eq', 0)
    if coefficients is not None:
        for reg, coefficient in zip(registers, coefficients):
            for offset, byte in enumerate(eq_encode(float(coefficient)).to_bytes(COEFF_BYTES, 'big')):
                codec(record, reg + offset, byte)
        codec(record, control, saved[control] & ~bypass)
        if owner == 'adc':
            record('tune', 'mic.eq', 1)


def tones(record, args, frequencies, internal=False):
    for hz in frequencies:
        for _ in range(args.internal_runs if internal else args.runs):
            record('tone', hz, args.ms, args.volume, *(('--internal',) if internal else ()), '--json')


def restore(record, saved):
    failures = []
    try:
        reset_tunables(record, 'mic', TUNABLES)
    except Exception as error:
        failures.append(error)
    for reg in (ADC_CONTROL, DAC_CONTROL):
        if reg in saved:
            try:
                codec(record, reg, saved[reg] | (ADC_BYPASS if reg == ADC_CONTROL else DAC_BYPASS))
            except Exception as error:
                failures.append(error)
    for reg in (*[r for r in REGISTERS if r not in (ADC_CONTROL, DAC_CONTROL)], ADC_CONTROL, DAC_CONTROL):
        if reg in saved:
            try:
                codec(record, reg, saved[reg])
            except Exception as error:
                failures.append(error)
    if failures:
        raise RuntimeError('EQ probe restore failed') from failures[0]


def capture(args):
    if (not args.hz or not args.hpf_hz or not args.codes
            or any(not 0 < hz < NYQUIST_HZ for hz in (*args.hz, *args.hpf_hz))
            or any(not 0 <= code <= LIMITS['hpf_s1'] for code in args.codes)
            or not 0 <= args.volume <= VOLUME_MAX or min(args.runs, args.internal_runs) <= 0
            or not 0 < args.ms <= MAX_MS or not 0 < args.take_ms <= MAX_MS):
        raise ValueError('invalid probe frequency, code, volume, duration or runs')
    with capture_records(args, 'EQ format and HPF code probe',
                         lambda command, *words: autana_run(command, '--board', args.board, *words)) as record:
        saved = {}
        try:
            record('status')
            for reg in REGISTERS:
                saved[reg] = codec(record, reg)
            for name, value in dict(DEFAULTS, eq=0).items():
                record('tune', 'mic.' + name, value)
            program(record, saved, 'dac', None)
            for name, coefficients in designs()['adc'].items():
                record.note('probe adc ' + name, {})
                program(record, saved, 'adc', coefficients)
                tones(record, args, args.hz)
            program(record, saved, 'adc', None)
            for code in args.codes:
                for name in ('hpf_s1', 'hpf_s2'):
                    record('tune', 'mic.' + name, code)
                record.note('probe hpf ' + str(code), {})
                tones(record, args, args.hpf_hz)
            for name in ('hpf_s1', 'hpf_s2'):
                record('tune', 'mic.' + name, DEFAULTS[name])
            for name, coefficients in designs()['dac'].items():
                record.note('probe dac ' + name, {})
                program(record, saved, 'dac', coefficients)
                tones(record, args, args.hz)
                tones(record, args, args.hz, internal=True)
            program(record, saved, 'dac', None)
            program(record, saved, 'adc', designs()['adc']['highpass'])
            args.take.parent.mkdir(parents=True, exist_ok=True)
            record('mic', 'record', args.take_ms, '-o', str(args.take), '--json')
        finally:
            restore(record, saved)


def main():
    args = parser().parse_args()
    capture(args)
    print(f'EQ probe capture: {args.out}')


if __name__ == '__main__':
    main()
