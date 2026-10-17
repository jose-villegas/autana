"""Write the multi-block PCM16 and ADPCM reference pack for the C suite."""

import argparse
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from audio import sound_asset as sound
from asset.asset_pack import build_pack


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", type=pathlib.Path, required=True)
    out = parser.parse_args().o
    samples = [round(1200 * math.sin(i * 0.04)) for i in range(sound.DEFAULT_BLOCK_FRAMES * 2 + 37)]
    loop = [sound.DEFAULT_BLOCK_FRAMES, len(samples)]
    out.mkdir(parents=True, exist_ok=True)
    (out / "sound_probe.apak").write_bytes(build_pack([
        (fmt, sound.TYPE, sound.encode(samples, fmt, loop)) for fmt in ("pcm16", "adpcm")]))


if __name__ == "__main__":
    main()
