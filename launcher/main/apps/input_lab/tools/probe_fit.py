#!/usr/bin/env python3
"""Fit how the touch panel misreports taps, from Input Lab console captures.

    python probe_fit.py CAPTURE.log [CAPTURE.log ...]

Reads every "probe" line, and fits, per round and pooled, where the panel
reported each tap against where it was aimed, in raw panel coordinates:

    reported_x = a*aim_x + b*aim_y + c      reported_y = d*aim_x + e*aim_y + f

Taps pinned at the panel's edge and taps on corner targets are left out: an
edge report is clamped, and a corner target can sit under the rounded glass.
Each orientation's fit is then tried on the other one, since a correction
that is the panel's own holds in any orientation and one that is the hand's
does not.
"""

import re
import statistics
import sys

PANEL_W, PANEL_H = 368, 448
CORNER = 40
BUTTON_HALF = 28

PROBE = re.compile(
    r"probe \d+ (?P<mode>\w+) q(?P<q>\d) aim -?\d+,-?\d+ aim_raw (?P<ax>-?\d+),(?P<ay>-?\d+) "
    r"first -?\d+,-?\d+ settled -?\d+,-?\d+ release -?\d+,-?\d+ raw_first (?P<fx>-?\d+),(?P<fy>-?\d+)"
)


def read_probes(paths):
    probes = []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = PROBE.search(line)
                if m:
                    probes.append({k: (v if k == "mode" else int(v)) for k, v in m.groupdict().items()})
    return probes


def usable(p):
    clamped = not (2 < p["fx"] < PANEL_W - 2 and 2 < p["fy"] < PANEL_H - 2)
    in_corner = (p["ax"] < CORNER or p["ax"] > PANEL_W - CORNER) and (p["ay"] < CORNER or p["ay"] > PANEL_H - CORNER)
    return not clamped and not in_corner


def solve3(m, v):
    rows = [m[i][:] + [v[i]] for i in range(3)]
    for c in range(3):
        pivot = max(range(c, 3), key=lambda r: abs(rows[r][c]))
        rows[c], rows[pivot] = rows[pivot], rows[c]
        for r in range(3):
            if r != c:
                k = rows[r][c] / rows[c][c]
                rows[r] = [a - k * b for a, b in zip(rows[r], rows[c])]
    return [rows[i][3] / rows[i][i] for i in range(3)]


def fit_axis(probes, reported):
    m = [[0.0] * 3 for _ in range(3)]
    v = [0.0] * 3
    for p in probes:
        basis = (p["ax"], p["ay"], 1.0)
        for i in range(3):
            v[i] += basis[i] * p[reported]
            for j in range(3):
                m[i][j] += basis[i] * basis[j]
    return solve3(m, v)


def fit(probes):
    return fit_axis(probes, "fx"), fit_axis(probes, "fy")


def residual_sd(probes, cal):
    cx, cy = cal
    ex = [p["fx"] - (cx[0] * p["ax"] + cx[1] * p["ay"] + cx[2]) for p in probes]
    ey = [p["fy"] - (cy[0] * p["ax"] + cy[1] * p["ay"] + cy[2]) for p in probes]
    return statistics.pstdev(ex), statistics.pstdev(ey)


def corrected(p, cal):
    """Inverts the fitted map for one tap: where it was really aimed."""
    (a, b, c), (d, e, f) = cal
    det = a * e - b * d
    x, y = p["fx"] - c, p["fy"] - f
    return (e * x - b * y) / det, (a * y - d * x) / det


def misses(probes, cal=None):
    out = []
    for p in probes:
        x, y = corrected(p, cal) if cal else (p["fx"], p["fy"])
        out.append(((x - p["ax"]) ** 2 + (y - p["ay"]) ** 2) ** 0.5)
    return out


def describe(name, probes, cal):
    (a, b, c), (d, e, f) = cal
    sx, sy = residual_sd(probes, cal)
    print(f"{name:22} n={len(probes):3}  x = {a:.3f}ax {b:+.3f}ay {c:+6.1f}   y = {d:+.3f}ax {e:.3f}ay {f:+6.1f}"
          f"   scatter {sx:4.1f},{sy:4.1f} px")


def main(paths):
    probes = [p for p in read_probes(paths) if usable(p)]
    if not probes:
        sys.exit("no usable probe lines")

    rounds = {}
    for p in probes:
        rounds.setdefault((p["mode"], p["q"]), []).append(p)
    for (mode, q), group in sorted(rounds.items()):
        if len(group) >= 6:
            describe(f"{mode} q{q}", group, fit(group))

    random = [p for p in probes if p["mode"] == "random"]
    pooled = fit(random)
    describe("random, pooled", random, pooled)

    print()
    for train, test in ((0, 1), (1, 0)):
        trained = [p for p in random if p["q"] == train]
        tested = [p for p in random if p["q"] == test]
        if len(trained) < 6 or len(tested) < 6:
            continue
        cal = fit(trained)
        before, after = misses(tested), misses(tested, cal)
        hit = lambda m: sum(1 for d in m if d <= BUTTON_HALF) / len(m)
        print(f"fit on q{train}, tried on q{test}: median miss {statistics.median(before):4.1f} -> "
              f"{statistics.median(after):4.1f} px, inside a {2 * BUTTON_HALF} px button {hit(before):4.0%} -> {hit(after):4.0%}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
