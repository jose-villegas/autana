"""Board shake against audible loudness from the committed low-tone sweep.

Tables: python launcher/tools/audio/audio_doc_tables.py --write
Figures: python launcher/tools/audio/audio_buzz_doc.py --out DIR
"""
import math
from pathlib import Path
from statistics import median

from audio_doc_tables import ROOT, read_capture
from audio_doc_blocks import caption, details, figures_main, line_chart
from audio_buzz_sweep import CAPTURE  # noqa: F401 (audio_doc_tables reads it from here)
from docs.plot_style import SLOTS, picture, save_themed_plot
from doc_tables import markdown

DOCUMENT = Path("docs/audio/Low-Frequency-Buzz.md")
REGENERATE = "python launcher/tools/audio/audio_doc_tables.py --write"
POWER_DB, AMPLITUDE_DB = 10, 20
# IEC 61672-1 A-weighting: pole frequencies (Hz) and the offset that makes 1 kHz read 0 dB.
A_POLES_HZ = (20.598997, 107.65265, 737.86223, 12194.217)
A_OFFSET_DB = 2.0
# THD sums harmonics 2..5; a small cone at low frequency puts most of that in the second.
HARMONIC = 2
# Shake counts as the tone's only this far above the silent board's own reading (sensor noise).
DETECT_RATIO = 2


def a_weighting_db(hz):
    f1, f2, f3, f4 = A_POLES_HZ
    squared = hz * hz
    gain = (f4 * f4 * squared * squared) / (
        (squared + f1 * f1) * math.sqrt((squared + f2 * f2) * (squared + f3 * f3)) * (squared + f4 * f4))
    return AMPLITUDE_DB * math.log10(gain) + A_OFFSET_DB


def audible_db(hz, tone_db, thd_db):
    """A-weighted mic level of the fundamental plus its harmonics, taken as all at HARMONIC x hz."""
    power = 10 ** ((tone_db + a_weighting_db(hz)) / POWER_DB)
    if thd_db is not None:
        power += 10 ** ((tone_db + thd_db + a_weighting_db(HARMONIC * hz)) / POWER_DB)
    return POWER_DB * math.log10(power)


def buzz_data(path):
    _, records, _ = read_capture(path)
    groups = {}
    for command, reply in records:
        if command[:1] != ["tone"]:
            continue
        hz, _, volume = map(int, command[1:4])
        if reply["hz"] != hz or reply.get("shake_rms_mg") is None:
            raise ValueError(f"tone {hz} Hz has no shake reading")
        groups.setdefault((hz, volume), []).append(reply)
    if not groups:
        raise ValueError("buzz capture has no tones")
    points = {}
    for (hz, volume), replies in groups.items():
        tone_db = median(reply["tone_db"] for reply in replies)
        thd = [reply["thd_db"] for reply in replies if reply["thd_db"] is not None]
        thd_db = median(thd) if thd else None
        shake = median(reply["shake_rms_mg"] for reply in replies)
        floor = median(reply["silence_shake_rms_mg"] for reply in replies)
        audible = audible_db(hz, tone_db, thd_db)
        # RMS adds in power, so the tone's own shake is what is left after the floor's.
        excess = math.sqrt(shake * shake - floor * floor) if shake >= DETECT_RATIO * floor else None
        points[(hz, volume)] = dict(
            runs=len(replies), shake=shake, peak=max(reply["shake_peak_mg"] for reply in replies), floor=floor,
            tone_db=tone_db, silence_db=median(reply["silence_db"] for reply in replies), thd_db=thd_db,
            audible=audible, score=None if excess is None else AMPLITUDE_DB * math.log10(excess) - audible)
    return points


def volumes_of(points):
    return sorted({volume for _, volume in points})


def reference_volume(points):
    """The loudest volume swept at every frequency: the one curve with no gaps."""
    frequencies = {hz for hz, _ in points}
    return max(volume for volume in volumes_of(points)
               if all((hz, volume) in points for hz in frequencies))


def render(path):
    points = buzz_data(path)
    reference = reference_volume(points)
    felt = [key for key in points if points[key]["score"] is not None]
    resonance = max((key for key in points if key[1] == reference), key=lambda key: points[key]["shake"])
    findings = [(f"Most shake at {reference}%", resonance)]
    if felt:
        findings.insert(0, ("Most shake per audible dB", max(felt, key=lambda key: points[key]["score"])))
    summary = (f"Shake counts where it reaches {DETECT_RATIO}x the silent board's reading"
               + ("" if felt else "; no tone did") + ".\n\n"
               + markdown(["Finding", "Frequency (Hz)", "Volume (%)", "Shake (mg RMS)", "Audible (dBFS, A)"], [
                   (label, hz, volume, f"{points[(hz, volume)]['shake']:.2f}", f"{points[(hz, volume)]['audible']:.1f}")
                   for label, (hz, volume) in findings]))
    rows = [(hz, volume, point["runs"], f"{point['shake']:.2f}", f"{point['peak']:.2f}", f"{point['floor']:.2f}",
             f"{point['tone_db']:.1f}", f"{point['silence_db']:.1f}",
             "n/a" if point["thd_db"] is None else f"{point['thd_db']:.1f}", f"{point['audible']:.1f}",
             "below floor" if point["score"] is None else f"{point['score']:.1f}")
            for (hz, volume), point in sorted(points.items())]
    table = markdown(["Hz", "Volume (%)", "Runs", "Shake (mg RMS)", "Worst peak (mg)", "Silence (mg RMS)",
                      "Tone (dBFS)", "Silence (dBFS)", "THD (dB)", "Audible (dBFS, A)", "Score (dB)"], rows)
    return (caption(path, REGENERATE, "replies") + summary + "\n"
            + picture("audio/buzz-shake", "Board shake against tone frequency, per volume")
            + picture("audio/buzz-score", "Shake per audible decibel against tone frequency, per volume")
            + details(table))


def update(capture, document, check=False):
    from generated_blocks import replace_block
    body = render(capture) if capture.exists() else "No capture yet.\n"
    return replace_block(document, "audio-buzz", body, check=check)


def figures(path, output):
    points = buzz_data(path)
    reference = reference_volume(points)
    for name, field, label, title in (
            ("buzz-shake", "shake", "Board shake (mg RMS)", "Board shake against tone frequency"),
            ("buzz-score", "score", "Shake per audible dB (dB re 1 mg)", "Shake per audible decibel")):
        series = [(f"{volume}%", sorted((hz, point[field]) for (hz, gain), point in points.items()
                                        if gain == volume and point[field] is not None), "o-", color)
                  for volume, color in zip(volumes_of(points), SLOTS)]
        if field == "shake":
            series.append(("Silence", sorted((hz, point["floor"]) for (hz, gain), point in points.items()
                                             if gain == reference), "--", SLOTS[-1]))
        fig, axis = line_chart(series, xlabel="Tone frequency (Hz)", ylabel=label)
        if field == "shake":
            axis.set_ylim(bottom=0)
        save_themed_plot(fig, axis, output / f"{name}.png", title)


if __name__ == "__main__":
    figures_main(__doc__, ROOT / CAPTURE, figures)
