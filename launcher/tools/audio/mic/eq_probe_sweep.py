"""Probe ADC/DAC EQ encoding and the microphone HPF code map.

autana --wait 3600 lock run -- python launcher/tools/audio/mic/eq_probe_sweep.py --board <id>
Playback: autana --wait 3600 lock run -- python launcher/tools/audio/mic/playback_dsp_capture.py --board <id>
"""
import math
from statistics import median
from pathlib import Path
import re
import sys
import argparse
import time

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'launcher/tools/audio'))
from autana_runner import autana_run, capture_parser, capture_records, reset_tunables
from audio_mic_dsp_sweep import DEFAULTS, LIMITS
from audio.sound_asset import SAMPLE_RATE_HZ
from audio_drc_sweep import numbers, MAX_MS, NYQUIST_HZ, VOLUME_MAX
from build.c_header import read_define
from audio.mic.eq_design import (
    eq_design_biquad, eq_design_first_order_highpass, eq_design_first_order_shelf, eq_encode, eq_classify_slots, eq_flat_slot,
    SLOT_HZ, SLOT_VALUES, SCALE_VALUES, SCALE_HZ, ORDER_HZ, PROBE_GAIN, eq_decide_layout,
    eq_layout_coefficients, NUMERATOR_COEFFICIENTS,
)

HEADER = ROOT / 'launcher/main/audio/audio_codec_eq.h'
CSM_REG, CSM_ON, CSM_RESETS = (read_define(HEADER, 'AUDIO_CODEC_EQ_CSM_' + name)
                                for name in ('REG', 'ON', 'RESETS'))
RESTART_WAIT_S = 0.005
RESTART_HZ = (100, 1000, 4000)
ADC_REGS = tuple(read_define(HEADER, 'AUDIO_CODEC_EQ_ADC_' + name + '_REG')
                 for name in ('B0', 'B1', 'B2', 'A1', 'A2'))
DAC_REGS = tuple(read_define(HEADER, 'AUDIO_CODEC_EQ_DAC_' + name + '_REG') for name in ('B0', 'B1', 'A1'))
ADC_CONTROL = read_define(HEADER, 'AUDIO_CODEC_EQ_ADC_CONTROL_REG')
DAC_CONTROL = read_define(HEADER, 'AUDIO_CODEC_EQ_DAC_CONTROL_REG')
ADC_BYPASS = read_define(HEADER, 'AUDIO_CODEC_EQ_ADC_BYPASS')
DAC_BYPASS = read_define(HEADER, 'AUDIO_CODEC_EQ_DAC_BYPASS')
RAM_FIELDS = {owner: (read_define(HEADER, 'AUDIO_CODEC_EQ_' + owner.upper() + '_RAM_REG'),
                       read_define(HEADER, 'AUDIO_CODEC_EQ_' + owner.upper() + '_RAMCLR'))
              for owner in ('adc', 'dac')}
COEFF_BYTES = read_define(HEADER, 'AUDIO_CODEC_EQ_BYTES')
REGISTERS = tuple(sorted({read_define(HEADER, 'AUDIO_CODEC_EQ_ADC_HPF_REG'), ADC_CONTROL, DAC_CONTROL, CSM_REG,
                          *(reg for reg, mask in RAM_FIELDS.values()),
                          *(reg + offset for reg in (*ADC_REGS, *DAC_REGS) for offset in range(COEFF_BYTES))}))
TUNABLES = tuple(DEFAULTS)
CAPTURE = ROOT / 'docs/audio/data/mic_eq_probe_capture.txt'
SLOTS_CAPTURE = CAPTURE.with_name('mic_eq_slots_capture.txt')
RESTART_CAPTURE = CAPTURE.with_name('mic_eq_restart_capture.txt')
CSD_CAPTURE = CAPTURE.with_name('mic_eq_csd_capture.txt')
TAKE_NAME = 'mic_eq_probe_take.wav'
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
VOICE_LOW_HZ, VOICE_HIGH_HZ = 300, 3400
CSD_DAC_HPF_HZ = 200
SHELF_HZ, SHELF_GAIN_DB = 250, -20
CSD_PASSBAND_HZ = {'adc': {'csd-highpass': (400, 1000, 2000, 4000),
                           'csd-voice': (400, 1000, 2000)},
                   'dac': {'csd-lowcut': (400, 1000, 2000, 4000),
                           'csd-shelf': (1000, 2000, 4000)}}
