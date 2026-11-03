"""Baseline-subtracted EQ responses and HPF cutoff estimates."""
import math
import re
from statistics import median, linear_regression

import numpy as np
from scipy.signal import freqz
from audio.mic.eq_probe_sweep import designs, csd_designs, ADC_REGS, DAC_REGS, CSD_PASSBAND_HZ, CSD_TOLERANCE_DB, DEFAULT_HZ
from audio.mic.eq_design import (
    eq_classify_slots, SLOT_HZ, SLOT_VALUES, SCALE_VALUES, AMPLITUDE_DB, SLOT_TOLERANCE_DB,
    FEEDBACK_SILENCE_DB, SCALE_HZ, eq_decide_layout, eq_order_response, ORDER_HZ,
    NUMERATOR_COEFFICIENTS,
)
from audio.sound_asset import SAMPLE_RATE_HZ
from audio.mic.mic_metrics import DB_AMPLITUDE
from audio_doc_blocks import number, details

CUTOFF_DB = -3.0
CURVE_POINTS = 256


def response(owner, case, frequencies):
    coefficients = (csd_designs() if case.startswith('csd-') else designs())[owner][case.removeprefix('rerun-')]
    if coefficients is None:
        return np.zeros(len(frequencies))
    split = NUMERATOR_COEFFICIENTS[owner]
    b, a = coefficients[:split], (1, *(-v for v in coefficients[split:]))
    _, values = freqz(b, a, worN=frequencies, fs=SAMPLE_RATE_HZ)
    return DB_AMPLITUDE * np.log10(np.abs(values))


def hpf_reference(data):
    return max(int(case) for owner, case, path in data if owner == 'hpf' and case != 'bypass')


def probe_data(records, *, relative=True):
    if not records or records[0][0] != ['buildid']:
        raise ValueError('EQ capture must start with buildid')
    groups = {}
    active = None
    measurement_settings = set()
    for command, reply in records:
        if command[:1] == ['probe']:
            if len(command) != 3 or command[1] not in ('adc', 'dac', 'hpf'):
                raise ValueError('unknown probe case')
            owner, case = command[1:]
            if owner != 'hpf' and case not in designs()[owner] and case not in csd_designs()[owner] and case not in ('zero', 'unity') and not slot_case(case) and not order_case(case) and case.removeprefix('rerun-') not in designs()[owner]:
                raise ValueError('unknown EQ design')
            if owner == 'hpf' and case != 'bypass' and not case.isdecimal():
                raise ValueError('invalid HPF code')
            active = owner, case
        elif command[:1] == ['tone']:
            if active is None:
                raise ValueError('tone has no probe case')
            hz, ms, volume = map(int, command[1:4])
            path = 'internal' if command[4:] == ['--internal'] else 'acoustic'
            if command[4:] not in ([], ['--internal']) or (path == 'internal' and active[0] != 'dac'):
                raise ValueError('invalid probe tone path')
            level = reply['tone_db']
            if (reply['hz'] != hz or reply.get('path', 'acoustic') != path or hz <= 0 or ms <= 0
                    or not isinstance(level, (float, int)) or not math.isfinite(level)):
                raise ValueError('invalid probe tone reply')
            measurement_settings.add((ms, volume))
            groups.setdefault((*active, path), {}).setdefault(hz, []).append(level)
    if not groups or len(measurement_settings) != 1:
        raise ValueError('missing tones or inconsistent probe settings')
    medians = {key: {hz: median(values) for hz, values in points.items()} for key, points in groups.items()}
    if not relative:
        return medians
    result = {}
    for (owner, case, path), points in medians.items():
        reference = str(hpf_reference(medians)) if owner == 'hpf' else 'bypass'
        baseline = medians.get((owner, reference, path))
        if baseline is None or not points.keys() <= baseline.keys():
            raise ValueError('missing matching probe baseline frequencies')
        result[owner, case, path] = {hz: value - baseline[hz] for hz, value in points.items()}
    return result


