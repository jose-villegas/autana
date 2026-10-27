"""Plot measured EQ responses over designs and the HPF code map in both themes."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'launcher/tools/audio'))
import numpy as np
from audio_doc_tables import read_capture
from audio_doc_blocks import figures_main
from audio.mic.eq_probe_sweep import CAPTURE
from audio.mic.mic_eq_probe_tables import (
    probe_data, response, hpf_reference, CURVE_POINTS, slot_case, slot_results, order_case, decided_layout,
)
from audio.mic.eq_design import AMPLITUDE_DB, ORDER_HZ, eq_order_response
from docs.plot_style import FIGURE_SIZE, SLOTS, subplots, save_themed_plot


def render(capture, output):
    _, records, _ = read_capture(capture)
    data = probe_data(records)
    if any(slot_case(case) for owner, case, path in data):
        render_slots(data, output)
        return
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


def render_slots(data, output):
    for owner in ('adc', 'dac'):
        points, roles = slot_results(data, owner)
        fig, axis = subplots(1, 1, FIGURE_SIZE)
        for index, (reg, values) in enumerate(points.items()):
            for value, tones in values.items():
                frequencies = sorted(tones)
                axis.plot(frequencies, [tones[hz] for hz in frequencies], 'o-',
                          color=SLOTS[index % len(SLOTS)],
                          label=f'0x{reg:02X} / {value:g}: {roles[reg]}')
        axis.set(xscale='log', xlabel='Frequency (Hz)', ylabel='Response relative to bypass (dB)')
        save_themed_plot(fig, axis, output / ('mic-eq-slots-' + owner + '.png'), owner.upper() + ' isolated slots')
        fig, axis = subplots(1, 1, FIGURE_SIZE)
        scale = []
        for (kind, case, path), tones in data.items():
            match = slot_case(case)
            if kind == owner and match and match[1] == 'scale':
                scale.append((float(match[3]), next(iter(tones.values()))))
        if scale:
            values, levels = zip(*sorted(scale))
            axis.plot(values, levels, 'o-', label='Measured')
            axis.plot(values, AMPLITUDE_DB * np.log10(values), label='Q2.28 reference')
        axis.set(xscale='log', xlabel='Encoded value (Q2.28)', ylabel='Response relative to bypass (dB)')
        save_themed_plot(fig, axis, output / ('mic-eq-scale-' + owner + '.png'), owner.upper() + ' scale')
        render_slot_checks(data, owner, output)


def render_slot_checks(data, owner, output):
    layout = decided_layout(data, owner)
    for category in ('order', 'designs'):
        selected = [(case, points) for (kind, case, path), points in data.items() if kind == owner
                    and (bool(order_case(case)) if category == 'order' else case.startswith('rerun-'))]
        if not selected:
            continue
        fig, axis = subplots(1, 1, FIGURE_SIZE)
        for index, (case, points) in enumerate(selected):
            match = order_case(case)
            reference = points[ORDER_HZ[0]] if match and match[1] == 'delay' else 0
            frequencies = sorted(points)
            color = SLOTS[index % len(SLOTS)]
            axis.plot(frequencies, [points[hz] - reference for hz in frequencies], 'o-', color=color, label=case)
            if match and layout:
                expected = eq_order_response(*layout[int(match[2], 16)])
                axis.plot(frequencies, [expected[hz] for hz in frequencies], '--', color=color)
            elif category == 'designs':
                curve = np.geomspace(min(frequencies), max(frequencies), CURVE_POINTS)
                axis.plot(curve, response(owner, case, curve), color=color, alpha=0.6)
        axis.set(xscale='log', xlabel='Frequency (Hz)', ylabel='Relative response (dB)')
        save_themed_plot(fig, axis, output / f'mic-eq-{category}-{owner}.png', owner.upper() + ' ' + category)


def main():
    figures_main(__doc__, CAPTURE, render)


if __name__ == '__main__':
    main()