CSD_TOLERANCE_DB = 3.0


def designs():
    return {'adc': {'bypass': None, 'half': (FLAT_GAIN, 0, 0, 0, 0),
                    'highpass': eq_design_biquad('highpass', HPF_HZ, BUTTERWORTH_Q),
                    'lowpass': eq_design_biquad('lowpass', LPF_HZ, BUTTERWORTH_Q)},
            'dac': {'bypass': None, 'half': (FLAT_GAIN, 0, 0), 'highpass': eq_design_first_order_highpass(DAC_HPF_HZ)}}


def csd_designs():
    low, high = (math.tan(math.pi * hz / SAMPLE_RATE_HZ) for hz in (VOICE_LOW_HZ, VOICE_HIGH_HZ))
    center = math.sqrt(low * high)
    return {'adc': {'bypass': None,
                    'csd-highpass': eq_design_biquad('highpass', HPF_HZ, BUTTERWORTH_Q),
                    'csd-voice': eq_design_biquad('bandpass', SAMPLE_RATE_HZ * math.atan(center) / math.pi,
                                                center / (high - low))},
            'dac': {'bypass': None, 'csd-lowcut': eq_design_first_order_highpass(CSD_DAC_HPF_HZ),
                    'csd-shelf': eq_design_first_order_shelf(SHELF_HZ, SHELF_GAIN_DB)}}


def csd_capture(record, saved, args):
    record('tune', 'audio.drc', 0)
    for owner, cases in csd_designs().items():
        program(record, saved, 'dac' if owner == 'adc' else 'adc', None)
        for case, coefficients in cases.items():
            record.note('probe ' + owner + ' ' + case, {})
            program(record, saved, owner, coefficients)
            restart(record, saved)
            tones(record, args, DEFAULT_HZ)
            if owner == 'dac':
                tones(record, args, DEFAULT_HZ, internal=True)


def parser():
    result = capture_parser(__doc__, CAPTURE)
    result.formatter_class = argparse.RawDescriptionHelpFormatter
    for name, default in (('hz', DEFAULT_HZ), ('hpf-hz', DEFAULT_HPF_HZ), ('codes', DEFAULT_CODES)):
        result.add_argument('--' + name, type=numbers, default=default)
    for name, default in (('volume', DEFAULT_VOLUME), ('runs', DEFAULT_RUNS),
                          ('internal-runs', DEFAULT_INTERNAL_RUNS), ('ms', DEFAULT_MS), ('take-ms', DEFAULT_TAKE_MS)):
        result.add_argument('--' + name, type=int, default=default)
    result.add_argument('--mode', choices=('designs', 'slots', 'restart', 'csd'), default='designs')
    result.add_argument('--take', type=Path, help='default: beside --out')
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


def restart(record, saved):
    reg00 = codec(record, CSM_REG)
    values = ((reg00 & ~CSM_ON) | CSM_RESETS,
              reg00 & ~(CSM_ON | CSM_RESETS), saved[CSM_REG] | CSM_ON)
    for value in values:
        codec(record, CSM_REG, value)
        time.sleep(RESTART_WAIT_S)
    record.note('restart', {'values': values})


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
        ram_reg, ram_mask = RAM_FIELDS[owner]
        ram = codec(record, ram_reg) & ~ram_mask
        codec(record, ram_reg, ram | ram_mask)
        codec(record, ram_reg, ram)
        record.note('ram-clear ' + owner, {'register': ram_reg, 'mask': ram_mask})


