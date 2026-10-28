"""Plot onset excess medians and capture ranges against de-click time constant."""
import argparse
import math
from pathlib import Path
from statistics import median

from audio_doc_tables import MIX_CAPTURES, ROOT, US_PER_MS, onset_data
from audio.sound_onset_capture import DECLICK_TUNABLE
from docs.plot_style import FIGURE_SIZE, SLOTS, save_themed_plot, subplots

EVENT_ORDER = ("start", "stop", "steal", "amp-on")
BAND_ALPHA = 0.15


def figure_data(captures):
    if not captures:
        raise ValueError("no sound-onset captures")
    settings = captures[0][0]
    if any(list(arms.items()) != list(settings.items()) for arms, _, _ in captures):
        raise ValueError("onset capture arms differ")
    times = [values[DECLICK_TUNABLE] / US_PER_MS for values in settings.values()]
    curves = []
    for event in EVENT_ORDER:
        medians, lows, highs = [], [], []
        for arm in settings:
            values = [median(value for value, _ in points[arm, event])
                      for _, points, _ in captures if points.get((arm, event))]
            medians.append(median(values) if values else math.nan)
            lows.append(min(values) if values else math.nan)
            highs.append(max(values) if values else math.nan)
        curves.append((event, medians, lows, highs))
    return times, curves


def render(captures, output):
    paths = sorted(captures.glob("sound_onset_capture_*.txt"))
    times, curves = figure_data([onset_data(path) for path in paths])
    positions = list(range(len(times)))
    fig, axis = subplots(1, 1, FIGURE_SIZE)
    for (event, medians, lows, highs), color in zip(curves, SLOTS):
        axis.plot(positions, medians, "o-", label=event, color=color)
        axis.fill_between(positions, lows, highs, color=color, alpha=BAND_ALPHA)
    axis.set(xlabel="De-click time constant (ms)", ylabel="Median excess (dB)")
    axis.set_xticks(positions, [f"{time:g}" if time else "off" for time in times])
    save_themed_plot(fig, axis, output / "sound-onset.png", "Onset excess across captures")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--captures", type=Path, default=ROOT / MIX_CAPTURES)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    render(args.captures, args.out)


if __name__ == "__main__":
    main()
