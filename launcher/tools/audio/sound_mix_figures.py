"""Plot mixer block cost against active voice count from committed captures."""
import argparse
from pathlib import Path

from audio_doc_tables import MIX_CAPTURES, ROOT, mix_data
from docs.plot_style import FIGURE_SIZE, SLOTS, save_themed_plot, subplots


def render(capture, output):
    capacity, _, _, _, rows, _, _ = mix_data(capture)
    fig, axis = subplots(1, 1, FIGURE_SIZE)
    sounds = dict.fromkeys(row[0] for row in rows)
    for sound, color in zip(sounds, SLOTS):
        samples = sorted((row for row in rows if row[0] == sound), key=lambda row: row[1])
        for column, label, line in ((2, "average", "o-"), (3, "worst", "o--")):
            axis.plot([row[1] for row in samples], [float(row[column]) for row in samples], line,
                      label=f"{sound}, {label}", color=color)
    axis.set(xlabel="Active voices", ylabel="Mixer cost (microseconds/block)", ylim=(0, None))
    axis.set_xlim(left=0)
    axis.set_xticks(sorted({row[1] for row in rows}))
    save_themed_plot(fig, axis, output / f"{capture.stem}.png", f"Mixer capacity: {capacity} voices")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--captures", type=Path, default=ROOT / MIX_CAPTURES)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    for capture in sorted(args.captures.glob("sound_mix_capture*.txt")):
        render(capture, args.out)


if __name__ == "__main__":
    main()
