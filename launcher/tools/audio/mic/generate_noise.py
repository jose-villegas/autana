"""Generate seeded pink noise or synthetic speech-shaped multitalker babble."""
import argparse
from pathlib import Path
import numpy as np
from scipy import signal
import soundfile as sf
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from audio.sound_asset import SAMPLE_RATE_HZ
PINK_POWER = 0.5
BABBLE_TALKERS = 8
FORMANT_HZ = (500, 1500, 2500)
FORMANT_Q = 3
ENVELOPE_HZ = (2, 7)
PEAK = 0.8

def generate(kind, seconds, rate=SAMPLE_RATE_HZ, seed=0):
    if kind not in ('pink', 'babble') or not np.isfinite(seconds) or seconds <= 0 or rate <= 2 * max(FORMANT_HZ):
        raise ValueError('pink/babble, positive duration and rate above formants required')
    rng = np.random.default_rng(seed)
    n = round(seconds * rate)
    if n < 2: raise ValueError('duration needs at least two samples')
    if kind == 'pink':
        spectrum = np.fft.rfft(rng.normal(size=n))
        frequency = np.fft.rfftfreq(n, 1 / rate)
        spectrum[0] = 0
        spectrum[1:] /= frequency[1:] ** PINK_POWER
        x = np.fft.irfft(spectrum, n=n)
    else:
        x = np.zeros(n)
        t = np.arange(n) / rate
        for _ in range(BABBLE_TALKERS):
            source = rng.normal(size=n)
            voice = sum(signal.lfilter(*signal.iirpeak(f, FORMANT_Q, fs=rate), source) for f in FORMANT_HZ)
            modulation = rng.uniform(*ENVELOPE_HZ)
            x += voice * (1 + np.sin(2 * np.pi * modulation * t + rng.uniform(0, 2 * np.pi))) ** 2
    return x * PEAK / np.max(np.abs(x))

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=('pink', 'babble'))
    parser.add_argument('--seconds', type=float, required=True)
    parser.add_argument('--rate', type=int, default=SAMPLE_RATE_HZ)
    parser.add_argument('--seed', type=int, default=0)
    parser.add_argument('-o', type=Path, required=True)
    args = parser.parse_args(argv)
    x = generate(args.kind, args.seconds, args.rate, args.seed)
    args.o.parent.mkdir(parents=True, exist_ok=True)
    sf.write(args.o, x, args.rate, subtype='PCM_16')

if __name__ == '__main__': main()