def cutoff(points):
    ordered = sorted(points.items())
    for (low_hz, low_db), (high_hz, high_db) in zip(ordered, ordered[1:]):
        if low_db <= CUTOFF_DB <= high_db and low_db != high_db:
            fraction = (CUTOFF_DB - low_db) / (high_db - low_db)
            return math.exp(math.log(low_hz) + fraction * math.log(high_hz / low_hz))
    return None


def render(path, records, caption, picture, details, markdown, regenerate):
    data = probe_data(records)
    body = caption(path, regenerate, 'replies')
    body += 'Responses subtract the matching bypass capture at each frequency. Error is measured minus designed response.\n\n'
    for owner in ('adc', 'dac'):
        body += picture('audio/mic-eq-probe-' + owner, owner.upper() + ' EQ measured and designed responses')
        for case in designs()[owner]:
            if case == 'bypass':
                continue
            paths = ('acoustic', 'internal') if owner == 'dac' else ('acoustic',)
            rows = []
            for path_name in paths:
                points = data.get((owner, case, path_name))
                if points is None:
                    raise ValueError('missing EQ probe case: ' + owner + ' ' + case + ' ' + path_name)
                frequencies = sorted(points)
                for hz, expected in zip(frequencies, response(owner, case, frequencies)):
                    measured = points[hz]
                    rows.append((path_name, hz, number(measured), number(expected), number(measured - expected)))
            body += f'### {owner.upper()} EQ: {case}\n\n'
            body += details(markdown(['Path', 'Hz', 'Measured (dB)', 'Designed (dB)', 'Error (dB)'], rows)) + '\n'
    reference = hpf_reference(data)
    body += picture('audio/mic-eq-probe-hpf', f'HPF code responses relative to code {reference}')
    body += f'HPF responses subtract code {reference}; cutoffs interpolate in log frequency. No measured crossing is n/a.\n\n'
    levels = probe_data(records, relative=False)
    frequencies = sorted(data['hpf', str(reference), 'acoustic'])
    rows = [(case, number(cutoff(points), digits=1),
             *[number(levels[owner, case, path_name][hz]) for hz in frequencies])
            for (owner, case, path_name), points in data.items() if owner == 'hpf' and case != 'bypass']
    return body + details(markdown(['HPF s1 = s2 code', f'Cutoff relative to code {reference} (Hz)',
                                   *[f'{hz} Hz raw (dBFS)' for hz in frequencies]], rows))


ORDER_CASE = re.compile(r'(delay|feedback)-([0-9a-f]{2})')


def order_case(case):
    return ORDER_CASE.fullmatch(case)


SLOT_CASE = re.compile(r'(slot|scale)-([0-9a-f]{2})-(0\.125|0\.25|0\.5|1)')


def slot_case(case):
    return SLOT_CASE.fullmatch(case)


def slot_results(data, owner):
    registers = sorted(ADC_REGS if owner == 'adc' else DAC_REGS)
    points = {reg: {} for reg in registers}
    for (kind, case, path), tones in data.items():
        match = slot_case(case)
        if kind == owner and match and match[1] == 'slot':
            if path != 'acoustic' or int(match[2], 16) not in points:
                raise ValueError('invalid slot register or path')
            points[int(match[2], 16)][float(match[3])] = tones
    return points, eq_classify_slots(points)


