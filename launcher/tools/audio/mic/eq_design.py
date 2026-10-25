"""ES8311 EQ coefficient design and Q2.28 encoding; standard library only, so board jobs need no packages."""
import math
import cmath
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


NUMERATOR_COEFFICIENTS = {'adc': 3, 'dac': 2}
SLOT_HZ = (100, 1000, 4000)
SLOT_VALUES = (0.25, 0.5)
SCALE_VALUES = (0.125, *SLOT_VALUES, 1.0)
SCALE_HZ = 1000
SLOT_TOLERANCE_DB = 1.5
FEEDBACK_SILENCE_DB = -40.0
AMPLITUDE_DB = read_define(EQ_HEADER.parents[3] / 'launcher/packages/math/include/math/scalar/math_db.h',
                           'MATH_DB_AMPLITUDE_SCALE')


def eq_classify_slots(points):
    """Classify baseline-relative isolated slots; incomplete or ambiguous data stays unresolved."""
    result = {}
    for reg, values in points.items():
        complete = all(value in values and set(values[value]) == set(SLOT_HZ) for value in SLOT_VALUES)
        levels = [level for tones in values.values() for level in tones.values()]
        finite = all(math.isfinite(level) for level in levels)
        numerator = complete and finite and all(
            abs(values[value][hz] - AMPLITUDE_DB * math.log10(value)) <= SLOT_TOLERANCE_DB
            for value in SLOT_VALUES for hz in SLOT_HZ)
        feedback = complete and finite and all(level <= FEEDBACK_SILENCE_DB for level in levels)
        result[reg] = 'numerator' if numerator else 'feedback' if feedback else 'unresolved'
    return result


def eq_flat_slot(points):
    for reg, values in points.items():
        if all(value in values and set(values[value]) == set(SLOT_HZ)
               and all(math.isfinite(level) for level in values[value].values())
               and max(values[value].values()) - min(values[value].values()) <= SLOT_TOLERANCE_DB
               and min(values[value].values()) > FEEDBACK_SILENCE_DB for value in SLOT_VALUES):
            return reg
    return None


ORDER_HZ = (SLOT_HZ[0], SAMPLE_RATE_HZ // 8, SAMPLE_RATE_HZ // 4)
PROBE_GAIN = 0.5


def eq_order_response(role, delay, sign=1):
    result = {}
    for hz in ORDER_HZ:
        phase = cmath.exp(-2j * math.pi * hz * delay / SAMPLE_RATE_HZ)
        gain = abs(PROBE_GAIN * (1 + phase) if role == 'numerator' else
                   PROBE_GAIN / (1 + sign * PROBE_GAIN * phase))
        result[hz] = AMPLITUDE_DB * math.log10(max(gain, math.ulp(1.0)))
    if role == 'numerator':
        reference = result[ORDER_HZ[0]]
        result = {hz: level - reference for hz, level in result.items()}
    return result


def eq_decide_layout(points, order_points, numerator_count):
    roles = eq_classify_slots(points)
    if (list(roles.values()).count('numerator') != numerator_count
            or list(roles.values()).count('feedback') != len(roles) - numerator_count):
        return None
    reference = min(reg for reg, role in roles.items() if role == 'numerator')
    layout = {reference: ('numerator', 0, 1)}
    for reg, role in roles.items():
        if reg == reference:
            continue
        measured = order_points.get(reg, {})
        if set(measured) != set(ORDER_HZ) or not all(math.isfinite(v) for v in measured.values()):
            return None
        if role == 'numerator':
            measured = {hz: level - measured[ORDER_HZ[0]] for hz, level in measured.items()}
        candidates = []
        for delay in range(1, numerator_count):
            for sign in ((1,) if role == 'numerator' else (1, -1)):
                expected = eq_order_response(role, delay, sign)
                if all((measured[hz] <= FEEDBACK_SILENCE_DB if expected[hz] <= FEEDBACK_SILENCE_DB else
                        abs(measured[hz] - expected[hz]) <= SLOT_TOLERANCE_DB) for hz in ORDER_HZ):
                    candidates.append((role, delay, sign))
        if len(candidates) != 1:
            return None
        layout[reg] = candidates[0]
    if any(len({delay for role, delay, sign in layout.values() if role == kind}) != count
           for kind, count in (('numerator', numerator_count), ('feedback', len(roles) - numerator_count))):
        return None
    return layout


def eq_layout_coefficients(registers, coefficients, layout, numerator_count):
    return [coefficients[delay if role == 'numerator' else numerator_count + delay - 1] * sign
            for role, delay, sign in (layout[reg] for reg in registers)]
