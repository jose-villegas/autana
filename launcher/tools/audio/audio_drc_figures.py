"""Plot committed limiter measurements; --capture PATH --out DIR."""
import argparse
import math
from pathlib import Path

from audio_doc_tables import DRC_CAPTURE, DRC_DEFAULT, DRC_REFERENCE_CODEC, ROOT, drc_data, read_capture
from audio_drc_sweep import DB_AMPLITUDE_SCALE, VOLUME_MAX
from docs.plot_style import FIGURE_SIZE, SLOTS, save_themed_plot, subplots

BASELINE_CODECS = (30, DRC_REFERENCE_CODEC, DRC_DEFAULT[0])
DEFAULT_MARKER_ORDER = 3


def figure_data(volumes, points):
    if not volumes or min(volumes) <= 0 or max(volumes) != VOLUME_MAX:
        raise ValueError("figures need positive tone volumes through full scale")
    if DRC_DEFAULT not in points:
        raise ValueError("missing default DRC configuration")
    inputs = [DB_AMPLITUDE_SCALE * math.log10(volume / VOLUME_MAX) for volume in volumes]
    curves = []
    for codec in BASELINE_CODECS:
        matches = [point for key, point in points.items() if key[:2] == (codec, 0)]
        if len(matches) != 1:
            raise ValueError(f"expected one DRC-off configuration at codec {codec}")
        curves.append((f"Codec {codec}%, DRC off", inputs, matches[0]["levels"]))
    for codec, label in ((DRC_DEFAULT[0], "Default"), (VOLUME_MAX, "Same limit")):
        key = (codec, *DRC_DEFAULT[1:])
        if key not in points:
            raise ValueError(f"missing limited configuration at codec {codec}")
        curves.append((f"{label}: codec {codec}%", inputs, points[key]["levels"]))
    thd = sorted((key[0], point["thd"]) for key, point in points.items() if not key[1])
    if any(value is None for _, value in thd) or points[DRC_DEFAULT]["thd"] is None:
        raise ValueError("missing full-scale THD")
    return curves, thd, (DRC_DEFAULT[0], points[DRC_DEFAULT]["thd"])


def render(capture, output):
    _, records, _ = read_capture(capture)
    hz, volumes, points = drc_data(records)
    curves, thd, default = figure_data(volumes, points)
    fig, axis = subplots(1, 1, FIGURE_SIZE)
    for (label, inputs, levels), color in zip(curves, SLOTS):
        axis.plot(inputs, levels, "o-", label=label, color=color)
    axis.set(xlabel="Tone level (dB re full scale)", ylabel="Microphone level (dBFS)",
             xlim=(min(curves[0][1]), 0), ylim=(None, 0))
    save_themed_plot(fig, axis, output / "drc-level.png", f"Output vs input at {hz} Hz")
    fig, axis = subplots(1, 1, FIGURE_SIZE)
    axis.plot(*zip(*thd), "o-", color=SLOTS[0], label="DRC off")
    axis.scatter(*default, marker="D", color=SLOTS[1], label=f"Default: codec {default[0]}%", zorder=DEFAULT_MARKER_ORDER)
    axis.set(xlabel="Codec volume (%)", ylabel="THD (dB relative to fundamental)", ylim=(None, 0))
    save_themed_plot(fig, axis, output / "drc-thd.png", f"Full-scale distortion at {hz} Hz", legend_columns=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, default=ROOT / DRC_CAPTURE)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    render(args.capture, args.out)


if __name__ == "__main__":
    main()
