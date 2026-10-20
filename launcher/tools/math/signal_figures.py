#!/usr/bin/env python3
"""Plot library responses for docs/math/Signal.md; --cc CC --out DIR."""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from math_figures import PANEL_SIZE, PANEL_TITLE_ROOM, ROOT, run_probe
from docs.plot_style import FAINT, LABEL_SIZE, SLOTS, save, save_gif, subplots
import numpy as np

PROBE = pathlib.Path(__file__).with_name("signal_figures_probe.c")
SOURCES = (PROBE, *(ROOT / "launcher/packages/math/src/signal" / name for name in ("goertzel.c", "pitch.c")))
GIF_FRAMES = 60
GIF_FPS = 10
PANELS = 2
AMPLITUDE_MAX = 1.1
REFERENCE_ALPHA = 0.6
LEGEND_ANCHOR = (0.5, -0.22)
SWEEP_HEIGHT_PANELS = 1.4
SWEEP_DPI = 80


def response_figure(data, sample_rate, output):
    fig, axis = subplots(1, 1, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    for index, (row, color) in enumerate(zip(data, SLOTS)):
        samples = np.array(row["samples"])
        bins = samples[:, 0]
        hz = bins * sample_rate / row["n"]
        axis.plot(hz, samples[:, 1], color=color, label=f"N = {row['n']}")
        reference = abs(np.sinc(bins) / np.sinc(bins / row["n"]))
        axis.plot(hz, reference, "--", color=color, alpha=REFERENCE_ALPHA,
                  label="Dirichlet reference" if index == 0 else None)
    axis.set(title=f"Detector response at {sample_rate:g} samples/s", xlabel="Tone offset from detector (Hz)",
             ylabel="Amplitude", ylim=(0, None))
    axis.legend(fontsize=LABEL_SIZE, loc="upper center", bbox_to_anchor=LEGEND_ANCHOR, ncol=PANELS)
    save(fig, output)


def sweep_animation(data, output):
    fig, (frequency, detector) = subplots(PANELS, 1, (PANEL_SIZE * PANELS, PANEL_SIZE * SWEEP_HEIGHT_PANELS))
    fig.set_dpi(SWEEP_DPI)
    times = np.array([row["time"] for row in data["sweep"]])
    hz = np.array([row["hz"] for row in data["sweep"]])
    amplitudes = np.array([row["amplitude"] for row in data["sweep"]])
    frequency.plot(times, hz, color=FAINT)
    lines = []
    for tone, color in zip(data["detectors"], SLOTS):
        frequency.axhline(tone, color=color, alpha=REFERENCE_ALPHA, linewidth=1)
        line, = detector.plot([], [], color=color, label=f"{tone:g} Hz")
        lines.append(line)
    cursor = frequency.axvline(times[0], color=SLOTS[0])
    frequency.set(xlabel="Time (s)", ylabel="Frequency (Hz)", title="Linear chirp; cursor at current block")
    detector.set(xlabel="Sweep frequency (Hz)", ylabel="Amplitude", xlim=(hz[0], hz[-1]),
                 ylim=(0, AMPLITUDE_MAX), title="Fresh detector window per block")
    detector.legend(fontsize=LABEL_SIZE, loc="upper right")

    def draw(index):
        cursor.set_xdata([times[index], times[index]])
        for column, line in enumerate(lines):
            line.set_data(hz[:index + 1], amplitudes[:index + 1, column])

    save_gif(fig, draw, GIF_FRAMES, output, GIF_FPS)


def pitch_figure(data, output):
    fig, axis = subplots(1, 1, (PANEL_SIZE * PANELS, PANEL_SIZE + PANEL_TITLE_ROOM))
    samples = np.array(data["pitch"])
    axis.plot(*samples.T, color=SLOTS[0])
    octave = samples[np.isclose(samples[:, 0] % data["octave"], 0)]
    axis.scatter(*octave.T, color=SLOTS[1])
    axis.set_xticks(octave[:, 0])
    axis.set_yticks(np.r_[0, octave[:, 1]])
    axis.grid(color=FAINT)
    axis.set(title=f"Octaves double frequency (reference {data['reference_hz']:g} Hz)",
             xlabel="Semitones from reference", ylabel="Frequency (Hz)", ylim=(0, None))
    save(fig, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    data = run_probe(args.cc, SOURCES)
    response_figure(data["response"], data["sample_rate_hz"], args.out / "goertzel-response.png")
    sweep_animation(data, args.out / "goertzel-sweep.gif")
    pitch_figure(data, args.out / "pitch-curve.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
