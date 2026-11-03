"""Render reference-relative HPF burst and speech measurements from recorded takes."""
from functools import partial
import math
from pathlib import Path
from statistics import median

from audio.mic import mic_metrics as metrics
from audio.mic.hpf_tap_capture import arms, schedule, SPEECH_HZ, THUMP_HZ, HAPTIC_HZ, TAKE_MS
from audio_doc_blocks import recorded_table, number
from autana_runner import recorded_take
from build.c_header import read_define

TAP_CUT_DB = 10.0
SPEECH_TOLERANCE_DB = 1.0
LOW_CUTOFF_HZ = 100
HAPTIC_BAND_HZ = (180, 225)
TAIL_MS = 40
BLOCK_SAMPLES = read_define(Path(__file__).resolve().parents[3] / 'main/audio/audio_codec.h',
                            'AUDIO_CODEC_BLOCK_SAMPLES')
COLUMNS = ('Arm', 'Below 100 Hz in taps (dB)', 'Haptic 202 Hz (dB)',
           'Worst speech-band change (dB)', '300 Hz (dB)', 'Verdict')


def take_metrics(path, state, check=None):
    return metrics.read_take_metrics(path, lambda samples, rate: analyze_take(samples, rate, state), check)


def analyze_take(samples, rate, state):
    if state['rate'] != rate or state['samples'] != len(samples) or len(samples) != rate * TAKE_MS // metrics.MILLISECONDS:
        raise ValueError('take and MIC_STATE differ')
    events = state.get('tones', [])
    expected = schedule()
    if len(events) != len(expected):
        raise ValueError('missing burst tones')
    bands = {THUMP_HZ: [], HAPTIC_HZ: []}
    for event, (at, hz, duration, gain) in zip(events, expected):
        block, actual_hz, actual_ms, actual_gain = event
        start = block * BLOCK_SAMPLES
        requested = rate * at // metrics.MILLISECONDS
        if (actual_hz, actual_ms, actual_gain) != (hz, duration, gain) or not requested <= start < requested + BLOCK_SAMPLES:
            raise ValueError('invalid burst tone')
        end = start + rate * (duration + TAIL_MS) // metrics.MILLISECONDS
        if end > len(samples):
            raise ValueError('truncated burst window')
        window = samples[start:end]
        bands[hz].append(metrics.low_band_db(window, LOW_CUTOFF_HZ, rate) if hz == THUMP_HZ
                         else metrics.band_level_db(window, *HAPTIC_BAND_HZ, rate))
    return dict(low=median(bands[THUMP_HZ]), haptic=median(bands[HAPTIC_HZ]))


def capture_data(path, records, check=None):
    expected = arms()
    groups = {}
    current = None
    runs = None
    for command, reply in records:
        if command == ['runs']:
            if runs is not None or type(reply) is not int or reply <= 0:
                raise ValueError('invalid HPF run count')
            runs = reply
        elif command == ['arm']:
            index = reply['index']
            if (type(index) is not int or not 0 <= index < len(expected) or index in groups
                    or reply != dict(expected[index], index=index)):
                raise ValueError('unknown or duplicate HPF arm')
            current = groups[index] = dict(takes=[], tones={})
        elif command[:2] == ['mic', 'record']:
            if current is None:
                raise ValueError('take without arm')
            current['takes'].append(take_metrics(*recorded_take(path, command, reply), check=check))
        elif command[:1] == ['tone']:
            if (current is None or reply['hz'] != int(command[1]) or reply['hz'] not in SPEECH_HZ
                    or not isinstance(reply['tone_db'], (int, float)) or not math.isfinite(reply['tone_db'])):
                raise ValueError('invalid HPF speech tone')
            current['tones'].setdefault(reply['hz'], []).append(reply['tone_db'])
    if runs is None or set(groups) != set(range(len(expected))):
        raise ValueError('missing HPF arms')
    for group in groups.values():
        if (len(group['takes']) != runs or set(group['tones']) != set(SPEECH_HZ)
                or any(len(values) != runs for values in group['tones'].values())):
            raise ValueError('missing HPF runs or tones')
    return groups


def verdict(low, worst):
    return '; '.join(('cuts taps' if low <= -TAP_CUT_DB else 'keeps taps',
                      'keeps speech' if abs(worst) <= SPEECH_TOLERANCE_DB else 'changes speech'))


def rows(groups):
    reference = groups[0]
    result = []
    for index, arm in enumerate(arms()):
        group = groups[index]
        low, haptic = (median(take[key] for take in group['takes'])
                       - median(take[key] for take in reference['takes']) for key in ('low', 'haptic'))
        speech = [median(group['tones'][hz]) - median(reference['tones'][hz]) for hz in SPEECH_HZ]
        worst = max(speech, key=abs)
        label = f"Code {arm['code']}" + (f" + EQ {arm['cutoff']} Hz" if arm['cutoff'] else '')
        if index == 0:
            label += ' (reference)'
        result.append((label, *(number(value) for value in (low, haptic, worst, speech[0])), verdict(low, worst)))
    return result


render = partial(recorded_table, COLUMNS, rows, capture_data)
