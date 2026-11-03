"""Generate bypass-relative voice intelligibility and ADC EQ response tables."""
from functools import partial
import math
from statistics import median

import numpy as np
from scipy import signal

from audio.mic import mic_metrics as metrics
from audio.mic.eq_voice_capture import arms, SPEECH_HZ, SAMPLE_RATE_HZ, clean_sentence, material_samples
from audio_doc_blocks import recorded_table, number
from autana_runner import recorded_take

STOI_NOISE_MIN = .02
SNR_NOISE_MIN_DB = 1.0
STOI_QUIET_LOSS_MAX = .01
RESPONSE_ERROR_MAX_DB = 1.5
REFERENCE_ENERGY_MIN = .999
COLUMNS = ('Arm', 'STOI quiet', 'STOI noise', 'SNR noise (dB)', 'Level (dB)', 'Clips',
           'Worst response error (dB)', 'Verdict')


def clean_reference(material):
    return material_samples(clean_sentence(material['sentence']), SAMPLE_RATE_HZ)[0]


def analyze_take(samples, rate, reference):
    if rate != SAMPLE_RATE_HZ:
        raise ValueError('voice take sample rate differs from reference')
    energy = np.sum(reference ** 2)
    aligned, reference = metrics.align(samples, reference)
    if np.sum(reference ** 2) < energy * REFERENCE_ENERGY_MIN:
        raise ValueError('truncated speech; playback and recording did not fully overlap')
    return dict(stoi=metrics.stoi_score(aligned, reference, rate),
                snr=metrics.speech_gap_snr_db(aligned, reference, rate),
                level=metrics.active_level_db(aligned, rate), clips=metrics.clip_count(samples))


def take_metrics(path, state, material, check=None):
    def analyze(samples, rate):
        if state['rate'] != rate or state['samples'] != len(samples):
            raise ValueError('voice take and MIC_STATE differ')
        return analyze_take(samples, rate, clean_reference(material))
    return metrics.read_take_metrics(path, analyze, check)


def capture_data(path, records, check=None):
    expected = arms()
    groups = {}
    current = material = materials = runs = None
    for command, reply in records:
        if command == ['runs']:
            if runs is not None or type(reply) is not int or reply <= 0:
                raise ValueError('invalid voice run count')
            runs = reply
        elif command == ['materials']:
            if materials is not None or not reply or any(value.get('condition') not in ('quiet', 'noise') for value in reply.values()):
                raise ValueError('invalid voice materials')
            materials = reply
        elif command == ['arm']:
            name = reply['name']
            if name not in expected or name in groups or reply != dict(name=name, coefficients=expected[name]):
                raise ValueError('unknown or duplicate voice arm')
            current = groups[name] = dict(takes={}, tones={})
            material = None
        elif command == ['material']:
            if current is None or materials is None or reply not in materials or reply in current['takes']:
                raise ValueError('unknown or duplicate voice material')
            material = reply
            current['takes'][material] = []
        elif command[:2] == ['mic', 'record'] and current is not None:
            if material is None:
                raise ValueError('voice take without material')
            current['takes'][material].append(take_metrics(*recorded_take(path, command, reply), materials[material], check))
        elif command[:1] == ['tone']:
            hz = reply['hz']
            level = reply['tone_db']
            if current is None or hz != int(command[1]) or hz not in SPEECH_HZ or not math.isfinite(level):
                raise ValueError('invalid voice response tone')
            current['tones'].setdefault(hz, []).append(level)
    if runs is None or materials is None or set(groups) != set(expected):
        raise ValueError('missing voice arms or metadata')
    sentences = {value['sentence'] for value in materials.values()}
    if sorted((v['sentence'], v['condition']) for v in materials.values()) != sorted(
            (sentence, condition) for sentence in sentences for condition in ('quiet', 'noise')):
        raise ValueError('voice materials need quiet and noise for each sentence')
    for group in groups.values():
        if (set(group['takes']) != set(materials) or any(len(takes) != runs for takes in group['takes'].values())
                or set(group['tones']) != set(SPEECH_HZ) or any(len(levels) != runs for levels in group['tones'].values())):
            raise ValueError('missing voice takes or response tones')
        group['materials'] = materials
    return groups


def designed_response(coefficients):
    if coefficients is None:
        return np.zeros(len(SPEECH_HZ))
    _, response = signal.freqz(coefficients[:3], [1, *(-value for value in coefficients[3:])],
                               worN=SPEECH_HZ, fs=SAMPLE_RATE_HZ)
    return metrics.DB_AMPLITUDE * np.log10(abs(response))


def verdict(quiet, noise, snr, error):
    helps = quiet >= -STOI_QUIET_LOSS_MAX and (noise >= STOI_NOISE_MIN or snr >= SNR_NOISE_MIN_DB)
    return '; '.join(('helps' if helps else 'does not help',
                      'matches design' if error <= RESPONSE_ERROR_MAX_DB else 'differs from design'))


def material_delta(group, baseline, key, condition=None):
    return median(median(take[key] for take in group['takes'][name])
                  - median(take[key] for take in baseline['takes'][name])
                  for name, material in group['materials'].items()
                  if condition is None or material['condition'] == condition)


def rows(groups):
    baseline = groups['bypass']
    result = []
    for name, coefficients in arms().items():
        group = groups[name]
        quiet = material_delta(group, baseline, 'stoi', 'quiet')
        noise = material_delta(group, baseline, 'stoi', 'noise')
        snr = material_delta(group, baseline, 'snr', 'noise')
        level = material_delta(group, baseline, 'level')
        clips = max(median(take['clips'] for take in takes) for takes in group['takes'].values())
        response = [median(group['tones'][hz]) - median(baseline['tones'][hz]) for hz in SPEECH_HZ]
        error = max(abs(np.asarray(response) - designed_response(coefficients)))
        result.append((name, f'{quiet:.3f}', f'{noise:.3f}', number(snr), number(level), number(clips), number(error),
                       'reference; matches design' if name == 'bypass' else verdict(quiet, noise, snr, error)))
    return result


render = partial(recorded_table, COLUMNS, rows, capture_data)
