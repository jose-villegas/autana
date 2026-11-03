"""Mono microphone metrics in dBFS; reference metrics preserve recorded gain."""
import argparse
import hashlib
import re
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
SETTLE_TOLERANCE_DB = 1.0
SETTLE_FRAME_MS = 5
SETTLE_WINDOW_MS = 200
ACTIVE_FRAME_THRESHOLD_DB = -50.0
SECOND_DIFFERENCE_ORDER = 2
TAKE_METRICS_NAME = 'take_metrics.json'
# Cached metrics keep the digits every platform's float maths agrees on.
TAKE_METRICS_DECIMALS = 9
LFS_POINTER_VERSION = b'version https://git-lfs.github.com/spec/v1\n'
LFS_OID = rb'^oid sha256:([0-9a-f]{64})$'


def _mono(x):
    x = np.asarray(x, dtype=float)
    if x.ndim != 1 or not len(x) or not np.all(np.isfinite(x)):
        raise ValueError('expected nonempty finite mono samples')
    return x


def read_take(path):
    x, rate = sf.read(path, dtype='float64')
    return _mono(x), rate


def _rounded(value):
    if isinstance(value, float):
        return round(value, TAKE_METRICS_DECIMALS)
    if isinstance(value, dict):
        return {k: _rounded(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_rounded(v) for v in value]
    return value


def read_take_metrics(path, analyze, check=None):
    """Validate cached metrics against audio content or its LFS content hash.

    check=None computes without refreshing; False refreshes and True verifies.
    """
    path = Path(path)
    cache = path.with_name(TAKE_METRICS_NAME)
    entries = json.loads(cache.read_text(encoding='utf-8')) if cache.exists() else {}
    content = path.read_bytes()
    pointer = content.startswith(LFS_POINTER_VERSION)
    oid = re.search(LFS_OID, content, re.MULTILINE) if pointer else None
    digest = oid[1].decode('ascii') if oid else hashlib.sha256(content).hexdigest()
    stored = entries.get(path.name)
    stale = f'{path}: take metrics stale; regenerate with the LFS audio present'
    if pointer:
        if not oid or not stored or stored.get('sha256') != digest or 'metrics' not in stored:
            raise ValueError(stale)
        return stored['metrics']
    samples, rate = read_take(path)
    result = _rounded(analyze(samples, rate))
    entry = dict(sha256=digest, metrics=result)
    if check is True and stored != entry:
        raise ValueError(stale)
    if check is False and stored != entry:
        entries[path.name] = entry
        cache.write_text(json.dumps(entries, indent=2, sort_keys=True, allow_nan=False) + '\n', encoding='utf-8', newline='\n')
    return result


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


def noise_dbfs(x):
    """Broadband RMS level of a take, for a floor or hiss with nothing playing."""
    x = _mono(x)
    return _db(np.mean(x * x)) if len(x) else DB_FLOOR


def low_band_db(x, cutoff, rate=SAMPLE_RATE_HZ):
    return band_level_db(x, 0, cutoff, rate)


def active_level_db(x, rate=SAMPLE_RATE_HZ, band=None):
    """Active-frame band level, or unbanded P.56-style envelope and hangover estimate.

    This estimate is not a conformance implementation of ITU-T P.56.
    """
    x = _mono(x)
    if band is not None:
        lo, hi = band
        if not 0 < lo < hi < rate / 2:
            raise ValueError('active band must lie inside Nyquist')
        filtered = signal.sosfilt(signal.butter(FILTER_ORDER, band, btype='bandpass', fs=rate, output='sos'), x)
        width = max(1, round(rate * FRAME_MS / MILLISECONDS))
        frames = [filtered[start:start + width] for start in range(0, len(x), width)]
        active = [frame for frame in frames if _db(np.mean(frame * frame)) >= ACTIVE_FRAME_THRESHOLD_DB]
        return _db(np.mean(np.concatenate(active) ** 2)) if active else DB_FLOOR
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


def speech_gap_snr_db(x, ref, rate=SAMPLE_RATE_HZ):
    """Speech-frame versus gap-frame band power, using the aligned clean reference gate."""
    x, ref = _mono(x), _mono(ref)
    if len(x) != len(ref) or rate / 2 <= SPEECH_HIGH_HZ:
        raise ValueError('equal aligned lengths and a speech-band sample rate required')
    mask = speech_mask(ref, rate)
    if not mask.any() or mask.all():
        raise ValueError('reference needs speech and gap frames')
    filtered = signal.sosfilt(signal.butter(FILTER_ORDER, [SPEECH_LOW_HZ, SPEECH_HIGH_HZ],
                                          btype='bandpass', fs=rate, output='sos'), x)
    return _db(np.mean(filtered[mask] ** 2)) - _db(np.mean(filtered[~mask] ** 2))


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


def click_excess_db(samples, rate, at_samples, window_ms=DEFAULT_TRANSIENT_MS):
    """Second-difference peak around each step minus its preceding steady peak."""
    x = _mono(samples)
    if rate <= 0 or window_ms <= 0:
        raise ValueError('positive rate and window required')
    radius = max(1, round(window_ms * rate / (2 * MILLISECONDS)))
    difference = np.abs(np.diff(x, n=SECOND_DIFFERENCE_ORDER, prepend=[x[0], x[0]]))
    result = []
    for at in at_samples:
        if not 2 * radius <= at < len(x) - radius:
            raise ValueError('step needs a preceding steady window and a full click window')
        peak = np.max(difference[at - radius:at + radius])
        steady = np.max(difference[at - 2 * radius:at - radius])
        result.append(_db(peak * peak) - _db(steady * steady))
    return result


def settle_ms(samples, rate, at_sample, end_sample=None):
    """First frame within one dB of the final RMS level and staying there; None if unresolved."""
    x = _mono(samples)
    if rate <= 0 or not 0 <= at_sample < len(x):
        raise ValueError('invalid rate or step position')
    end = min(len(x), len(x) if end_sample is None else end_sample)
    width = max(1, round(SETTLE_FRAME_MS * rate / MILLISECONDS))
    frames = [x[start:start + width] for start in range(at_sample, end - width + 1, width)]
    if len(frames) < 2:
        return None
    levels = np.array([_db(np.mean(frame * frame)) for frame in frames])
    tail_frames = max(1, round(SETTLE_WINDOW_MS / SETTLE_FRAME_MS))
    target = float(np.median(levels[-min(tail_frames, max(1, len(levels) // 2)):]))
    outside = np.flatnonzero(np.abs(levels - target) > SETTLE_TOLERANCE_DB)
    first = int(outside[-1] + 1) if len(outside) else 0
    return first * width * MILLISECONDS / rate if first < len(levels) - 1 else None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('take', type=Path)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args(argv)
    x, rate = read_take(args.take)
    result = dict(rate=rate, samples=len(x), clips=clip_count(x), noise_dbfs=noise_dbfs(x),
                  active_dbfs=active_level_db(x, rate),
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
