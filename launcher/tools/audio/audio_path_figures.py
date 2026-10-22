"""Plot acoustic level sweeps from committed tone replies."""
import argparse
from pathlib import Path
from matplotlib.ticker import NullLocator

from audio_doc_tables import CAPTURE, ROOT, path_data, sweep_data
from docs.plot_style import FIGURE_SIZE, SLOTS, save_themed_plot, subplots


def render(capture, output):
    _, tones, _ = path_data(capture)
    for name, column, context, samples in sweep_data(tones):
        if name == "audio-duration":
            continue
        samples = sorted(samples)
        fig, axis = subplots(1, 1, FIGURE_SIZE)
        for field, label, color in zip(("tone_db", "silence_db"), ("Tone", "Silence"), SLOTS):
            axis.plot([value for value, _ in samples], [reply[field] for _, reply in samples],
                      "o-", label=label, color=color)
        axis.set(xlabel=column, ylabel="Microphone level (dBFS)", xscale="log", ylim=(None, 0))
        axis.set_xticks([value for value, _ in samples], [str(value) for value, _ in samples])
        axis.xaxis.set_minor_locator(NullLocator())
        save_themed_plot(fig, axis, output / f"{name}.png", context)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, default=ROOT / CAPTURE)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    render(args.capture, args.out)


if __name__ == "__main__":
    main()
