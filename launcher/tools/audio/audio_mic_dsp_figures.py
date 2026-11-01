"""Plot microphone DSP captures in both documentation themes."""

from audio_doc_tables import ROOT, read_capture
from audio_doc_blocks import CLIPPED_DBFS, figures_main
from audio_mic_dsp_sweep import CAPTURE, DEFAULTS, HPF_VOLUME, LEVEL_DB
from audio_mic_dsp_tables import feature, label, mic_data
from docs.plot_style import FIGURE_SIZE, SLOTS, save_themed_plot, subplots

NOISE_ROW_INCHES = 0.28


def render(capture, output):
    _, records, _ = read_capture(capture)
    data = mic_data(records)
    primary_hz = next(int(command[1]) for command, _ in records if command[:1] == ["tone"])
    frequency_volumes = {volume for point in data for hz, volume in point["points"] if hz != primary_hz}
    if len(frequency_volumes) > 1:
        raise ValueError("HPF frequency sweep must hold volume constant")
    frequency_volume = next(iter(frequency_volumes), HPF_VOLUME)
    clipped = [point for point in data if point["peak"] >= CLIPPED_DBFS]
    data = [point for point in data if point not in clipped]
    not_shown = "; clipped, not shown: " + ", ".join(label(p["config"]) for p in clipped) if clipped else ""
    baseline = next((point for point in data if point["config"] == DEFAULTS), None)
    if baseline is None:
        raise ValueError("missing microphone baseline")
    targets = sorted({(point["config"]["alc_max"], point["config"]["alc_min"])
                      for point in data if feature(point["config"]) == "ALC"})
    fig, axes = subplots(max(1, len(targets)), 1, (FIGURE_SIZE[0], FIGURE_SIZE[1] * max(1, len(targets))), squeeze=False)
    for axis, target in zip(axes[:, 0], targets or [None]):
        curves = [baseline] + [point for point in data if feature(point["config"]) == "ALC"
                              and (point["config"]["alc_max"], point["config"]["alc_min"]) == target]
        for point, color in zip(curves, SLOTS):
            levels = sorted((volume, values["tone_db"]) for (hz, volume), values in point["points"].items()
                            if hz == primary_hz)
            if levels:
                axis.plot(*zip(*levels), "o-", color=color, label=label(point["config"]))
        axis.set(xscale="log", xlabel="Tone volume (%)", ylabel="Microphone level (dBFS)",
                 title=f"ALC target max/min {LEVEL_DB[target[0]]:.1f}/{LEVEL_DB[target[1]]:.1f} dB" if target else None)
        axis.legend(fontsize=8, loc="best")
        axis.grid(alpha=0.3)
    from docs.plot_style import save_themes
    save_themes(fig, output / "mic-dsp-level.png")
    fig, axis = subplots(1, 1, (FIGURE_SIZE[0] + 3, max(FIGURE_SIZE[1], len(data) * NOISE_ROW_INCHES)))
    for index, point in enumerate(data):
        axis.plot(point["noise"], index, "o", color=SLOTS[index % len(SLOTS)], label="Noise floor" if index == 0 else None)
    axis.set(yticks=range(len(data)), yticklabels=[feature(p["config"]) + ": " + label(p["config"]) for p in data],
             xlabel="Broadband noise floor (dBFS)")
    axis.invert_yaxis()
    save_themed_plot(fig, axis, output / "mic-dsp-noise.png", "Silence after settling" + not_shown)
    fig, axis = subplots(1, 1, FIGURE_SIZE)
    for index, point in enumerate(data):
        config = point["config"]
        if feature(config) not in ("Baseline", "HPF") or not config["hpf"]:
            continue
        levels = sorted((hz, values["tone_db"]) for (hz, volume), values in point["points"].items()
                        if volume == frequency_volume)
        if levels:
            axis.plot(*zip(*levels), "o-", color=SLOTS[index % len(SLOTS)],
                      label=f"s1/s2 {config['hpf_s1']}/{config['hpf_s2']}")
    axis.set(xscale="log", xlabel="Frequency (Hz)", ylabel="Microphone level (dBFS)")
    save_themed_plot(fig, axis, output / "mic-dsp-hpf.png", "HPF coefficients" + not_shown)


def main():
    figures_main(__doc__, CAPTURE, render)


if __name__ == "__main__":
    main()
