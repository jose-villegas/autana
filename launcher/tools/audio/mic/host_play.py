"""Play a WAV through the default host output device, with gain in dB."""
import argparse
import math
import sounddevice as sd
import soundfile as sf
import numpy as np
DB_AMPLITUDE = 20

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wav')
    parser.add_argument('--gain', type=float, default=0)
    args = parser.parse_args(argv)
    if not math.isfinite(args.gain):
        parser.error('gain must be finite')
    x, rate = sf.read(args.wav, dtype='float32')
    x = x * 10 ** (args.gain / DB_AMPLITUDE)
    if not np.isfinite(x).all() or np.max(np.abs(x)) > 1:
        parser.error('gain would clip; lower --gain')
    sd.play(x, rate)
    sd.wait()

if __name__ == '__main__':
    main()
