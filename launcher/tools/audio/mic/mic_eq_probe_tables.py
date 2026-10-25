"""Baseline-subtracted EQ responses and HPF cutoff estimates."""
import math
from statistics import median

import numpy as np
from scipy.signal import freqz
from audio.mic.eq_probe_sweep import designs, SAMPLE_RATE_HZ
from audio.mic.mic_metrics import DB_AMPLITUDE
from audio_doc_blocks import number

CUTOFF_DB = -3.0
CURVE_POINTS = 256
NUMERATOR_COEFFICIENTS = {'adc': 3, 'dac': 2}


def response(owner, case, frequencies):
    coefficients = designs()[owner][case]
    if coefficients is None:
        return np.zeros(len(frequencies))
    split = NUMERATOR_COEFFICIENTS[owner]
    b, a = coefficients[:split], (1, *coefficients[split:])
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
            if owner != 'hpf' and case not in designs()[owner]:
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
        if baseline is None or points.keys() != baseline.keys():
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
