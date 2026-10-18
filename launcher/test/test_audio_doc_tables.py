"""Capture-derived audio tables and their regeneration contract."""
import importlib.util
import tempfile
import argparse
import json
import os
import sys
from unittest import mock
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("audio_doc_tables", ROOT / "launcher/tools/audio/audio_doc_tables.py")
audio = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audio)


class AudioTablesTest(unittest.TestCase):
    def test_capture_tables(self):
        tables = audio.render(ROOT / audio.CAPTURE)
        self.assertEqual(set(tables), {"audio-ram", "audio-frequency", "audio-volume", "audio-duration"})
        self.assertIn("56", tables["audio-ram"])
        self.assertIn("3096", tables["audio-ram"])
        self.assertIn("3040", tables["audio-ram"])
        self.assertIn("63488", tables["audio-ram"])
        self.assertIn("500 | -14.9", tables["audio-duration"])
        self.assertIn("100 | -14.1", tables["audio-volume"])
        for body in tables.values():
            self.assertIn("A308", body)
            self.assertIn("2521fd82ef55-dev", body)

    def test_capture_header_comments_are_optional(self):
        capture = ROOT / audio.CAPTURE
        lines = capture.read_text().splitlines()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.txt"
            path.write_text("\n".join([lines[0], *(line for line in lines[1:] if not line.startswith("#"))]))
            self.assertEqual(audio.render(path), audio.render(capture))

    def test_changed_capture_changes_tables(self):
        capture = (ROOT / audio.CAPTURE).read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.txt"
            path.write_text(capture.replace('"tone_db": -31.2', '"tone_db": -32.7'))
            self.assertIn("-32.7", audio.render(path)["audio-frequency"])

    def test_build_mismatch_is_rejected(self):
        capture = (ROOT / audio.CAPTURE).read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.txt"
            path.write_text(capture.replace('2521fd82ef55-dev', 'other-dev', 1))
            with self.assertRaisesRegex(ValueError, "build"):
                audio.render(path)

    def test_check_detects_stale_doc_without_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Audio.md"
            path.write_bytes((ROOT / audio.DOCUMENT).read_bytes())
            self.assertFalse(audio.update(ROOT / audio.CAPTURE, path, check=True))
            original = path.read_bytes().replace(b"-31.2", b"-32.7")
            path.write_bytes(original)
            self.assertTrue(audio.update(ROOT / audio.CAPTURE, path, check=True))
            self.assertEqual(path.read_bytes(), original)
            audio.update(ROOT / audio.CAPTURE, path, check=False)
            self.assertFalse(audio.update(ROOT / audio.CAPTURE, path, check=True))