def render_slots(path, records, caption, picture, details, markdown, regenerate):
    data = probe_data(records)
    body = caption(path, regenerate, 'replies')
    decisions = {}
    for owner in ('adc', 'dac'):
        points, roles = slot_results(data, owner)
        decisions[owner] = ', '.join(f'0x{reg:02X}: {role}' for reg, role in roles.items())
    body += 'Run 2. Isolated-slot decisions: ' + '; '.join(
        owner.upper() + ' (' + decision + ')' for owner, decision in decisions.items()) + '.\n\n'
    body += (f'Numerators match 20 log10(v) within {SLOT_TOLERANCE_DB:g} dB at every probe tone and value; '
             f'feedback slots are at or below {FEEDBACK_SILENCE_DB:g} dB relative to bypass.\n\n')
    for owner in ('adc', 'dac'):
        points, roles = slot_results(data, owner)
        body += picture('audio/mic-eq-slots-' + owner, owner.upper() + ' isolated-slot responses and scale')
        rows = [(f'0x{reg:02X}', value, hz, number(levels.get(hz)),
                 number(AMPLITUDE_DB * math.log10(value)), roles[reg])
                for reg, values in points.items() for value in SLOT_VALUES
                for levels in (values.get(value, {}),) for hz in SLOT_HZ]
        body += details(markdown(['Register', 'Value', 'Hz', 'Relative (dB)', 'Numerator (dB)', 'Decision'], rows)) + '\n'
        body += picture('audio/mic-eq-scale-' + owner, owner.upper() + ' coefficient scale')
        checks = []
        for (kind, case, path_name), tones in data.items():
            if kind != owner or case == 'bypass' or case.startswith('slot-') or order_case(case):
                continue
            for hz, measured in sorted(tones.items()):
                match = slot_case(case)
                expected = (AMPLITUDE_DB * math.log10(float(match[3])) if match else
                            0.0 if case == 'unity' else 'silence' if case == 'zero' else
                            float(response(owner, case, [hz])[0]))
                checks.append((case, hz, number(measured), expected if isinstance(expected, str) else number(expected)))
        body += details(markdown(['Check', 'Hz', 'Relative (dB)', 'Expected (dB)'], checks)) + '\n'
        layout = decided_layout(data, owner)
        if any(kind == owner and order_case(case) for kind, case, path in data):
            body += picture('audio/mic-eq-order-' + owner, owner.upper() + ' delay and feedback sign probes')
        if any(kind == owner and case.startswith('rerun-') for kind, case, path in data):
            body += picture('audio/mic-eq-designs-' + owner, owner.upper() + ' designs using the measured layout')
        if layout is None:
            body += owner.upper() + ' delay order/sign unresolved; design rerun skipped.\n\n'
        else:
            body += owner.upper() + ' decided layout: ' + ', '.join(
                f'0x{reg:02X} = {role} z^-{delay}' + (f' (sign {sign:+d})' if role == 'feedback' else '')
                for reg, (role, delay, sign) in sorted(layout.items())) + '.\n\n'
            body += 'The lowest numerator register is the b0 delay reference.\n\n'
            body += details(markdown(['Register', 'Role', 'Delay', 'Feedback sign'],
                                     [(f'0x{reg:02X}', role, delay, sign if role == 'feedback' else 'n/a')
                                      for reg, (role, delay, sign) in sorted(layout.items())])) + '\n'
        order_rows = []
        for (kind, case, path_name), tones in data.items():
            match = order_case(case)
            if kind != owner or not match:
                continue
            reg = int(match[2], 16)
            reference = tones[ORDER_HZ[0]] if match[1] == 'delay' else 0
            expected = eq_order_response(*layout[reg]) if layout else {}
            for hz, measured in sorted(tones.items()):
                order_rows.append((case, hz, number(measured - reference), number(expected.get(hz))))
        body += details(markdown(['Order/sign probe', 'Hz', 'Relative (dB)', 'Decided model (dB)'], order_rows)) + '\n'
        scale = scale_result(data, owner)
        if scale is not None:
            body += details(markdown(['Scale slot', 'Slope (dB/octave)', 'Unity offset (dB)'],
                                     [(f'0x{scale[0]:02X}', *map(number, scale[1:]))])) + '\n'
    return body


def scale_result(data, owner):
    points = {}
    for (kind, case, path), tones in data.items():
        match = slot_case(case)
        if kind == owner and match and match[1] == 'scale':
            if path != 'acoustic' or set(tones) != {SCALE_HZ}:
                raise ValueError('invalid scale tone path or frequency')
            points.setdefault(int(match[2], 16), {})[float(match[3])] = tones[SCALE_HZ]
    if not points:
        return None
    if len(points) != 1 or set(next(iter(points.values()))) != set(SCALE_VALUES):
        raise ValueError('scale probe needs every value on one register')
    reg, values = next(iter(points.items()))
    slope, offset = linear_regression([math.log2(value) for value in values], list(values.values()))
    return reg, slope, offset


