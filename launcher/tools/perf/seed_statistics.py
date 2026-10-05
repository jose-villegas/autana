"""Inference on independent layout seed means, without external dependencies."""
import itertools
import math
import random
from statistics import mean, median, variance


def beta_fraction(a, b, x):
    qab, qap, qam = a + b, a + 1, a - 1
    c = 1.0
    d = 1 - qab * x / qap
    d = 1 / max(abs(d), 1e-300) * (1 if d >= 0 else -1)
    h = d
    for m in range(1, 301):
        for aa in (m * (b - m) * x / ((qam + 2*m) * (a + 2*m)),
                   -(a + m) * (qab + m) * x / ((a + 2*m) * (qap + 2*m))):
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
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0
    scale = math.exp(math.lgamma(a+b) - math.lgamma(a) - math.lgamma(b)
                     + a * math.log(x) + b * math.log1p(-x))
    if x < (a+1) / (a+b+2):
        return scale * beta_fraction(a, b, x) / a
    return 1 - scale * beta_fraction(b, a, 1-x) / b


def t_cdf(value, degrees):
    if math.isinf(value):
        return 1.0 if value > 0 else 0.0
    tail = regularized_beta(degrees / (degrees + value*value), degrees/2, 0.5) / 2
    return 1-tail if value >= 0 else tail


def t_quantile(probability, degrees):
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
    ordered = sorted(values, key=values.get)
    adjusted, previous = {}, 0.0
    for index, key in enumerate(ordered):
        previous = max(previous, min(1.0, (len(ordered)-index) * values[key]))
        adjusted[key] = previous
    return adjusted


def minimum_seeds(alpha):
    seeds = 2
    while 2 / math.comb(2*seeds, seeds) > alpha:
        seeds += 1
    return seeds


def permutation(a, b, alpha, rng, samples=19999):
    values = a + b
    n = len(a)
    count = math.comb(len(values), n)
    if 2 / count > alpha:
        return None
    observed = abs(mean(b) - mean(a))
    total = sum(values)
    def extreme(indices):
        selected = sum(values[i] for i in indices)
        difference = abs((total-selected)/len(b) - selected/n)
        return difference >= observed - 1e-12
    if count <= samples:
        return sum(extreme(indices) for indices in itertools.combinations(range(len(values)), n)) / count
    hits = sum(extreme(rng.sample(range(len(values)), n)) for _ in range(samples))
    return (hits + 1) / (samples + 1)


def row_test(a, b, delta, alpha, rng):
    result = dict(a=mean(a), b=mean(b), a_median=median(a),
                  b_median=median(b), ratio=mean(b)/mean(a) if mean(a) else None)
    if min(a+b) <= 0 or min(len(a), len(b)) < 2:
        return dict(result, p=1.0, equivalence=1.0, permutation=None, interval=None)
    la, lb = [math.log(x) for x in a], [math.log(x) for x in b]
    va, vb = variance(la)/len(la), variance(lb)/len(lb)
    se = math.sqrt(va + vb)
    difference = mean(lb) - mean(la)
    degrees = ((va+vb)**2 / (va*va/(len(la)-1) + vb*vb/(len(lb)-1))) if se else float('inf')
    def cdf_at(value):
        if se:
            return t_cdf(value/se, degrees)
        return 0.0 if value < 0 else 1.0 if value > 0 else 0.5
    p = min(1.0, 2 * min(cdf_at(difference), 1-cdf_at(difference)))
    lower, upper = math.log1p(-delta), math.log1p(delta)
    equivalence = max(1-cdf_at(difference-lower), cdf_at(difference-upper))
    width = t_quantile(1-alpha/2, degrees) * se if se else 0.0
    return dict(result, log_difference=difference, p=p, equivalence=equivalence,
                permutation=permutation(la, lb, alpha, rng),
                interval=[math.exp(difference-width), math.exp(difference+width)])


def compare(a, b, threshold=1.0, alpha=0.05, rng_seed=0, family=None):
    names = sorted(set(a) | set(b))
    rng = random.Random(rng_seed)
    results = {name: row_test(a[name], b[name], threshold/100, alpha, rng)
               for name in names if a.get(name) and b.get(name)}
    family = set(family or names)
    p = holm({name: results[name]['p'] if name in results else 1 for name in family})
    eq = holm({name: results[name]['equivalence'] if name in results else 1 for name in family})
    for name in names:
        if name not in results:
            results[name] = dict(a=None, b=None, ratio=None, interval=None, permutation=None)
        row = results[name]
        row.update(p_adjusted=p.get(name, 1), equivalence_adjusted=eq.get(name, 1))
        verdict = 'inconclusive'
        if eq.get(name, 1) <= alpha:
            verdict = 'no change'
        elif (p.get(name, 1) <= alpha and row['permutation'] is not None
              and row['permutation'] <= alpha and (row['ratio']-1)*row['log_difference'] > 0):
            verdict = 'regressed' if row['ratio'] > 1 else 'improved'
        row['verdict'] = verdict
    return results
