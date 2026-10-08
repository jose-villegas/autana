"""Markdown tables over a device capture's per-phase frame costs.

A run's phases map a phase name to the numbers its log line carried:
min/max/avg/med/p95 in us, or the average alone.
"""

STATS = ("min", "max", "avg", "med", "p95")


def phase_stats(match):
    """The min/max/avg/med/p95 a phase line's regex captured, by group name."""
    return {name: int(value) for name, value in match.groupdict().items() if name in STATS}


def fps(us: int) -> str:
    return f"{1000000.0 / us:.1f}" if us > 0 else "?"


def comparison_lines(runs, labels, phase_order):
    """Each phase's average across `labels`, then Total's avg/median/p95 as fps."""
    lines = ["## Comparison (average, us)", ""]
    lines.append("| Phase | " + " | ".join(f"`{label}`" for label in labels) + " |")
    lines.append("|---|" + "---:|" * len(labels))
    for phase in phase_order:
        row = [f"{phase} (us)"]
        for label in labels:
            p = runs[label]["phases"].get(phase)
            row.append(str(p["avg"]) if p else "?")
        lines.append("| " + " | ".join(row) + " |")
    lines.append("")
    lines.append("| | " + " | ".join(labels) + " |")
    lines.append("|---|" + "---:|" * len(labels))
    for stat, stat_label in (("avg", "avg"), ("med", "median"), ("p95", "p95")):
        row = [f"**Total fps ({stat_label})**"]
        for label in labels:
            total = runs[label]["phases"].get("Total")
            row.append(fps(total[stat]) if total and stat in total else "?")
        lines.append("| " + " | ".join(row) + " |")
    lines.append("")
    return lines


def write_phase_report(path, lines, runs, labels, phase_order, heading):
    """Writes `lines`, the comparison, then each run under `heading(label, run)` in full."""
    lines = lines + comparison_lines(runs, labels, phase_order)
    for label in labels:
        lines += [heading(label, runs[label]), ""] + phase_table_lines(runs[label]["phases"], phase_order)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def phase_table_lines(phases, phase_order):
    """One run's phases in full; an average-only phase shows dashes elsewhere."""
    lines = ["| Phase | Min (us) | Max (us) | Avg (us) | Median (us) | P95 (us) |",
             "|---|---:|---:|---:|---:|---:|"]
    for phase in phase_order:
        p = phases.get(phase)
        if p is None:
            lines.append(f"| {phase} | ? | ? | ? | ? | ? |")
        elif "min" in p:
            bold = "**" if phase == "Total" else ""
            lines.append(f"| {bold}{phase}{bold} | {p['min']} | {p['max']} | "
                         f"{p['avg']} | {p['med']} | {p['p95']} |")
        else:
            lines.append(f"| {phase} | - | - | {p['avg']} | - | - |")
    lines.append("")
    return lines