def decided_layout(data, owner):
    points, roles = slot_results(data, owner)
    order_points = {int(match[2], 16): tones for (kind, case, path), tones in data.items()
                    if kind == owner and (match := order_case(case))}
    return eq_decide_layout(points, order_points, NUMERATOR_COEFFICIENTS[owner])


RESTART_TOLERANCE_DB = 3.0
RESTART_VERDICT_HZ = (1000, 4000)


def restart_expected(owner, case, hz):
    if case == 'zero':
        return FEEDBACK_SILENCE_DB
    if case == 'unity':
        return 0.0
    return float(response(owner, case, [hz])[0])


def render_restart(path, records, caption, markdown, regenerate):
    from audio.mic.eq_probe_sweep import restart_designs, RESTART_HZ
    data = probe_data(records)
    rows, verdicts = [], []
    for owner in ('adc', 'dac'):
        follows = True
        for case in restart_designs(owner):
            for path_name in (('acoustic', 'internal') if owner == 'dac' else ('acoustic',)):
                points = data.get((owner, case, path_name), {})
                if set(points) != set(RESTART_HZ):
                    raise ValueError('restart capture needs every case, path and frequency')
                for hz, measured in sorted(points.items()):
                    expected = restart_expected(owner, case, hz)
                    error = max(0.0, measured - expected) if case == 'zero' else measured - expected
                    if hz in RESTART_VERDICT_HZ and (error > 0 if case == 'zero' else abs(error) > RESTART_TOLERANCE_DB):
                        follows = False
                    rows.append((owner, case, path_name, hz, number(measured),
                                 ('<= ' if case == 'zero' else '') + number(expected), number(error)))
        verdicts.append(owner.upper() + ': ' + ('within Q2.28 design tolerance' if follows else 'Q2.28 words mis-encoded') + '.')
    return ('### EQ after a state-machine restart\n\n'
            + 'These words were Q2.28 (mis-encoded). Each case loads the coefficients, then restarts the codec state machine (REG00: resets with CSM off, '
            + 'resets cleared, CSM on) before measuring.\n\n'
            + caption(path, regenerate, 'replies')
            + markdown(['Owner', 'Case', 'Path', 'Hz', 'Measured minus bypass (dB)', 'Designed (dB)', 'Error (dB)'], rows)
            + '\n' + '\n\n'.join(verdicts) + '\n')


def render_csd(path, records, caption, markdown, regenerate):
    data = probe_data(records)
    body = '### Signed power-of-two (CSD) EQ validation\n\n' + caption(path, regenerate, 'replies')
    body += f'Passband tolerance is {CSD_TOLERANCE_DB:g} dB at the named frequencies; every path must pass.\n\n'
    for owner, cases in CSD_PASSBAND_HZ.items():
        for case, passband in cases.items():
            rows = []
            follows = True
            for path_name in (('acoustic', 'internal') if owner == 'dac' else ('acoustic',)):
                points = data.get((owner, case, path_name), {})
                if set(points) != set(DEFAULT_HZ):
                    raise ValueError('CSD capture needs every case, path and frequency')
                for hz, expected in zip(sorted(points), response(owner, case, sorted(points))):
                    error = points[hz] - expected
                    if hz in passband and abs(error) > CSD_TOLERANCE_DB:
                        follows = False
                    rows.append((path_name, hz, number(points[hz]), number(expected), number(error)))
            body += f"{owner} {case}: {'PASS' if follows else 'FAIL'} (passband: {', '.join(map(str, passband))} Hz).\n\n"
            body += details(markdown(['Path', 'Hz', 'Measured minus bypass (dB)', 'Designed (dB)', 'Error (dB)'], rows)) + '\n'
    return body
