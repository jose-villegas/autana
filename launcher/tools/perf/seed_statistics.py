"""Inference on independent layout seed means, without external dependencies."""
import itertools
import math
import random
from functools import lru_cache
from statistics import mean, median, variance


def beta_fraction(a, b, x):
    """Evaluate the continued fraction for the incomplete beta function."""
    qab, qap, qam = a + b, a + 1, a - 1
    c = 1.0
    d = 1 - qab * x / qap
    d = 1 / max(abs(d), 1e-300) * (1 if d >= 0 else -1)
    h = d
    for m in range(1, 301):
        for aa in (m * (b - m) * x / ((qam + 2 * m) * (a + 2 * m)),
                   -(a + m) * (qab + m) * x / ((a + 2 * m) * (qap + 2 * m))):
            d = 1 + aa * d
            c = 1 + aa / c
            if abs(d) < 1e-300:
                d = 1e-300
            if abs(c) < 1e-300:
                c = 1e-300
            d = 1 / d
            change = d * c
            h *= change
        if abs(change - 1) < 3e-14:
            break
    return h


def regularized_beta(x, a, b):
    """Return the regularized incomplete beta function."""
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0
    scale = math.exp(math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
                     + a * math.log(x) + b * math.log1p(-x))
    if x < (a + 1) / (a + b + 2):
        return scale * beta_fraction(a, b, x) / a
    return 1 - scale * beta_fraction(b, a, 1 - x) / b


def t_cdf(value, degrees):
    """Return the Student t cumulative probability."""
    if math.isinf(value):
        return 1.0 if value > 0 else 0.0
    tail = regularized_beta(degrees / (degrees + value * value), degrees / 2, 0.5) / 2
    return 1 - tail if value >= 0 else tail


@lru_cache(maxsize=4096)
def t_quantile(probability, degrees):
    """Return a positive Student t quantile by bounded bisection."""
    low, high = 0.0, 1.0
    while t_cdf(high, degrees) < probability:
        high *= 2
    for _ in range(70):
        middle = (low + high) / 2
        if t_cdf(middle, degrees) < probability:
            low = middle
        else:
            high = middle
    return (low + high) / 2


def holm(values):
    """Monotone Holm step-down p-values keyed by row name."""
    ordered = sorted(values, key=values.get)
    adjusted, previous = {}, 0.0
    for index, key in enumerate(ordered):
        previous = max(previous, min(1.0, (len(ordered) - index) * values[key]))
        adjusted[key] = previous
    return adjusted


def minimum_seeds(alpha):
    """Smallest balanced seed count whose exact two-sided p can reach alpha."""
    seeds = 2
    while 2 / math.comb(2 * seeds, seeds) > alpha:
        seeds += 1
    return seeds


def permutation_samples(alpha):
    """Keep the Monte Carlo floor at most a twentieth of the cross-check alpha."""
    samples = max(19999, math.ceil(20 / alpha))
    if samples > 2000000:
        raise ValueError("alpha requires more than 2000000 permutation resamples")
    return samples


def permutation(a, b, alpha, rng, samples=None):
    """Two-sided exact or add-one Monte Carlo p-value on seed observations."""
    samples = permutation_samples(alpha) if samples is None else samples
    values = a + b
    n = len(a)
    count = math.comb(len(values), n)
    observed = abs(mean(b) - mean(a))
    total = sum(values)
    def extreme(indices):
        selected = sum(values[i] for i in indices)
        difference = abs((total - selected) / len(b) - selected / n)
        return difference >= observed - 1e-12
    if count <= samples:
        return sum(extreme(indices) for indices in itertools.combinations(range(len(values)), n)) / count
    hits = sum(extreme(rng.sample(range(len(values)), n)) for _ in range(samples))
    return (hits + 1) / (samples + 1)


def row_test(a, b, delta, alpha, rng, permutation_alpha=None):
    """Welch and TOST statistics on positive log seed means."""
    result = dict(a=mean(a), b=mean(b), a_median=median(a),
                  b_median=median(b), ratio=mean(b) / mean(a) if mean(a) else None)
    if min(a + b) <= 0 or min(len(a), len(b)) < 2:
        return dict(result, p=1.0, equivalence=1.0, permutation=None, interval=None)
    log_a, log_b = [math.log(x) for x in a], [math.log(x) for x in b]
    variance_a = variance(log_a) / len(log_a)
    variance_b = variance(log_b) / len(log_b)
    standard_error = math.sqrt(variance_a + variance_b)
    difference = mean(log_b) - mean(log_a)
    if standard_error:
        degrees = (variance_a + variance_b) ** 2 / (
            variance_a ** 2 / (len(log_a) - 1) + variance_b ** 2 / (len(log_b) - 1))
    else:
        degrees = float("inf")
    def cdf_at(value):
        if standard_error:
            return t_cdf(value / standard_error, degrees)
        return 0.0 if value < 0 else 1.0 if value > 0 else 0.5
    p = min(1.0, 2 * min(cdf_at(difference), 1 - cdf_at(difference)))
    lower, upper = math.log1p(-delta), math.log1p(delta)
    equivalence = max(1 - cdf_at(difference - lower), cdf_at(difference - upper))
    width = t_quantile(1 - alpha / 2, degrees) * standard_error if standard_error else 0.0
    return dict(result, log_difference=difference, p=p, equivalence=equivalence,
                permutation=permutation(log_a, log_b, permutation_alpha or alpha, rng),
                interval=[math.exp(difference - width), math.exp(difference + width)])


def compare(a, b, threshold=1.0, alpha=0.05, rng_seed=0, permutation_alpha=None):
    """Decide each row with Holm inside each family and a permutation cross-check.

    Alpha is the share allocated to each decision family for this look.
    The permutation cross-check uses its separate nominal alpha.
    """
    names = sorted(set(a) | set(b))
    rng = random.Random(rng_seed)
    cross_check_alpha = permutation_alpha if permutation_alpha is not None else alpha
    results = {}
    for name in names:
        left, right = a.get(name, []), b.get(name, [])
        if left and right and min(left + right) > 0:
            results[name] = row_test(left, right, threshold / 100, alpha, rng, cross_check_alpha)
        else:
            if (left and min(left) <= 0) or (right and min(right) <= 0):
                verdict = "not measured"
            elif name not in b and left:
                verdict = "removed"
            elif name not in a and right:
                verdict = "added"
            else:
                verdict = "not measured"
            results[name] = dict(a=mean(left) if left else None, b=mean(right) if right else None,
                                 ratio=None, interval=None, permutation=None, p=1.0,
                                 equivalence=1.0, verdict=verdict)
    differences = holm({name: row["p"] for name, row in results.items()})
    equivalents = holm({name: row["equivalence"] for name, row in results.items()})
    for name, row in results.items():
        row.update(p_adjusted=differences[name], equivalence_adjusted=equivalents[name])
        if "verdict" in row:
            continue
        verdict = "inconclusive"
        if equivalents[name] <= alpha:
            verdict = "no change"
        elif (differences[name] <= alpha and row["permutation"] is not None
              and row["permutation"] <= cross_check_alpha
              and (row["ratio"] - 1) * row["log_difference"] > 0):
            verdict = "regressed" if row["ratio"] > 1 else "improved"
        row["verdict"] = verdict
    return results