def tones(record, args, frequencies, internal=False):
    levels = {}
    for hz in frequencies:
        for _ in range(args.internal_runs if internal else args.runs):
            reply = record('tone', hz, args.ms, args.volume, *(('--internal',) if internal else ()), '--json')
            levels.setdefault(hz, []).append(reply['tone_db'])
    return {hz: median(values) for hz, values in levels.items()}


def restore(record, saved, tunables=None):
    failures = []
    try:
        reset_tunables(record, 'mic', TUNABLES)
    except Exception as error:
        failures.append(error)
    try:
        reset_tunables(record, 'audio', ('drc',))
    except Exception as error:
        failures.append(error)
    for reg in (ADC_CONTROL, DAC_CONTROL):
        if reg in saved:
            try:
                codec(record, reg, saved[reg] | (ADC_BYPASS if reg == ADC_CONTROL else DAC_BYPASS))
            except Exception as error:
                failures.append(error)
    for reg in (*[r for r in REGISTERS if r not in (ADC_CONTROL, DAC_CONTROL, CSM_REG)], ADC_CONTROL, DAC_CONTROL, CSM_REG):
        if reg in saved:
            try:
                codec(record, reg, saved[reg])
            except Exception as error:
                failures.append(error)
    if CSM_REG in saved:
        try:
            restart(record, saved)
            if tunables is not None:
                codec(record, CSM_REG, saved[CSM_REG])
        except Exception as error:
            failures.append(error)
    for name, value in (tunables or {}).items():
        try:
            record('tune', name, value)
        except Exception as error:
            failures.append(error)
    if failures:
        raise RuntimeError('EQ probe restore failed') from failures[0]


def restart_designs(owner):
    count = len(ADC_REGS if owner == 'adc' else DAC_REGS)
    return {'unity': (1.0, *([0.0] * (count - 1))), 'zero': (0.0,) * count,
            'half': designs()[owner]['half'], 'highpass': designs()[owner]['highpass']}


def restart_capture(record, saved, args):
    for owner, registers in (('adc', ADC_REGS), ('dac', DAC_REGS)):
        program(record, saved, 'dac' if owner == 'adc' else 'adc', None)
        record('tune', 'audio.drc', 0)
        record.note('probe ' + owner + ' bypass', {})
        program(record, saved, owner, None)
        tones(record, args, RESTART_HZ)
        if owner == 'dac':
            tones(record, args, RESTART_HZ, internal=True)
        for case, coefficients in restart_designs(owner).items():
            record.note('probe ' + owner + ' ' + case, {})
            program(record, saved, owner, coefficients)
            restart(record, saved)
            readback = {hex(reg + offset): codec(record, reg + offset)
                        for reg in registers for offset in range(COEFF_BYTES)}
            record.note('coefficients ' + owner + ' ' + case, readback)
            tones(record, args, RESTART_HZ)
            if owner == 'dac':
                tones(record, args, RESTART_HZ, internal=True)


def slot_capture(record, saved, args):
    for owner, registers in (('adc', sorted(ADC_REGS)), ('dac', sorted(DAC_REGS))):
        program(record, saved, 'adc' if owner == 'dac' else 'dac', None)
        if owner == 'dac':
            record('tune', 'audio.drc', 0)
        record.note('probe ' + owner + ' bypass', {})
        program(record, saved, owner, None)
        baseline = tones(record, args, sorted(set((*SLOT_HZ, *ORDER_HZ, *args.hz))))
        cases = [('zero', [0.0] * len(registers), SLOT_HZ)]
        unity = [0.0] * len(registers)
        unity[0] = 1.0
        cases.append(('unity', unity, SLOT_HZ))
        for case, coefficients, frequencies in cases:
            record.note('probe ' + owner + ' ' + case, {})
            program(record, saved, owner, coefficients)
            tones(record, args, frequencies)
        points = {}
        for index, reg in enumerate(registers):
            points[reg] = {}
            for value in SLOT_VALUES:
                coefficients = [0.0] * len(registers)
                coefficients[index] = value
                record.note(f'probe {owner} slot-{reg:02x}-{value:g}', {})
                ordered = dict(zip(registers, coefficients))
                program(record, saved, owner, [ordered[r] for r in (ADC_REGS if owner == 'adc' else DAC_REGS)])
                levels = tones(record, args, SLOT_HZ)
                points[reg][value] = {hz: level - baseline[hz] for hz, level in levels.items()}
        roles = eq_classify_slots(points)
        record.note('layout ' + owner, {hex(reg): role for reg, role in roles.items()})
        flat = eq_flat_slot(points)
        if flat is None:
            record.note('unresolved ' + owner, {'reason': 'no non-silent flat slot'})
            continue
        for value in SCALE_VALUES:
            coefficients = [value if reg == flat else 0.0 for reg in (ADC_REGS if owner == 'adc' else DAC_REGS)]
            record.note(f'probe {owner} scale-{flat:02x}-{value:g}', {})
            program(record, saved, owner, coefficients)
            tones(record, args, (SCALE_HZ,))
        order_capture(record, saved, args, owner, points, baseline)


