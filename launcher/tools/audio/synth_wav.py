"""Generate a deterministic mono PCM16 damped sine or note-list WAV or FLAC."""

import argparse
import math
import pathlib
import struct
import sys
import wave

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from audio.sound_asset import SAMPLE_RATE_HZ, PCM_BYTES, CHANNELS, SAMPLE_MAX

MILLISECONDS_PER_SECOND = 1000
MIDI_REFERENCE_NOTE = 69
MIDI_REFERENCE_HZ = 440
MIDI_NOTES_PER_OCTAVE = 12
MIDI_NOTE_MAX = 127


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    pitch = parser.add_mutually_exclusive_group(required=True)
    pitch.add_argument("--frequency", type=float)
    pitch.add_argument("--notes", help="comma-separated MIDI notes; duration-ms is per note")
    parser.add_argument("--duration-ms", type=float, required=True)
    parser.add_argument("--decay", type=float, required=True, help="exponential decay per second")
    parser.add_argument("--peak", type=float, required=True, help="fraction of int16 full scale")
    parser.add_argument("-o", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    try:
        notes = [int(note) for note in args.notes.split(",")] if args.notes else []
    except ValueError:
        parser.error("notes must be comma-separated MIDI integers")
    if any(not 0 <= note <= MIDI_NOTE_MAX for note in notes):
        parser.error("MIDI notes must be in 0..127")
    frequencies = [MIDI_REFERENCE_HZ * 2 ** ((note - MIDI_REFERENCE_NOTE) / MIDI_NOTES_PER_OCTAVE)
                   for note in notes] if notes else [args.frequency]
    if not all(math.isfinite(v) for v in (*frequencies, args.duration_ms, args.decay, args.peak)) \
            or any(not 0 < frequency < SAMPLE_RATE_HZ / 2 for frequency in frequencies) or args.duration_ms <= 0 \
            or args.decay < 0 or not 0 < args.peak <= 1:
        parser.error("frequency must be below Nyquist, duration positive, decay nonnegative and peak in 0..1")
    frames = round(args.duration_ms * SAMPLE_RATE_HZ / MILLISECONDS_PER_SECOND)
    if frames == 0:
        parser.error("duration must contain at least one sample")
    samples = [round(SAMPLE_MAX * args.peak * math.exp(-args.decay * i / SAMPLE_RATE_HZ)
                     * math.sin(2 * math.pi * frequency * i / SAMPLE_RATE_HZ)
                     * (math.sin(math.pi * i / frames) ** 2 if notes else 1))
               for frequency in frequencies for i in range(frames)]
    args.o.parent.mkdir(parents=True, exist_ok=True)
    pcm = struct.pack(f"<{len(samples)}h", *samples)
    if args.o.suffix.lower() == ".flac":
        import soundfile
        with soundfile.SoundFile(args.o, "w", samplerate=SAMPLE_RATE_HZ, channels=CHANNELS,
                                 format="FLAC", subtype="PCM_16") as flac:
            flac.buffer_write(pcm, dtype="int16")
    else:
        with wave.open(str(args.o), "wb") as wav:
            wav.setparams((CHANNELS, PCM_BYTES, SAMPLE_RATE_HZ, 0, "NONE", ""))
            wav.writeframes(pcm)


if __name__ == "__main__":
    main()
