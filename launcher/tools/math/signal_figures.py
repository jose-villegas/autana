#!/usr/bin/env python3
"""Plot library responses for docs/math/Signal.md; --cc CC --out DIR."""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from math_figures import PANEL_SIZE, PANEL_TITLE_ROOM, ROOT, run_probe
from docs.plot_style import FAINT, LABEL_SIZE, SLOTS, TITLE_SIZE, save, save_gif, subplots
import numpy as np

PROBE = pathlib.Path(__file__).with_name("signal_figures_probe.c")
SOURCES = (PROBE, *(ROOT / "launcher/packages/math/src/signal" / name for name in ("goertzel.c", "pitch.c")))
GIF_FRAMES = 60
GIF_FPS = 10
PANELS = 2
BAR_MAX = 1.1
WAVE_MAX = 1.1
REFERENCE_ALPHA = 0.7
LEGEND_ANCHOR = (0.5, -0.22)
SWEEP_HEIGHT_PANELS = 1.4
SWEEP_DPI = 80


def response_figure(data, output):
    fig, axis = subplots(1, 1, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    for row, color in zip(data, SLOTS):
        samples = np.array(row["samples"])
        axis.plot(*samples.T, color=color, label=f"N = {row['n']}")
    row = data[-1]
    bins = np.array(row["samples"])[:, 0]
    reference = abs(np.sinc(bins) / np.sinc(bins / row["n"]))
    axis.plot(bins, reference, "--", color=FAINT, alpha=REFERENCE_ALPHA, label="Dirichlet reference (positive-frequency term)")
    axis.set(title="Unit sine around one detector bin", xlabel="Tone offset from detector (bins of each N)",
             ylabel="goertzel_amplitude", ylim=(0, None))
    axis.legend(fontsize=LABEL_SIZE, loc="upper center", bbox_to_anchor=LEGEND_ANCHOR, ncol=PANELS)
    save(fig, output)


def sweep_animation(data, output):
    fig, (wave, detector) = subplots(PANELS, 1, (PANEL_SIZE * PANELS, PANEL_SIZE * SWEEP_HEIGHT_PANELS))
    fig.set_dpi(SWEEP_DPI)
    samples = np.array(data["sweep"][0]["wave"])
    trace, = wave.plot(*samples.T, color=SLOTS[0])
    wave.set(xlabel="Time within block (s)", ylabel="Sample", ylim=(-WAVE_MAX, WAVE_MAX))
    labels = [f"{hz:g} Hz" for hz in data["detectors"]]
    bars = detector.bar(labels, data["sweep"][0]["amplitude"], color=SLOTS[:len(labels)])
    detector.set(ylabel="goertzel_amplitude", ylim=(0, BAR_MAX), title="Independent window per block")

    def draw(index):
        row = data["sweep"][index]
        trace.set_data(*np.array(row["wave"]).T)
        for bar, amplitude in zip(bars, row["amplitude"]):
            bar.set_height(amplitude)
        wave.set_title(f"Linear chirp — block centre {row['hz']:.0f} Hz", fontsize=TITLE_SIZE)

    save_gif(fig, draw, GIF_FRAMES, output, GIF_FPS)


def pitch_figure(data, output):
    fig, axis = subplots(1, 1, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    samples = np.array(data["pitch"])
    axis.plot(*samples.T, color=SLOTS[0])
    octave = samples[np.isclose(samples[:, 0] % data["octave"], 0)]
    axis.scatter(*octave.T, color=SLOTS[1])
    axis.set_xticks(octave[:, 0])
    axis.set_yticks(octave[:, 1])
    axis.grid(color=FAINT)
    axis.set(title=f"Equal temperament from {data['reference_hz']:g} Hz; each octave doubles frequency",
             xlabel="Semitones from reference", ylabel="Frequency (Hz)", ylim=(0, None))
    save(fig, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    data = run_probe(args.cc, SOURCES)
    response_figure(data["response"], args.out / "goertzel-response.png")
    sweep_animation(data, args.out / "goertzel-sweep.gif")
    pitch_figure(data, args.out / "pitch-curve.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
