"""Mono microphone metrics in dBFS; reference metrics preserve recorded gain."""
import argparse
import json
import math
from pathlib import Path
import sys
import numpy as np
from scipy import signal
import soundfile as sf
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from build.c_header import read_define
from audio.sound_asset import SAMPLE_RATE_HZ, SAMPLE_MAX
DB_POWER = 10
DB_AMPLITUDE = 20
DB_FLOOR = -160.0
FRAME_MS = 20
HANGOVER_MS = 200
ACTIVE_THRESHOLD_DB = -25.0
SPEECH_LOW_HZ = 300
SPEECH_HIGH_HZ = 3400
MILLISECONDS = 1000
FILTER_ORDER = 6
DEFAULT_TRANSIENT_MS = 20
P56_MARGIN_DB = 15.9
P56_SMOOTH_S = 0.03
P56_HANGOVER_S = 0.2
P56_THRESHOLD_COUNT = 15


def _mono(x):
    x = np.asarray(x, dtype=float)
    if x.ndim != 1 or not len(x) or not np.all(np.isfinite(x)):
        raise ValueError('expected nonempty finite mono samples')
    return x


def read_take(path):
    x, rate = sf.read(path, dtype='float64')
    return _mono(x), rate


def _db(power):
    return float(DB_POWER * np.log10(max(float(power), 10 ** (DB_FLOOR / DB_POWER))))


def align(x, ref):
    """Return overlapping (recording, reference), removing the correlation lag."""
    x, ref = _mono(x), _mono(ref)
    correlation = signal.correlate(x - np.mean(x), ref - np.mean(ref), mode='full', method='fft')
    lag = int(signal.correlation_lags(len(x), len(ref))[np.argmax(correlation)])
    start_x, start_ref = max(lag, 0), max(-lag, 0)
    count = min(len(x) - start_x, len(ref) - start_ref)
    return x[start_x:start_x + count], ref[start_ref:start_ref + count]


def clip_count(x):
    x = _mono(x)
    return int(np.count_nonzero((x <= -1) | (x >= SAMPLE_MAX / (SAMPLE_MAX + 1))))


def band_level_db(x, lo, hi, rate=SAMPLE_RATE_HZ):
    """Rectangular FFT band power, including DC and Nyquist once."""
    x = _mono(x)
    if not 0 <= lo < hi <= rate / 2:
        raise ValueError('band must lie within Nyquist')
    frequency = np.fft.rfftfreq(len(x), 1 / rate)
    power = np.abs(np.fft.rfft(x)) ** 2 / len(x) ** 2
    power[1:] *= 2
    if len(x) % 2 == 0:
        power[-1] /= 2
    return _db(np.sum(power[(frequency >= lo) & (frequency < hi)]))


def low_band_db(x, cutoff, rate=SAMPLE_RATE_HZ):
    return band_level_db(x, 0, cutoff, rate)


def active_level_db(x, rate=SAMPLE_RATE_HZ):
    """P.56-style two-pole envelope, hangover and threshold-margin interpolation.

    This estimate is not a conformance implementation of ITU-T P.56.
    """
    x = _mono(x)
    pole = math.exp(-1 / (rate * P56_SMOOTH_S))
    envelope = signal.lfilter([1 - pole], [1, -pole], np.abs(x))
    envelope = signal.lfilter([1 - pole], [1, -pole], envelope)
    thresholds = 2.0 ** np.arange(-P56_THRESHOLD_COUNT, 0)
    energy = np.sum(x * x)
    if not energy:
        return DB_FLOOR
    levels, margins = [], []
    hold = max(1, round(rate * P56_HANGOVER_S))
    for threshold in thresholds:
        hits = np.flatnonzero(envelope >= threshold)
        if not len(hits):
            continue
        ends = np.minimum(hits + hold, len(x))
        active = int(ends[0] - hits[0] + np.maximum(0, ends[1:] - np.maximum(ends[:-1], hits[1:])).sum())
        level = _db(energy / active)
        levels.append(level)
        margins.append(level - DB_AMPLITUDE * math.log10(threshold))
    if not levels:
        return _db(np.mean(x * x))
    return float(np.interp(P56_MARGIN_DB, margins[::-1], levels[::-1]))


def speech_mask(ref, rate=SAMPLE_RATE_HZ):
    """Reference frame energy gate with a trailing hangover, expanded to samples."""
    ref = _mono(ref)
    width = max(1, round(rate * FRAME_MS / MILLISECONDS))
    frames = np.pad(ref, (0, (-len(ref)) % width)).reshape(-1, width)
    power = np.mean(frames * frames, axis=1)
    if not np.max(power):
        return np.zeros(len(ref), dtype=bool)
    active = power >= np.max(power) * 10 ** (ACTIVE_THRESHOLD_DB / DB_POWER)
    hangover = max(1, round(HANGOVER_MS / FRAME_MS))
    active = np.convolve(active.astype(int), np.ones(hangover, dtype=int), mode='full')[:len(active)] > 0
    return np.repeat(active, width)[:len(ref)]


def speech_band_snr_db(x, ref, rate=SAMPLE_RATE_HZ):
    x, ref = _mono(x), _mono(ref)
    if len(x) != len(ref) or rate / 2 <= SPEECH_HIGH_HZ:
        raise ValueError('equal aligned lengths and a speech-band sample rate required')
    sos = signal.butter(FILTER_ORDER, [SPEECH_LOW_HZ, SPEECH_HIGH_HZ], btype='bandpass', fs=rate, output='sos')
    mask = speech_mask(ref, rate)
    if not mask.any():
        raise ValueError('reference has no active speech')
    speech = signal.sosfilt(sos, ref)[mask]
    error = signal.sosfilt(sos, x - ref)[mask]
    return _db(np.mean(speech * speech)) - _db(np.mean(error * error))


def stoi_score(x, ref, rate=SAMPLE_RATE_HZ):
    from pystoi import stoi
    x, ref = _mono(x), _mono(ref)
    if len(x) != len(ref):
        raise ValueError('STOI needs equal aligned lengths')
    return float(stoi(ref, x, rate, extended=False))


def transient_peak_db(x, at_s, window_ms=DEFAULT_TRANSIENT_MS, rate=SAMPLE_RATE_HZ):
    x = _mono(x)
    if not math.isfinite(at_s) or not math.isfinite(window_ms) or at_s < 0 or window_ms <= 0:
        raise ValueError('nonnegative position and positive window required')
    center = round(at_s * rate)
    radius = max(1, round(window_ms * rate / (2 * MILLISECONDS)))
    segment = x[max(0, center - radius):min(len(x), center + radius)]
    if not len(segment):
        raise ValueError('transient window outside recording')
    return _db(np.max(segment * segment))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('take', type=Path)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args(argv)
    x, rate = read_take(args.take)
    result = dict(rate=rate, samples=len(x), clips=clip_count(x), active_dbfs=active_level_db(x, rate),
                  low_band_dbfs=low_band_db(x, SPEECH_LOW_HZ, rate))
    if args.reference:
        ref, ref_rate = read_take(args.reference)
        if rate != ref_rate:
            parser.error('recording and reference sample rates must match')
        aligned, ref = align(x, ref)
        result.update(speech_snr_db=speech_band_snr_db(aligned, ref, rate), stoi=stoi_score(aligned, ref, rate))
    print(json.dumps(result, allow_nan=False) if args.json else '\n'.join(f'{k}: {v}' for k, v in result.items()))

if __name__ == '__main__':
    main()
