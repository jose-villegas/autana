"""Plot measured EQ responses over designs and the HPF code map in both themes."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'launcher/tools/audio'))
import numpy as np
from audio_doc_tables import read_capture
from audio_doc_blocks import render_capture_figures
from audio.mic.eq_probe_sweep import CAPTURE
from audio.mic.mic_eq_probe_tables import probe_data, response, hpf_reference, CURVE_POINTS
from docs.plot_style import FIGURE_SIZE, SLOTS, subplots, save_themed_plot


def render(capture, output):
    _, records, _ = read_capture(capture)
    data = probe_data(records)
    for owner in ('adc', 'dac', 'hpf'):
        fig, axis = subplots(1, 1, FIGURE_SIZE)
        for index, ((kind, case, path), points) in enumerate(
                (item for item in data.items() if item[0][0] == owner and item[0][1] != 'bypass')):
            frequencies = sorted(points)
            color = SLOTS[index % len(SLOTS)]
            label = case + (' / ' + path if owner == 'dac' else '')
            axis.plot(frequencies, [points[hz] for hz in frequencies], 'o-' if owner == 'hpf' else 'o',
                      color=color, label=label)
            if owner != 'hpf':
                curve = np.geomspace(min(frequencies), max(frequencies), CURVE_POINTS)
                axis.plot(curve, response(owner, case, curve), color=color, alpha=0.6)
        axis.set(xscale='log', xlabel='Frequency (Hz)', ylabel='Response relative to ' + (f'code {hpf_reference(data)}' if owner == 'hpf' else 'bypass') + ' (dB)')
        save_themed_plot(fig, axis, output / ('mic-eq-probe-' + owner + '.png'), owner.upper() + ' response')


def main():
    render_capture_figures(__doc__, render, CAPTURE, ROOT / "docs/images/audio")


if __name__ == '__main__':
    main()
