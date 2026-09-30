"""Summarises setup-probe captures: per variant, stop and core, the cumulative
time and each stage's increment, as min/median/max over the poses of the
per-pose median across runs. Measurement only, never ships.

    python aggregate.py CAPTURE.log [CAPTURE.log ...]
"""

import re
import statistics
import sys
from collections import defaultdict

MHZ = 240.0
STOPS = ["walk", "fetch", "reject", "color", "setup", "rows", "spanset", "whole"]
SP = re.compile(r"SP v=(\S+) t=(\d+) stop=(\S+) (.*)")
SPT = re.compile(r"SPT v=(\S+) t=(\d+) clusters=\d+ transform_cyc=(\d+) mid=\d+ sub0=(\d+) sub1=(\d+)")

# data[variant][stop][core][pose] -> list of per-run dicts
data = defaultdict(lambda: defaultdict(lambda: defaultdict(lambda: defaultdict(list))))
subs = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
transform = defaultdict(lambda: defaultdict(list))
runs = 0
for path in sys.argv[1:]:
    text = open(path, encoding="utf-8", errors="replace").read()
    for m in SPT.finditer(text):
        v, t = m.group(1), int(m.group(2))
        transform[v][t].append(int(m.group(3)))
        subs[v][0][t].append(int(m.group(4)))
        subs[v][1][t].append(int(m.group(5)))
    for m in SP.finditer(text):
        v, t, stop = m.group(1), int(m.group(2)), m.group(3)
        fields = dict(kv.split("=") for kv in m.group(4).split())
        for core in (0, 1):
            data[v][stop][core][t].append({k[3:]: int(x) for k, x in fields.items() if k.startswith(f"c{core}_")})
    runs += text.count("batch:") or 1


def per_pose(v, stop, core, key):
    return {t: statistics.median(r[key] for r in rs) for t, rs in data[v][stop][core].items()}


def mmm(values):
    values = sorted(values)
    return f"{min(values):6.2f} {statistics.median(values):6.2f} {max(values):6.2f}"


for v in data:
    print(f"\n== {v}: ms per core, min/median/max over poses (per-pose median of the runs)")
    tr = [statistics.median(x) / MHZ / 1000 for x in transform[v].values()]
    print(f"transform, one core (all parts): {mmm(tr)}")
    print(f"{'stop':8} {'core0 cumulative':>21} | {'core1 cumulative':>21} | {'core0 increment':>21} | "
          f"{'core1 increment':>21} | cyc/tri c0 c1 | dstall% of incr c0 c1 | insn/tri c0")
    prev = None
    for stop in STOPS:
        if stop not in data[v]:
            continue
        row = [stop.ljust(8)]
        cum = {c: per_pose(v, stop, c, "cyc") for c in (0, 1)}
        stall = {c: per_pose(v, stop, c, "dstall") for c in (0, 1)}
        insn = {c: per_pose(v, stop, c, "insn") for c in (0, 1)}
        for c in (0, 1):
            row.append(mmm([x / MHZ / 1000 for x in cum[c].values()]))
        incs = {}
        for c in (0, 1):
            if prev is None:
                incs[c] = cum[c]
                istall = stall[c]
                iinsn = insn[c]
            else:
                pc = per_pose(v, prev, c, "cyc")
                incs[c] = {t: cum[c][t] - pc[t] for t in cum[c]}
            row.append(mmm([x / MHZ / 1000 for x in incs[c].values()]))
        cyc_tri = []
        stall_share = []
        for c in (0, 1):
            sub = {t: statistics.median(x) for t, x in subs[v][c].items()}
            cyc_tri.append(sum(incs[c].values()) / max(1, sum(sub.values())))
            ps = per_pose(v, prev, c, "dstall") if prev else {t: 0 for t in stall[c]}
            ds = sum(stall[c][t] - ps[t] for t in stall[c])
            stall_share.append(100.0 * ds / max(1, sum(incs[c].values())))
        pi = per_pose(v, prev, 0, "insn") if prev else {t: 0 for t in insn[0]}
        sub0 = sum(statistics.median(x) for x in subs[v][0].values())
        insn_tri = sum(insn[0][t] - pi[t] for t in insn[0]) / max(1, sub0)
        cum_share = [100.0 * sum(stall[c].values()) / max(1, sum(cum[c].values())) for c in (0, 1)]
        print(" | ".join(row) + f" | {cyc_tri[0]:5.0f} {cyc_tri[1]:5.0f} | {stall_share[0]:5.1f} {stall_share[1]:5.1f} |"
              f" {insn_tri:5.0f} | cum dstall% {cum_share[0]:4.1f} {cum_share[1]:4.1f}")
        prev = stop
    whole = {c: per_pose(v, "whole", c, "cyc") for c in (0, 1)}
    frame = [max(whole[0][t], whole[1][t]) / MHZ / 1000 for t in whole[0]]
    print(f"draw wall (slower core) mean over poses: {statistics.mean(frame):.2f} ms; "
          f"setup stop slower core mean: "
          f"{statistics.mean(max(per_pose(v, 'setup', 0, 'cyc')[t], per_pose(v, 'setup', 1, 'cyc')[t]) for t in whole[0]) / MHZ / 1000:.2f} ms")
print(f"\ncaptures: {len(sys.argv) - 1}")
