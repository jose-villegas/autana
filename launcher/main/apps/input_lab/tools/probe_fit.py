#!/usr/bin/env python3
"""Fit how the touch panel misreports taps, from Input Lab console captures.

    python probe_fit.py CAPTURE.log [CAPTURE.log ...]

Reads every "probe" line. Taps logged with the touch correction off (cal 0)
are fitted, per round and pooled, as where the panel reported each tap
against where it was aimed, both in panel coordinates:

    reported_x = xx*aim_x + xy*aim_y + x0      reported_y = yx*aim_x + yy*aim_y + y0

The pooled line is printed ready to paste as the touch driver's PANEL_FIT.
Taps logged with the correction on (cal 1) are not fitted - they are already
corrected - and are scored instead: how far each landed from its aim.

Taps pinned at the panel's edge and taps on corner targets are left out: an
edge report is clamped, and a corner target can sit under the rounded glass.
Each orientation's fit is also tried on the other orientation.
"""

import re
import statistics
import sys

PANEL_W, PANEL_H = 368, 448
CORNER = 40
BUTTON_HALF = 28

PROBE = re.compile(
    r"probe \d+ (?P<mode>\w+) q(?P<q>\d) aim -?\d+,-?\d+ aim_panel (?P<ax>-?\d+),(?P<ay>-?\d+) "
    r"first -?\d+,-?\d+ settled -?\d+,-?\d+ release -?\d+,-?\d+ panel_first (?P<fx>-?\d+),(?P<fy>-?\d+)"
    r".* cal (?P<cal>-?\d+)"
)


def parse(lines):
    probes = []
    for line in lines:
        m = PROBE.search(line)
        if m:
            probes.append({k: v if k == "mode" else int(v) for k, v in m.groupdict().items()})
    return probes


def read_probes(paths):
    probes = []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            probes += parse(f)
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
    """((xx, xy, x0), (yx, yy, y0)) of reported against aimed."""
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


def inside_button(distances):
    return sum(1 for d in distances if d <= BUTTON_HALF) / len(distances)


def panel_fit_line(cal):
    (xx, xy, x0), (yx, yy, y0) = cal
    return f".xx = {xx:.3f}f, .xy = {xy:.3f}f, .x0 = {x0:.1f}f, .yx = {yx:.3f}f, .yy = {yy:.3f}f, .y0 = {y0:.1f}f,"


def describe(name, probes, cal):
    (a, b, c), (d, e, f) = cal
    sx, sy = residual_sd(probes, cal)
    print(f"{name:22} n={len(probes):3}  x = {a:.3f}ax {b:+.3f}ay {c:+6.1f}   y = {d:+.3f}ax {e:.3f}ay {f:+6.1f}"
          f"   scatter {sx:4.1f},{sy:4.1f} px")


def score(name, probes):
    d = misses(probes)
    print(f"{name:22} n={len(probes):3}  median miss {statistics.median(d):4.1f} px, "
          f"inside a {2 * BUTTON_HALF} px button {inside_button(d):4.0%}")


def main(paths):
    probes = [p for p in read_probes(paths) if usable(p)]
    raw = [p for p in probes if p["cal"] == 0 and p["mode"] == "random"]
    checked = [p for p in probes if p["cal"] == 1 and p["mode"] == "random"]

    for q in (0, 1):
        group = [p for p in checked if p["q"] == q]
        if group:
            score(f"corrected q{q}", group)
    if len(raw) < 6:
        if checked:
            return
        sys.exit("no uncorrected (cal 0) random probes to fit")

    for q in (0, 1):
        group = [p for p in raw if p["q"] == q]
        if len(group) >= 6:
            describe(f"uncorrected q{q}", group, fit(group))
    pooled = fit(raw)
    describe("uncorrected, pooled", raw, pooled)
    print(f"PANEL_FIT: {panel_fit_line(pooled)}")

    for train, test in ((0, 1), (1, 0)):
        trained = [p for p in raw if p["q"] == train]
        tested = [p for p in raw if p["q"] == test]
        if len(trained) < 6 or len(tested) < 6:
            continue
        cal = fit(trained)
        before, after = misses(tested), misses(tested, cal)
        print(f"fit on q{train}, tried on q{test}: median miss {statistics.median(before):4.1f} -> "
              f"{statistics.median(after):4.1f} px, inside a {2 * BUTTON_HALF} px button "
              f"{inside_button(before):4.0%} -> {inside_button(after):4.0%}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