def order_capture(record, saved, args, owner, points, baseline):
    roles = eq_classify_slots(points)
    reference = next((reg for reg, role in sorted(roles.items()) if role == 'numerator'), None)
    if reference is None or 'unresolved' in roles.values():
        record.note('unresolved ' + owner, {'reason': 'slot membership unresolved; rerun skipped'})
        return
    registers = ADC_REGS if owner == 'adc' else DAC_REGS
    numerator_count = NUMERATOR_COEFFICIENTS[owner]
    record.note('reference ' + owner, {'b0_register': reference, 'convention': 'lowest numerator register'})
    order_points = {}
    for reg, role in sorted(roles.items()):
        if reg == reference:
            continue
        case = ('delay' if role == 'numerator' else 'feedback') + f'-{reg:02x}'
        record.note(f'probe {owner} {case}', {})
        program(record, saved, owner, [PROBE_GAIN if r in (reference, reg) else 0.0 for r in registers])
        levels = tones(record, args, ORDER_HZ)
        order_points[reg] = {hz: level - baseline[hz] for hz, level in levels.items()}
    layout = eq_decide_layout(points, order_points, numerator_count)
    record.note('decided-layout ' + owner, {hex(reg): role for reg, role in layout.items()} if layout else {})
    if layout is None:
        record.note('unresolved ' + owner, {'reason': 'order/sign ambiguous; rerun skipped'})
        return
    for case in ('highpass', 'lowpass') if owner == 'adc' else ('highpass',):
        coefficients = eq_layout_coefficients(registers, designs()[owner][case], layout, numerator_count)
        record.note(f'probe {owner} rerun-{case}', {})
        program(record, saved, owner, coefficients)
        tones(record, args, args.hz)


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
            if args.mode == 'csd':
                csd_capture(record, saved, args)
                return
            if args.mode == 'restart':
                restart_capture(record, saved, args)
                return
            if args.mode == 'slots':
                slot_capture(record, saved, args)
                return
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
            record('tune', 'audio.drc', 0)
            for name, coefficients in designs()['dac'].items():
                record.note('probe dac ' + name, {})
                program(record, saved, 'dac', coefficients)
                tones(record, args, args.hz)
                tones(record, args, args.hz, internal=True)
            program(record, saved, 'dac', None)
            program(record, saved, 'adc', designs()['adc']['highpass'])
            args.take = args.take or args.out.with_name(TAKE_NAME)
            args.take.parent.mkdir(parents=True, exist_ok=True)
            record('mic', 'record', args.take_ms, '-o', str(args.take), '--json')
        finally:
            restore(record, saved)


def main():
    args = parser().parse_args()
    if args.out == CAPTURE:
        args.out = {'slots': SLOTS_CAPTURE, 'restart': RESTART_CAPTURE, 'csd': CSD_CAPTURE}.get(args.mode, CAPTURE)
    capture(args)
    print(f'EQ probe capture: {args.out}')


if __name__ == '__main__':
    main()
