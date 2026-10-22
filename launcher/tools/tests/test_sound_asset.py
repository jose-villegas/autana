"""Sound roots, stored samples and pack integration."""

import ast
import re
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
from audio import synth_wav, sound_import
from bake import bake, produce
from unittest.mock import patch
from asset.asset_pack import parse_pack
from r3d import build_pack
from sound_probe import signal

# A low-amplitude smooth signal includes the initial index-zero adaptation.
MAX_ERROR = 300
RMS_ERROR = 50


class SoundTests(unittest.TestCase):
    @staticmethod
    def write_wav(path, samples, rate=sound.SAMPLE_RATE_HZ, width=sound.PCM_BYTES):
        with wave.open(str(path), "wb") as wav:
            wav.setparams((sound.CHANNELS, width, rate, 0, "NONE", ""))
            wav.writeframes(b"".join(v.to_bytes(width, "little", signed=True) for v in samples))

    def test_ffmpeg_vectors_and_frozen_encoder(self):
        text = (TOOLS.parent / "test/fixtures/audio_adpcm_reference.h").read_text()
        def vector(name):
            values = re.search(r"\b" + name + r"\[\] = \{([^}]+)\}", text)[1]
            return ast.literal_eval("[" + values + "]")
        for prefix in ("", "SATURATION_"):
            with self.subTest(prefix=prefix):
                block, expected = bytes(vector(prefix + "BLOCK")), vector(prefix + "EXPECTED")
                entry = bytearray(sound.encode([0] * len(expected), "adpcm"))
                struct.pack_into("<H", entry, sound.AT_BLOCK_FRAMES, len(expected))
                entry = entry[:sound.HEADER_SIZE] + block
                self.assertEqual(sound.decode(entry)[1], expected)
        self.assertEqual(sound.encode_adpcm([0, 1000, -1000, 2000, -2000], 5), bytes.fromhex("00000000f7f7"))

    def test_frozen_adpcm_loop_header(self):
        entry = sound.encode([123] * 600, "adpcm", [505, 600])
        expected = bytes.fromhex("010001012256000058020000f901000058020000f901000000000000200000007b000000")
        self.assertEqual(entry[:sound.HEADER_SIZE + sound.BLOCK_HEADER_BYTES], expected)

    def test_decode_block_and_loop_rules(self):
        cases = [
            ("pcm16", sound.AT_BLOCK_FRAMES, "H", 1, "block_frames"),
            ("adpcm", sound.AT_BLOCK_FRAMES, "H", sound.DEFAULT_BLOCK_FRAMES - 1, "block_frames"),
            ("adpcm", sound.AT_LOOP_START, "I", 1, "one-shot"),
            ("adpcm", sound.AT_LOOP_END, "I", 1, "loop requires"),
            ("adpcm", sound.AT_LOOP_END, "I", len(signal()) + 1, "loop requires"),
            ("adpcm", sound.AT_LOOP_START, "I", 1, "block-aligned"),
        ]
        for fmt, at, encoding, value, reason in cases:
            loop = [sound.DEFAULT_BLOCK_FRAMES, len(signal())] if reason != "one-shot" else None
            entry = bytearray(sound.encode(signal(), fmt, loop))
            struct.pack_into("<" + encoding, entry, at, value)
            with self.subTest(at=at, value=value, reason=reason), self.assertRaisesRegex(sound.SoundError, reason):
                sound.decode(entry)
        entry = bytearray(sound.encode(signal(), "pcm16", [1, 2]))
        struct.pack_into("<I", entry, sound.AT_LOOP_END, 1)
        with self.assertRaisesRegex(sound.SoundError, "loop requires"):
            sound.decode(entry)

    def test_decode_bounds_gap_version_and_step_limit(self):
        for fmt in sound.FORMATS:
            entry = sound.encode(signal(), fmt)
            with self.subTest(fmt=fmt), self.assertRaisesRegex(sound.SoundError, "data leaves"):
                sound.decode(entry[:-1])
            bad = bytearray(entry)
            struct.pack_into("<I", bad, sound.AT_DATA, sound.AT_DATA)
            with self.assertRaisesRegex(sound.SoundError, "data leaves"):
                sound.decode(bad)
            bad = bytearray(entry[:sound.HEADER_SIZE] + bytes(sound.ALIGNMENT) + entry[sound.HEADER_SIZE:])
            struct.pack_into("<I", bad, sound.AT_DATA, sound.HEADER_SIZE + sound.ALIGNMENT)
            self.assertEqual(sound.decode(bad)[1], sound.decode(entry)[1])
            bad[sound.HEADER_SIZE + sound.ALIGNMENT - 1] = 1
            with self.assertRaisesRegex(sound.SoundError, "header gap"):
                sound.decode(bad)
        entry = bytearray(sound.encode(signal(), "adpcm"))
        struct.pack_into("<H", entry, sound.AT_VERSION, sound.VERSION + 1)
        with self.assertRaisesRegex(sound.SoundError, "header"):
            sound.decode(entry)
        struct.pack_into("<H", entry, sound.AT_VERSION, sound.VERSION)
        entry[sound.HEADER_SIZE + sound.ADPCM_AT_STEP_INDEX] = sound.STEP_INDEX_MAX
        self.assertEqual(len(sound.decode(entry)[1]), len(signal()))
        entry[sound.HEADER_SIZE + sound.ADPCM_AT_STEP_INDEX] += 1
        with self.assertRaisesRegex(sound.SoundError, "block header"):
            sound.decode(entry)

    def test_mono_and_full_scale_32_bit_wav(self):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / "mono.wav"
            self.write_wav(path, [sound.SAMPLE_MIN, -1, 0, 1, sound.SAMPLE_MAX])
            self.assertEqual(sound_import.read_source(path), [sound.SAMPLE_MIN, -1, 0, 1, sound.SAMPLE_MAX])
            self.write_wav(path, [-(1 << 31), (1 << 31) - 1], width=4)
            self.assertEqual(sound_import.read_source(path), [sound.SAMPLE_MIN, sound.SAMPLE_MAX])

    def test_reject_invalid_and_empty_source(self):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / "bad.wav"
            self.write_wav(path, [1, -1])
            path.write_bytes(b"invalid sound")
            with self.assertRaisesRegex(sound.SoundError, "cannot decode"):
                sound_import.read_source(path)
            self.write_wav(path, [])
            with self.assertRaisesRegex(sound.SoundError, "empty"):
                sound_import.read_source(path)

    def test_resample_up_and_noninteger_ratio(self):
        frequency, duration = 1000, 0.2
        for rate in (11025, 48000):
            samples = [12000 * math.sin(2 * math.pi * frequency * i / rate)
                       for i in range(round(rate * duration))]
            actual = sound_import.resample(samples, rate)
            with self.subTest(rate=rate):
                self.assertEqual(len(actual), round(sound.SAMPLE_RATE_HZ * duration))
                inner = actual[100:-100]
                crossings = sum(a <= 0 < b for a, b in zip(inner, inner[1:]))
                measured = crossings * sound.SAMPLE_RATE_HZ / (len(inner) - 1)
                self.assertAlmostEqual(measured, frequency, delta=10)

    def test_toml_loop_uses_resampled_frames(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder) / "note.sound.toml"
            self.write_wav(root.with_suffix(".wav"), [0] * 100, rate=11025)
            root.write_text('source="note.sound.wav"\nformat="pcm16"\nloop=[10,200]')
            self.assertEqual(sound.decode(sound_import.bake(root))[2:4], (10, 200))
            root.write_text('source="note.sound.wav"\nformat="pcm16"\nloop=[10,201]')
            with self.assertRaisesRegex(sound.SoundError, "loop requires"):
                sound_import.bake(root)

    def test_engine_tap_duration(self):
        folder = TOOLS.parent / "main/engine/sounds"
        entries = {"tap": (sound.TYPE, sound_import.bake(folder / "tap.sound.toml"))}
        with wave.open(str(folder / "tap.wav"), "rb") as wav:
            frames = round(wav.getnframes() * sound.SAMPLE_RATE_HZ / wav.getframerate())
        self.assertEqual(len(sound.decode(entries["tap"][1])[1]), frames)

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
            self.write_wav(root.with_name("x.wav"), [1, -2, 3])
            root.write_text('source="x.wav"\nformat="pcm16"')
            self.assertEqual(sound.decode(sound_import.bake(root))[1], [1, -2, 3])
            cases = ['source="x.wav"', 'source="x.wav"\nformat="bad"',
                     'source="x.wav"\nformat="pcm16"\ngain=1',
                     'source="x.wav"\nformat="pcm16"\nloop=[1]',
                     'source="x.wav"\nformat="pcm16"\nloop=[true,2]',
                     'source="x.wav"\nformat="pcm16"\nloop="bad"']
            cases += [f'source="{p}"\nformat="pcm16"' for p in
                      ("../x.wav", "a/x.wav", "C:x.wav", "x.png", "a\\\\x.wav", "/x.wav")]
            for text in cases:
                root.write_text(text)
                with self.subTest(text=text), self.assertRaisesRegex(sound.SoundError, "allowed keys|loop|not a WAV"):
                    sound_import.bake(root)

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
                self.assertEqual(sound_import.read_source(path), [8192, -8192])

    def test_resampling_frequency_length_and_alias_rejection(self):
        source_rate, frequency, duration = 44100, 1000, 0.1
        samples = [12000 * math.sin(2 * math.pi * frequency * i / source_rate)
                   for i in range(round(source_rate * duration))]
        actual = sound_import.resample(samples, source_rate)
        self.assertEqual(len(actual), round(sound.SAMPLE_RATE_HZ * duration))
        crossings = sum(a <= 0 < b for a, b in zip(actual[100:-100], actual[101:-99]))
        measured = crossings * sound.SAMPLE_RATE_HZ / (len(actual) - 200)
        self.assertAlmostEqual(measured, frequency, delta=15)
        high = [12000 * math.sin(2 * math.pi * 16000 * i / source_rate) for i in range(len(samples))]
        filtered = sound_import.resample(high, source_rate)[100:-100]
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
            cache = path / "cache"
            found = bake.bakes([path])
            rows = {item.key: produce.produce(item, cache) for item in found}
            with patch.object(bake, "read_lock", return_value=rows):
                entries = parse_pack(build_pack.pack_bytes([path], cache=cache, offline=True)["group"])
            self.assertEqual(entries["note"], (sound.TYPE, sound_import.bake(path / "note.sound.toml")))
            self.assertEqual(len(sound.decode(entries["note"][1])[1]), round(sound.SAMPLE_RATE_HZ * 0.2))


if __name__ == "__main__":
    unittest.main()
