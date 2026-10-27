"""sound_mix_image bakes its variant on this host and appends the made row to the scratch lock."""
import subprocess
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sound_mix_image

KEY = "a" * 64


class LockVariantTests(unittest.TestCase):
    def made(self, stdout, row):
        done = subprocess.CompletedProcess([], 0, stdout=stdout, stderr="")
        return (mock.patch.object(sound_mix_image.subprocess, "run", return_value=done),
                mock.patch.object(sound_mix_image.produce, "made", return_value=row))

    def test_appends_the_made_row_to_the_scratch_lock(self):
        row = {"output": "tap_pcm16.sond", "source": "launcher/main/engine/sounds/tap_pcm16.sound.toml", "key": KEY,
               "sha256": "b" * 64, "size": 9734, "suffix": ".sond", "host": "Windows AMD64"}
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            lock = tree / sound_mix_image.LOCK
            lock.parent.mkdir(parents=True)
            lock.write_text('[[bake]]\noutput = "tap.sond"\nkey = "c"\n', encoding="utf-8")
            run, made = self.made(f"made tap_pcm16.sond  key {KEY}  sha256 {'b' * 64}\n", row)
            with run, made:
                sound_mix_image.lock_variant(tree, "tap_pcm16.sond", cache=tree)
            rows = tomllib.loads(lock.read_text(encoding="utf-8"))["bake"]
        self.assertEqual([entry["output"] for entry in rows], ["tap.sond", "tap_pcm16.sond"])
        self.assertEqual(rows[1], {name: row[name] for name in sound_mix_image.ROW_FIELDS if name in row})

    def test_fails_when_the_bake_made_nothing_for_the_variant(self):
        with tempfile.TemporaryDirectory() as directory:
            run, made = self.made(f"made other.sond  key {KEY}  sha256 {'b' * 64}\n", None)
            with run, made, self.assertRaisesRegex(ValueError, "made no tap_pcm16.sond"):
                sound_mix_image.lock_variant(Path(directory), "tap_pcm16.sond", cache=Path(directory))


if __name__ == "__main__":
    unittest.main()
