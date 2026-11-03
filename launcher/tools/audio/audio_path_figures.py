"""Plot acoustic level sweeps from committed tone replies."""
from matplotlib.ticker import NullLocator

from audio_doc_tables import CAPTURE, ROOT, path_data, sweep_data
from audio_doc_blocks import figures_main, line_chart
from docs.plot_style import SLOTS, save_themed_plot


def render(capture, output):
    _, tones, _ = path_data(capture)
    for name, column, context, samples in sweep_data(tones):
        if name == "audio-duration":
            continue
        samples = sorted(samples)
        fig, axis = line_chart(
            [(label, [(value, reply[field]) for value, reply in samples], "o-", color)
             for field, label, color in zip(("tone_db", "silence_db"), ("Tone", "Silence"), SLOTS)],
            xlabel=column, ylabel="Microphone level (dBFS)", xscale="log", ylim=(None, 0))
        axis.set_xticks([value for value, _ in samples], [str(value) for value, _ in samples])
        axis.xaxis.set_minor_locator(NullLocator())
        save_themed_plot(fig, axis, output / f"{name}.png", context)


if __name__ == "__main__":
    figures_main(__doc__, ROOT / CAPTURE, render)
