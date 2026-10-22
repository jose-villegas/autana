"""Sound import fidelity and cache isolation."""
import hashlib
import math
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import soundfile as sf

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from audio import sound_asset as sound, sound_import
from bake import bake, produce
from r3d import build_pack

TAP_SHA256 = "2bbe8de22ad6f0b92b93fced1a779933609df85eacac132de553e8b2e51fa2cf"


def reference(samples, rate):
    cutoff = min(1.0, sound.SAMPLE_RATE_HZ / rate)
    radius = sound_import.SINC_RADIUS / cutoff
    result = []
    for i in range(round(len(samples) * sound.SAMPLE_RATE_HZ / rate)):
        position = i * rate / sound.SAMPLE_RATE_HZ
        total = weights = 0.0
        for j in range(max(0, math.ceil(position - radius)), min(len(samples), math.floor(position + radius) + 1)):
            distance = j - position
            phase = distance * cutoff
            sinc = math.sin(math.pi * phase) / (math.pi * phase) if phase else 1.0
            weight = sinc * (1 + math.cos(math.pi * distance / radius)) / 2
            total += samples[j] * weight
            weights += weight
        result.append(round(total / weights))
    return result


class SoundBakeTests(unittest.TestCase):
    def test_common_formats_and_downmix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            frames = sound.SAMPLE_RATE_HZ // 4
            mono = np.sin(np.arange(frames) * 2 * np.pi * 440 / sound.SAMPLE_RATE_HZ) * 0.25
            stereo = np.column_stack((mono, np.zeros(frames)))
            for suffix, fmt, subtype in (("wav", "WAV", "PCM_16"), ("flac", "FLAC", "PCM_16"),
                                         ("ogg", "OGG", "VORBIS"), ("mp3", "MP3", "MPEG_LAYER_III")):
                with self.subTest(suffix=suffix):
                    source = root / f"note.{suffix}"
                    sf.write(source, stereo, sound.SAMPLE_RATE_HZ, format=fmt, subtype=subtype)
                    recipe = root / "note.sound.toml"
                    recipe.write_text(f'source="{source.name}"\nformat="pcm16"')
                    actual = sound.decode(sound_import.bake(recipe))[1]
                    decoded, _ = sf.read(source, always_2d=True)
                    expected = np.clip(np.rint(decoded.mean(axis=1) * (sound.SAMPLE_MAX + 1)),
                                       sound.SAMPLE_MIN, sound.SAMPLE_MAX).astype(int).tolist()
                    self.assertLessEqual(np.max(np.abs(np.asarray(actual) - expected)), 1)
                    self.assertLess(np.sqrt(np.mean((np.asarray(actual) - mono[:len(actual)] *
                                                     (sound.SAMPLE_MAX + 1) / 2) ** 2)), 150)

    def test_resample_reference(self):
        for rate in (11025, 32000, 44100, 48000):
            for seed in range(3):
                samples = np.random.default_rng(seed).integers(sound.SAMPLE_MIN, sound.SAMPLE_MAX, 151).tolist()
                with self.subTest(rate=rate, seed=seed):
                    actual = np.rint(sound_import.resample(samples, rate))
                    self.assertLessEqual(np.max(np.abs(actual - reference(samples, rate))), 1)

    def test_tap_golden(self):
        recipe = TOOLS.parent / "main/engine/sounds/tap.sound.toml"
        self.assertEqual(hashlib.sha256(sound_import.bake(recipe)).hexdigest(), TAP_SHA256)

    def test_sound_keys_and_cached_pack(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            recipe = root / "note.sound.toml"
            source = root / "note.wav"
            recipe.write_text('source="note.wav"\nformat="pcm16"')
            sf.write(source, [0.0, 0.25, -0.25], sound.SAMPLE_RATE_HZ, subtype="PCM_16")
            tools = {stage: bake.tool_digest(stage) for stage in bake.STAGES}
            unchanged = None
            def found():
                nonlocal unchanged
                with patch.object(bake, "tool_digest", side_effect=tools.__getitem__):
                    items = bake.bakes([root, build_pack.DEFAULT_SEARCH])
                other = {item.output: item.key for item in items if item.kind != "sound"}
                if unchanged is not None:
                    self.assertEqual(unchanged, other)
                unchanged = other
                return next(item for item in items if item.source == recipe)
            first = found()
            self.assertEqual(first.kind, "sound")
            cache = root / "cache"
            row = produce.produce(first, cache)
            self.assertEqual(row["sha256"], hashlib.sha256(sound_import.bake(recipe)).hexdigest())
            with patch.object(bake, "read_lock", return_value={first.key: row}):
                packed = build_pack.pack_bytes([root], cache=cache, offline=True)
            self.assertIn("note", packed)
            source.write_bytes(source.read_bytes() + b"changed")
            self.assertNotEqual(found().key, first.key)
            recipe.write_text('source="note.wav"\nformat="adpcm"')
            second = found()
            self.assertNotEqual(second.key, first.key)
            tools["sound"] = "changed"
            self.assertNotEqual(found().key, second.key)

    def test_audio_requirements_are_tracked_and_sound_counts_both_pins(self):
        self.assertIn(TOOLS / "audio/requirements.txt", bake.tracked_requirements())
        pins = [line for _, line in bake.requirement_pins("sound")]
        for owner, package in (("render", "numpy"), ("audio", "soundfile")):
            expected = next(line for line in bake.requirement_lines(TOOLS / owner / "requirements.txt")
                            if bake.package_name(line) == package)
            self.assertIn(expected, pins)

    def test_soundfile_pin_moves_only_sound_key(self):
        before = {stage: bake.tool_digest(stage) for stage in bake.STAGES}
        first = {item.output: item for item in bake.bakes([build_pack.DEFAULT_SEARCH])}
        original = bake.requirement_lines
        def bumped(path):
            return [line + ".post1" if bake.package_name(line) == "soundfile" else line for line in original(path)]
        try:
            bake.requirement_pins.cache_clear()
            bake.tool_digest.cache_clear()
            with patch.object(bake, "requirement_lines", side_effect=bumped):
                after = {stage: bake.tool_digest(stage) for stage in bake.STAGES}
                second = bake.bakes([build_pack.DEFAULT_SEARCH])
            for item in second:
                with self.subTest(output=item.output):
                    self.assertEqual(item.key == first[item.output].key, item.kind != "sound")
            self.assertNotEqual(before.pop("sound"), after.pop("sound"))
            self.assertEqual(before, after)
        finally:
            bake.requirement_pins.cache_clear()
            bake.tool_digest.cache_clear()

    def test_all_stages_ignore_code_comments_and_docstrings(self):
        before = {stage: bake.tool_digest(stage) for stage in bake.STAGES}
        original = pathlib.Path.open
        original_bytes = pathlib.Path.read_bytes
        source = TOOLS / "audio/sound_import.py"
        changed = b'# changed comment\n"""Changed docstring."""\n' + source.read_bytes()
        def edited(path, *args, **kwargs):
            if path.name == "sound_import.py" and args == ("rb",):
                import io
                return io.BytesIO(changed)
            return original(path, *args, **kwargs)
        try:
            bake.tool_digest.cache_clear()
            bake.module_facts.cache_clear()
            with patch.object(pathlib.Path, "open", edited), patch.object(
                    pathlib.Path, "read_bytes", lambda path: changed if path == source else original_bytes(path)):
                after = {stage: bake.tool_digest(stage) for stage in bake.STAGES}
            self.assertEqual(before, after)
        finally:
            bake.tool_digest.cache_clear()
            bake.module_facts.cache_clear()

    def test_pack_keys_need_no_site_packages(self):
        result = subprocess.run([sys.executable, "-S", str(TOOLS / "bake/bake.py"), "list", "--kind", "sound"],
                                capture_output=True, text=True, check=True)
        self.assertIn("tap.sond", result.stdout)

    def test_lfs_source_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "note.flac"
            source.write_bytes(b"source bytes")
            recipe = root / "note.sound.toml"
            recipe.write_text('source="note.flac"\nformat="adpcm"')
            first = bake.bakes([root])[0].key
            oid = hashlib.sha256(source.read_bytes()).hexdigest()
            source.write_text(f"version https://git-lfs.github.com/spec/v1\noid sha256:{oid}\nsize 12\n")
            self.assertEqual(bake.bakes([root])[0].key, first)


if __name__ == "__main__":
    unittest.main()
