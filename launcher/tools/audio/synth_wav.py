"""Generate a deterministic mono PCM16 damped sine WAV at the stored rate."""

import argparse
import math
import pathlib
import struct
import sys
import wave

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from audio.sound_asset import SAMPLE_RATE_HZ, PCM_BYTES, CHANNELS, SAMPLE_MAX

MILLISECONDS_PER_SECOND = 1000


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frequency", type=float, required=True)
    parser.add_argument("--duration-ms", type=float, required=True)
    parser.add_argument("--decay", type=float, required=True, help="exponential decay per second")
    parser.add_argument("--peak", type=float, required=True, help="fraction of int16 full scale")
    parser.add_argument("-o", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    if not all(math.isfinite(v) for v in (args.frequency, args.duration_ms, args.decay, args.peak)) \
            or not 0 < args.frequency < SAMPLE_RATE_HZ / 2 or args.duration_ms <= 0 \
            or args.decay < 0 or not 0 < args.peak <= 1:
        parser.error("frequency must be below Nyquist, duration positive, decay nonnegative and peak in 0..1")
    frames = round(args.duration_ms * SAMPLE_RATE_HZ / MILLISECONDS_PER_SECOND)
    if frames == 0:
        parser.error("duration must contain at least one sample")
    samples = [round(SAMPLE_MAX * args.peak * math.exp(-args.decay * i / SAMPLE_RATE_HZ)
                     * math.sin(2 * math.pi * args.frequency * i / SAMPLE_RATE_HZ)) for i in range(frames)]
    args.o.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(args.o), "wb") as wav:
        wav.setparams((CHANNELS, PCM_BYTES, SAMPLE_RATE_HZ, 0, "NONE", ""))
        wav.writeframes(struct.pack(f"<{len(samples)}h", *samples))


if __name__ == "__main__":
    main()