class SoundMixTablesTest(unittest.TestCase):
    def fixture(self, directory):
        path = Path(directory) / "sound_mix_capture_test.txt"
        path.write_text(
            '# autana sound-mix capture on A308, 2026-10-18, records\n'
            'buildid\t{"build_id":"test-dev"}\ncapacity\t8\nvoices\t0,1\n'
            'sound tap\tadpcm\n'
            'point tap 0\tsound_mix 0.001/0.002 ms/block n=1\n'
            'point tap 1\tsound_mix 0.050/0.091 ms/block n=2\n'
            'point tap 1\tsound_mix 0.080/0.090 ms/block n=1\n'
            'stats tap 1\tSOUND ram voices=100 queue=200 stack=300 mixer=400\n'
            'stats tap 1\tSOUND stack_min_free=50 margin=20\n'
            'buildid\t{"build_id":"test-dev"}\n', encoding="utf-8")
        return path

    def test_weighted_windows_and_load(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory)
            body = audio.render_mix(path)
            self.assertIn("60.00 | 91.00 | 1.03 | 1.57 | 3", body)
            self.assertIn("5804.99 ?s", body)
            self.assertIn("capacity **8 voices**", body)
            self.assertIn("50 | 20 | 300 | 100 | 200 | 400", body)
            tree = Path(directory)
            for owner in ["sound/sound_engine.h", "audio/audio_codec.h", "audio/audio_out.h"]:
                target = tree / "launcher/main" / owner
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((ROOT / "launcher/main" / owner).read_bytes())
            header = tree / "launcher/main/audio/audio_codec.h"
            header.write_text(header.read_text().replace("128u", "256u"))
            self.assertAlmostEqual(audio.mix_period(tree), 2 * audio.mix_period(ROOT))

    def test_rejects_invalid_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory)
            original = path.read_text()
            for old, new, error in [('test-dev', 'other-dev', 'build'),
                                    ('voices\t0,1', 'voices\t0,1,4', 'point'),
                                    ('capacity\t8', 'capacity\t0', 'capacity')]:
                path.write_text(original.replace(old, new, 1))
                with self.assertRaisesRegex(ValueError, error):
                    audio.render_mix(path)

    def test_mix_check_and_missing_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            self.fixture(directory)
            doc = Path(directory) / "Mixer.md"
            doc.write_text('<!-- generated: sound-mix-cost -->\n<!-- /generated: sound-mix-cost -->')
            args = (ROOT / audio.CAPTURE, ROOT / audio.DOCUMENT)
            self.assertTrue(audio.update(*args, check=True, mix_captures=directory, mix_document=doc))
            audio.update(*args, mix_captures=directory, mix_document=doc)
            self.assertFalse(audio.update(*args, check=True, mix_captures=directory, mix_document=doc))
            with self.assertRaisesRegex(ValueError, "no sound-mix"):
                audio.update(*args, mix_captures=Path(directory) / "absent", mix_document=doc)


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f"launcher/tools/audio/{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SoundMixToolsTest(unittest.TestCase):
    def test_image_only_edits_measurement_files(self):
        image = load_tool("sound_mix_image")
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            header = tree / image.HEADER
            header.parent.mkdir(parents=True)
            (tree / image.ASSET).parent.mkdir(parents=True)
            original = b"#define OTHER 9\r\n#define SOUND_VOICE_COUNT 8\r\n"
            header.write_bytes(original)
            with mock.patch.object(image.subprocess, "run", return_value=argparse.Namespace(stdout="")):
                image.prepare(tree, 32)
                self.assertEqual(header.read_bytes(), original.replace(b"COUNT 8", b"COUNT 32"))
                self.assertEqual((tree / image.ASSET).read_text(), 'source = "tap.wav"\nformat = "pcm16"\n')
                self.assertEqual({p.relative_to(tree) for p in tree.rglob("*") if p.is_file()},
                                 {image.ASSET, image.HEADER})
                image.prepare(tree)
                self.assertIn(b"COUNT 32", header.read_bytes())
                for invalid in [b"#define OTHER 8\n", original + original]:
                    header.write_bytes(invalid)
                    with self.assertRaisesRegex(ValueError, "exactly one"):
                        image.prepare(tree, 16)
            with mock.patch.object(image.subprocess, "run",
                                   return_value=argparse.Namespace(stdout=" M unrelated.txt\n")):
                with self.assertRaisesRegex(ValueError, "outside"):
                    image.prepare(tree, 16)
        with self.assertRaisesRegex(ValueError, "checkout"):
            image.prepare(ROOT)

    def test_capture_fake_autana_sequence(self):
        capture = load_tool("sound_mix_capture")
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            sounds = tree / "launcher/main/engine/sounds"
            sounds.mkdir(parents=True)
            for sound, fmt in [("tap", "adpcm"), ("tap_pcm16", "pcm16")]:
                (sounds / f"{sound}.sound.toml").write_text(f'format = "{fmt}"\n')
            for owner in ["shell/shell.c", "audio/audio_out_device.h", "sound/sound_mixer.h"]:
                target = tree / "launcher/main" / owner
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((ROOT / "launcher/main" / owner).read_bytes())
            stub = tree / "fake.py"
            log = tree / "commands.jsonl"
            stub.write_text(
                'import json,sys\nfrom pathlib import Path\n'
                f'log=Path({str(log)!r})\n'
                'with log.open("a") as f: f.write(json.dumps(sys.argv[1:])+"\\n")\n'
                'args=sys.argv[1:]\n'
                'if args==["buildid","--json"]: print(json.dumps({"build_id":"fake-dev"}))\n'
                'elif args==["sound","stats"]: print("SOUND slot=0\\nSOUND slot=1\\nSOUND ram voices=10 queue=20 stack=30 mixer=40\\nSOUND stack_min_free=5 margin=2")\n'
                'elif args[:1]==["monitor"]: print("noise\\nsound_mix 0.052/0.091 ms/block n=2")\n')
            wrapper = tree / ("autana.cmd" if os.name == "nt" else "autana")
            wrapper.write_text(f'@"{sys.executable}" "{stub}" %*\n' if os.name == "nt" else
                               f'#!/bin/sh\nexec "{sys.executable}" "{stub}" "$@"\n')
            wrapper.chmod(0o755)
            args = argparse.Namespace(autana=str(wrapper), tree=tree, out=tree / "capture.txt",
                                      board="TEST", sounds="tap,tap_pcm16", voices="0,1,2", seconds=1)
            waits = []
            capture.capture(args, sleep=waits.append)
            commands = [json.loads(line) for line in log.read_text().splitlines()]
            expected = [["sound", "stats"], ["buildid", "--json"]]
            expected += [["sound", "stop", "all"], ["monitor", "1", "--stream"]]
            for sound in ["tap", "tap_pcm16"]:
                for count in [1, 2]:
                    expected += [["sound", "stop", "all"]] + [["sound", "play", sound, "loop"]] * count
                    expected += [["monitor", "1", "--stream"]]
            expected += [["sound", "stats"], ["sound", "stop", "all"], ["buildid", "--json"]]
            self.assertEqual(commands, expected)
            self.assertEqual(waits, [5.0, 3.25, 3.25, 3.25, 3.25])
            body = audio.render_mix(args.out)
            self.assertIn("fake-dev", body)
            args.voices = "3"
            with self.assertRaisesRegex(ValueError, "capacity"):
                capture.capture(args, sleep=waits.append)
            stub.write_text("import sys\nsys.exit(7)\n")
            args.voices = "1"
            with self.assertRaises(capture.subprocess.CalledProcessError):
                capture.capture(args, sleep=waits.append)
            args.autana = str(tree / "absent")
            with self.assertRaises(OSError):
                capture.capture(args, sleep=waits.append)


if __name__ == "__main__":
    unittest.main()
