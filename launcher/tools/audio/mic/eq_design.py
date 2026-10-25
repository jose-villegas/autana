"""ES8311 EQ coefficient design and Q2.28 encoding; standard library only, so board jobs need no packages."""
import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from build.c_header import read_define
from audio.sound_asset import SAMPLE_RATE_HZ

EQ_HEADER = Path(__file__).resolve().parents[3] / 'main/audio/audio_codec_eq.h'
EQ_SCALE = read_define(EQ_HEADER, 'AUDIO_CODEC_EQ_SCALE')
EQ_MASK = read_define(EQ_HEADER, 'AUDIO_CODEC_EQ_MASK')
EQ_MIN = read_define(EQ_HEADER, 'AUDIO_CODEC_EQ_MIN')
EQ_MAX = -EQ_MIN - 1 / EQ_SCALE


def eq_encode(value):
    if not math.isfinite(value):
        return 0
    value = min(EQ_MAX, max(EQ_MIN, value)) * EQ_SCALE
    rounded = math.floor(value + .5) if value >= 0 else math.ceil(value - .5)
    return rounded & EQ_MASK


def eq_design_biquad(kind, f0, q, rate=SAMPLE_RATE_HZ):
    """RBJ [b0,b1,b2,a1,a2], denominator 1+a1*z^-1+a2*z^-2."""
    if kind not in ('highpass', 'lowpass') or not 0 < f0 < rate / 2 or not math.isfinite(q) or q <= 0:
        raise ValueError('highpass/lowpass, frequency below Nyquist and positive Q required')
    omega = 2 * math.pi * f0 / rate
    c = math.cos(omega)
    alpha = math.sin(omega) / (2 * q)
    b = [(1 + c) / 2, -(1 + c), (1 + c) / 2] if kind == 'highpass' else [(1 - c) / 2, 1 - c, (1 - c) / 2]
    return [value / (1 + alpha) for value in (*b, -2 * c, 1 - alpha)]


def eq_design_first_order_highpass(f0, rate=SAMPLE_RATE_HZ):
    """Bilinear one-pole Butterworth [b0,b1,a1], denominator 1+a1*z^-1."""
    if not 0 < f0 < rate / 2:
        raise ValueError('frequency below Nyquist required')
    k = math.tan(math.pi * f0 / rate)
    b0 = 1 / (1 + k)
    return [b0, -b0, (k - 1) / (k + 1)]
