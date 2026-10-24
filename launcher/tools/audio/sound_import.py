"""Decode common sound sources for the sound bake producer, then encode SOND v1."""

import math
import pathlib
import sys

import numpy as np
import soundfile as sf

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from audio import sound_asset as sound

SINC_RADIUS = 32
# Bound the temporary filter matrix independently of the recording's duration.
RESAMPLE_BATCH_FRAMES = 1024


def resample(samples, rate):
    if rate == sound.SAMPLE_RATE_HZ:
        return samples
    samples = np.asarray(samples, dtype=np.float64)
    cutoff = min(1.0, sound.SAMPLE_RATE_HZ / rate)
    radius = SINC_RADIUS / cutoff
    count = round(len(samples) * sound.SAMPLE_RATE_HZ / rate)
    result = np.empty(count, dtype=np.float64)
    offsets = np.arange(-math.ceil(radius), math.ceil(radius) + 1)
    for first in range(0, count, RESAMPLE_BATCH_FRAMES):
        end = min(count, first + RESAMPLE_BATCH_FRAMES)
        position = np.arange(first, end) * rate / sound.SAMPLE_RATE_HZ
        indices = np.floor(position).astype(np.int64)[:, None] + offsets
        distance = indices - position[:, None]
        valid = (indices >= 0) & (indices < len(samples)) & (np.abs(distance) <= radius)
        weights = np.sinc(distance * cutoff) * (1 + np.cos(np.pi * distance / radius)) / 2
        weights = np.where(valid, weights, 0)
        values = samples[np.clip(indices, 0, len(samples) - 1)]
        result[first:end] = np.sum(values * weights, axis=1) / np.sum(weights, axis=1)
    return result


def read_source(path):
    try:
        with sf.SoundFile(path) as source:
            samples = source.read(dtype="float64", always_2d=True)
            rate = source.samplerate
            if len(samples) == 0:
                raise sound.SoundError(f"{path}: empty sound")
    except (OSError, sf.SoundFileError) as error:
        raise sound.SoundError(f"{path}: cannot decode sound: {error}") from error
    if not np.isfinite(samples).all():
        raise sound.SoundError(f"{path}: nonfinite samples")
    mono = samples.mean(axis=1) * (sound.SAMPLE_MAX + 1)
    return np.clip(np.rint(resample(mono, rate)), sound.SAMPLE_MIN, sound.SAMPLE_MAX).astype(int).tolist()


def bake(path):
    source, fmt, loop = sound.load_source(path)
    return sound.encode(read_source(source), fmt, loop)
