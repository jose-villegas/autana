"""Sound roots, stored samples and pack integration."""

import math
import pathlib
import struct
import sys
import tempfile
import unittest
import wave

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from audio import sound_asset as sound
from audio import synth_wav
from asset.asset_pack import parse_pack
from r3d import build_pack

# A low-amplitude smooth signal includes the initial index-zero adaptation.
MAX_ERROR = 300
RMS_ERROR = 50


def signal():
    return [round(1200 * math.sin(i * 0.04)) for i in range(sound.DEFAULT_BLOCK_FRAMES * 2 + 37)]


class SoundTests(unittest.TestCase):
    def test_round_trips(self):
        samples = signal()
        for fmt in ("pcm16", "adpcm"):
            loop = [sound.DEFAULT_BLOCK_FRAMES, len(samples)]
            decoded_fmt, actual, start, end, block = sound.decode(sound.encode(samples, fmt, loop))
            self.assertEqual(decoded_fmt, fmt)
            self.assertEqual((start, end), tuple(loop))
            self.assertEqual(block, sound.DEFAULT_BLOCK_FRAMES if fmt == "adpcm" else 0)
            if fmt == "pcm16":
                self.assertEqual(actual, samples)
            else:
                errors = [a - b for a, b in zip(samples, actual)]
                self.assertLessEqual(max(map(abs, errors)), MAX_ERROR)
                self.assertLessEqual(math.sqrt(sum(e * e for e in errors) / len(errors)), RMS_ERROR)

    def test_frozen_header(self):
        expected = bytes.fromhex("0100000122560000020000000000000000000000000000000000000020000000")
        self.assertEqual(sound.encode([1, -2], "pcm16", None), expected + b"\x01\x00\xfe\xff")

    def test_roots_reject_bad_settings(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder) / "note.sound.toml"
            cases = ['source="x.wav"', 'source="x.wav"\nformat="bad"',
                     'source="x.wav"\nformat="pcm16"\ngain=1',
                     'source="x.wav"\nformat="pcm16"\nloop=[1]',
                     'source="x.wav"\nformat="pcm16"\nloop=[true,2]',
                     'source="x.wav"\nformat="pcm16"\nloop="bad"']
            cases += [f'source="{p}"\nformat="pcm16"' for p in
                      ("../x.wav", "a/x.wav", "C:x.wav", "x.png", "a\\\\x.wav", "/x.wav")]
            for text in cases:
                root.write_text(text)
                with self.subTest(text=text), self.assertRaises(sound.SoundError):
                    sound.bake(root)

    def test_loop_and_sample_rejections(self):
        for loop in ([0, 0], [2, 1], [-1, 2], [0, 4], [False, 2], [0], "bad"):
            with self.subTest(loop=loop), self.assertRaises(sound.SoundError):
                sound.encode([0, 1, 2], "pcm16", loop)
        with self.assertRaisesRegex(sound.SoundError, f"0.*{sound.DEFAULT_BLOCK_FRAMES}"):
            sound.encode(signal(), "adpcm", [1, 10])
        for samples in ([], [32768], [-32769], [1.5]):
            with self.assertRaises(sound.SoundError):
                sound.encode(samples, "pcm16", None)

    def test_reader_rejections(self):
        entry = sound.encode(signal(), "adpcm", None)
        mutations = [(sound.AT_CHANNELS, b"\x02"), (sound.AT_FORMAT, b"\x02"),
                     (sound.AT_RESERVED16, b"\x01"), (sound.AT_RESERVED32, b"\x01"),
                     (sound.AT_SAMPLE_RATE, b"\x00"), (sound.AT_DATA, b"\x21"),
                     (sound.AT_FRAMES, bytes(4)), (sound.AT_BLOCK_FRAMES, bytes(2)),
                     (sound.HEADER_SIZE + sound.ADPCM_AT_STEP_INDEX, bytes([sound.STEP_INDEX_MAX + 1])),
                     (sound.HEADER_SIZE + sound.ADPCM_AT_RESERVED, b"\x01")]
        for offset, value in mutations:
            bad = bytearray(entry)
            bad[offset:offset + len(value)] = value
            with self.subTest(offset=offset), self.assertRaises(sound.SoundError):
                sound.decode(bad)
        with self.assertRaises(sound.SoundError):
            sound.decode(entry[:-1])

    def test_mixdown_integer_widths(self):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / "source.wav"
            for width in (1, 2, 3, 4):
                scale = 1 << (width * 8 - 2)
                values = [scale, 0, -scale, 0]
                raw = b"".join((v + 128).to_bytes(1, "little") if width == 1
                               else v.to_bytes(width, "little", signed=True) for v in values)
                with wave.open(str(path), "wb") as wav:
                    wav.setparams((2, width, sound.SAMPLE_RATE_HZ, 0, "NONE", ""))
                    wav.writeframes(raw)
                self.assertEqual(sound.read_wav(path), [8192, -8192])

    def test_resampling_frequency_length_and_alias_rejection(self):
        source_rate, frequency, duration = 44100, 1000, 0.1
        samples = [12000 * math.sin(2 * math.pi * frequency * i / source_rate)
                   for i in range(round(source_rate * duration))]
        actual = sound.resample(samples, source_rate)
        self.assertEqual(len(actual), round(sound.SAMPLE_RATE_HZ * duration))
        crossings = sum(a <= 0 < b for a, b in zip(actual[100:-100], actual[101:-99]))
        measured = crossings * sound.SAMPLE_RATE_HZ / (len(actual) - 200)
        self.assertAlmostEqual(measured, frequency, delta=15)
        high = [12000 * math.sin(2 * math.pi * 16000 * i / source_rate) for i in range(len(samples))]
        filtered = sound.resample(high, source_rate)[100:-100]
        self.assertLess(math.sqrt(sum(v*v for v in filtered) / len(filtered)), 100)

    def test_synthesis_and_folder_pack(self):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder)
            wav = path / "note.wav"
            args = ["--frequency", "900", "--duration-ms", "200", "--decay", "25", "--peak", "0.2", "-o", str(wav)]
            synth_wav.main(args)
            first = wav.read_bytes()
            synth_wav.main(args)
            self.assertEqual(wav.read_bytes(), first)
            (path / "note.sound.toml").write_text('source="note.wav"\nformat="adpcm"')
            (path / "group.pack.toml").write_text("")
            entries = parse_pack(build_pack.pack_bytes([path])["group"])
            self.assertEqual(entries["note"][0], sound.TYPE)
            self.assertEqual(len(sound.decode(entries["note"][1])[1]), round(sound.SAMPLE_RATE_HZ * 0.2))


if __name__ == "__main__":
    unittest.main()
