"""ES8311 EQ coefficient design and signed power-of-two encoding; standard library only, so board jobs need no packages."""
import math
import cmath
import functools
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from build.c_header import read_define
from audio.sound_asset import SAMPLE_RATE_HZ

EQ_HEADER = Path(__file__).resolve().parents[3] / 'main/audio/audio_codec_eq.h'
EQ_TERMS, EQ_FIELD_BITS, EQ_SIGN_BIT, EQ_SHIFT_MAX = (
    read_define(EQ_HEADER, 'AUDIO_CODEC_EQ_' + name)
    for name in ('TERMS', 'FIELD_BITS', 'SIGN_BIT', 'SHIFT_MAX'))
EQ_UNIT = 1 << EQ_SHIFT_MAX
EQ_MAX = EQ_TERMS


@functools.lru_cache(maxsize=1)
def eq_reachable_sums():
    """Exact term-count reachability in units of the smallest power of two."""
    sums = [1]
    for _ in range(EQ_TERMS):
        bits = 0
        for shift in range(EQ_SHIFT_MAX + 1):
            power = EQ_UNIT >> shift
            bits |= sums[-1] << (EQ_UNIT + power)
            bits |= sums[-1] << (EQ_UNIT - power)
        sums.append(bits)
    return tuple(sums)


def eq_encode(value):
    if not math.isfinite(value) or not -EQ_MAX <= value <= EQ_MAX:
        raise ValueError('finite coefficient within the six-term range required')
    sums = eq_reachable_sums()
    position = value * EQ_UNIT + EQ_TERMS * EQ_UNIT
    lower_limit = math.floor(position)
    lower = (sums[-1] & ((1 << (lower_limit + 1)) - 1)).bit_length() - 1
    upper_bits = sums[-1] >> math.ceil(position)
    upper = math.ceil(position) + (upper_bits & -upper_bits).bit_length() - 1
    target = min((lower, upper), key=lambda candidate: abs((candidate - EQ_TERMS * EQ_UNIT) / EQ_UNIT - value)) - EQ_TERMS * EQ_UNIT
    word = 0
    for remaining in range(EQ_TERMS - 1, -1, -1):
        candidates = [(shift, sign, (-1 if sign else 1) * (EQ_UNIT >> shift))
                      for shift in range(EQ_SHIFT_MAX + 1) for sign in (0, EQ_SIGN_BIT)]
        for shift, sign, power in sorted(candidates, key=lambda item: abs(target - item[2])):
            index = target - power + remaining * EQ_UNIT
            if index >= 0 and (sums[remaining] >> index) & 1:
                word = (word << EQ_FIELD_BITS) | sign | shift
                target -= power
                break
    return word


def eq_decode(word):
    if not isinstance(word, int) or not 0 <= word < 1 << (EQ_TERMS * EQ_FIELD_BITS):
        raise ValueError(f'{EQ_TERMS * EQ_FIELD_BITS}-bit coefficient word required')
    result = 0.0
    for _ in range(EQ_TERMS):
        result += (-1 if word & EQ_SIGN_BIT else 1) * 2 ** -(word & EQ_SHIFT_MAX)
        word >>= EQ_FIELD_BITS
    return result


def eq_design_biquad(kind, f0, q, rate=SAMPLE_RATE_HZ, *, gain_db=0):
    """RBJ [b0,b1,b2,a1,a2], stored feedback added to the output recurrence."""
    if kind not in ('highpass', 'lowpass', 'bandpass', 'peaking') or not 0 < f0 < rate / 2 or not math.isfinite(q) or q <= 0 or not math.isfinite(gain_db):
        raise ValueError('supported biquad, frequency below Nyquist, positive Q and finite gain required')
    omega = 2 * math.pi * f0 / rate
    c = math.cos(omega)
    alpha = math.sin(omega) / (2 * q)
    b = [(1 + c) / 2, -(1 + c), (1 + c) / 2] if kind == 'highpass' else [(1 - c) / 2, 1 - c, (1 - c) / 2]
    if kind == 'bandpass':
        b = [alpha, 0, -alpha]
    if kind == 'peaking':
        amplitude = 10 ** (gain_db / (2 * AMPLITUDE_DB))
        denominator = 1 + alpha / amplitude
        return [value / denominator for value in
                (1 + alpha * amplitude, -2 * c, 1 - alpha * amplitude, 2 * c, alpha / amplitude - 1)]
    return [value / (1 + alpha) for value in (*b, 2 * c, alpha - 1)]


def eq_design_first_order_highpass(f0, rate=SAMPLE_RATE_HZ):
    """Bilinear one-pole Butterworth [b0,b1,a1], stored feedback added to the output recurrence."""
    if not 0 < f0 < rate / 2:
        raise ValueError('frequency below Nyquist required')
    k = math.tan(math.pi * f0 / rate)
    b0 = 1 / (1 + k)
    return [b0, -b0, (1 - k) / (k + 1)]


def eq_design_first_order_shelf(f0, gain_db, rate=SAMPLE_RATE_HZ):
    """First-order bass shelf with unity high-frequency gain and stored feedback."""
    if not 0 < f0 < rate / 2 or not math.isfinite(gain_db):
        raise ValueError('frequency below Nyquist and finite gain required')
    gain = 10 ** (gain_db / AMPLITUDE_DB)
    k = math.tan(math.pi * f0 / rate) / math.sqrt(gain)
    return [(1 + gain * k) / (1 + k), (gain * k - 1) / (1 + k), (1 - k) / (1 + k)]


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
