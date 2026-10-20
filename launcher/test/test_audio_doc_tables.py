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
        self.assertEqual(set(tables), {"audio-ram", "audio-frequency", "audio-volume", "audio-duration", "audio-loopback"})
        self.assertIn("24", tables["audio-ram"])
        self.assertIn("1448", tables["audio-ram"])
        self.assertIn("1424", tables["audio-ram"])
        self.assertIn("57344", tables["audio-ram"])
        self.assertIn("500 | -32.9", tables["audio-duration"])
        self.assertIn("100 | -22.5", tables["audio-volume"])
        for body in tables.values():
            self.assertIn("A308", body)
            self.assertIn("164511c54388-dev", body)

    def test_acoustic_sweep_can_extend_beyond_internal(self):
        tables = audio.render(ROOT / audio.CAPTURE)
        for hz in (300, 3000):
            self.assertIn(f"| {hz} |", tables["audio-frequency"])
            self.assertNotIn(f"| {hz} |", tables["audio-loopback"])
        for hz in (500, 1000, 2000, 4000, 8000):
            self.assertIn(f"| {hz} |", tables["audio-loopback"])

    def test_internal_point_without_acoustic_partner_is_rejected(self):
        lines = (ROOT / audio.CAPTURE).read_text().splitlines()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.txt"
            path.write_text("\n".join(line for line in lines
                                      if not line.startswith("tone 500 500\t")))
            with self.assertRaisesRegex(ValueError, "paired"):
                audio.render(path)

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
            path.write_text(capture.replace('"tone_db": -51.8', '"tone_db": -32.7'))
            self.assertIn("-32.7", audio.render(path)["audio-frequency"])

    def test_build_mismatch_is_rejected(self):
        capture = (ROOT / audio.CAPTURE).read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.txt"
            path.write_text(capture.replace('164511c54388-dev', 'other-dev', 1))
            with self.assertRaisesRegex(ValueError, "build"):
                audio.render(path)

    def test_check_detects_stale_doc_without_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Audio.md"
            path.write_bytes((ROOT / audio.DOCUMENT).read_bytes())
            self.assertFalse(audio.update(ROOT / audio.CAPTURE, path, check=True))
            original = path.read_bytes().replace(b"-51.8", b"-32.7")
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
            'stage mix\tsound_mix\nstage present\tpresent\nperiod_us\t5804.99\nwindow_seconds\t1.5\n'
            'sound tap\tadpcm\n'
            'playing tap 0\t0\ndropped tap 0\t4\ndropped tap 0\t4\n'
            'point tap 0\tI (41179) shell: ms/frame avg/worst: present 9.0/10.0 | total 17.0 || sound_mix 0.001/0.002 ms/block n=1\n'
            'playing tap 1\t1\ndropped tap 1\t4\ndropped tap 1\t4\n'
            'point tap 1\tI (41179) shell: ms/frame avg/worst: present 10.0/11.0 | total 18.0 || sound_mix 0.050/0.090 ms/block n=2\n'
            'point tap 1\tI (41179) shell: ms/frame avg/worst: present 12.0/13.0 | total 20.0 || sound_mix 0.080/0.091 ms/block n=1\n'
            'stats tap 1\tSOUND ram voices=100 queue=200 stack=300 mixer=400\n'
            'stats tap 1\tSOUND stack_min_free=50 margin=20\n'
            'buildid\t{"build_id":"test-dev"}\n', encoding="utf-8")
        return path

    def test_weighted_windows_and_load(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory)
            body = audio.render_mix(path)
            self.assertIn("60.00 | 91.00 | 1.03 | 1.57 | 3", body)
            self.assertIn("5804.99 µs", body)
            self.assertIn("11.00 | 19.00 | +2.00", body)
            self.assertIn("capacity **8 voices**", body)
            self.assertIn("50 | 20 | 300 | 100 | 200 | 400", body)

    def test_captured_metadata_drives_tables(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory)
            path.write_text(path.read_text().replace("sound_mix", "mix_test")
                            .replace("stage present\tpresent", "stage present\tdisplay")
                            .replace("present ", "display ").replace("5804.99", "11609.98"), encoding="utf-8")
            body = audio.render_mix(path)
            self.assertIn("11609.98 µs", body)
            self.assertIn("display ms/frame", body)
            self.assertIn("60.00 | 91.00 | 0.52 | 0.78", body)

    def test_rejects_invalid_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.fixture(directory)
            original = path.read_text()
            for old, new, error in [('test-dev', 'other-dev', 'build'),
                                    ('voices\t0,1', 'voices\t0,1,4', 'point'),
                                    ('capacity\t8', 'capacity\t0', 'capacity'),
                                    ('playing tap 1\t1', 'playing tap 1\t0', 'playing'),
                                    ('playing tap 1\t1\n', '', 'playing'),
                                    ('dropped tap 1\t4\ndropped tap 1\t4',
                                     'dropped tap 1\t4\ndropped tap 1\t5', 'dropped'),
                                    ('dropped tap 1\t4\n', '', 'dropped'),
                                    ('present 9.0/10.0', 'absent 9.0/10.0', 'present'),
                                    ('total 17.0', 'absent 17.0', 'total'),
                                    ('n=1', 'n=0', 'window'),
                                    ('voices\t0,1', 'voices\t1', 'zero-voice')]:
                path.write_text(original.replace(old, new, 1))
                with self.assertRaisesRegex(ValueError, error):
                    audio.render_mix(path)
            for prefix in ("capacity", "voices", "period_us", "point tap 0", "stats tap 1"):
                path.write_text("\n".join(line for line in original.splitlines()
                                          if not line.startswith(prefix + "\t")), encoding="utf-8")
                error = "point" if prefix.startswith("point") else "stats" if prefix.startswith("stats") else f"missing {prefix}"
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
            self.assertIn("5804.99 µs", doc.read_text(encoding="utf-8"))
            self.assertIn("Average µs", doc.read_text(encoding="utf-8"))
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
            (tree / "launcher/main/engine/sounds").mkdir(parents=True)
            source = Path("launcher/main/engine/sounds/chime.sound.toml")
            asset = source.with_name("chime_pcm16.sound.toml")
            (tree / source).write_bytes(b'source = "chime.wav"\r\nformat = "adpcm"\r\nloop = true\r\n')
            original = b"#define OTHER 9\r\n#define SOUND_VOICE_COUNT 8\r\n"
            header.write_bytes(original)
            with mock.patch.object(image.subprocess, "run", return_value=argparse.Namespace(stdout="")):
                image.prepare(tree, "chime", "pcm16", 32)
                self.assertEqual(header.read_bytes(), original.replace(b"COUNT 8", b"COUNT 32"))
                self.assertEqual((tree / asset).read_bytes(), b'source = "chime.wav"\r\nformat = "pcm16"\r\nloop = true\r\n')
                self.assertEqual({p.relative_to(tree) for p in tree.rglob("*") if p.is_file()},
                                 {source, asset, image.HEADER})
                image.prepare(tree, "chime", "pcm16")
                self.assertIn(b"COUNT 32", header.read_bytes())
                with self.assertRaisesRegex(ValueError, "missing"):
                    image.prepare(tree, "absent", "pcm16")
                with self.assertRaisesRegex(ValueError, "already"):
                    image.prepare(tree, "chime", "adpcm")
                for invalid in [b"#define OTHER 8\n", original + original]:
                    header.write_bytes(invalid)
                    with self.assertRaisesRegex(ValueError, "exactly one"):
                        image.prepare(tree, "chime", "pcm16", 16)
            with mock.patch.object(image.subprocess, "run",
                                   return_value=argparse.Namespace(stdout=" M unrelated.txt\n")):
                with self.assertRaisesRegex(ValueError, "outside"):
                    image.prepare(tree, "chime", "pcm16", 16)
        with self.assertRaisesRegex(ValueError, "checkout"):
            image.prepare(ROOT, "chime", "pcm16")

    def capture_fake_autana_sequence(self, leftovers=0):
        capture = load_tool("sound_mix_capture")
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            sounds = tree / "launcher/main/engine/sounds"
            sounds.mkdir(parents=True)
            for sound, fmt in [("tap", "adpcm"), ("tap_pcm16", "pcm16")]:
                (sounds / f"{sound}.sound.toml").write_text(f'format = "{fmt}"\n')
            for owner in ["shell/shell.c", "audio/audio_out_device.h", "sound/sound_mixer.h",
                          "sound/sound_engine.h", "audio/audio_codec.h", "audio/audio_out.h",
                          "sound/sound_mixer_device.c", "shell/shell_apps.c"]:
                target = tree / "launcher/main" / owner
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((ROOT / "launcher/main" / owner).read_bytes())
            stub = tree / "fake.py"
            log = tree / "commands.jsonl"
            stub.write_text(
                'import json,sys\nfrom pathlib import Path\n'
                f'log=Path({str(log)!r})\n'
                'with log.open("a") as f: f.write(json.dumps(sys.argv[1:])+"\\n")\n'
                f'leftovers={leftovers}\n'
                'state=log.with_suffix(".state")\n'
                'playing,stops=json.loads(state.read_text()) if state.exists() else [0,0]\n'
                'args=sys.argv[1:]\n'
                'if args==["sound","stop","all"]: stops+=1; playing=int(stops<=leftovers)\n'
                'if args[:2]==["sound","play"]: playing+=1\n'
                'state.write_text(json.dumps([playing,stops]))\n'
                'if args==["buildid","--json"]: print(json.dumps({"build_id":"fake-dev"}))\n'
                'elif args==["sound","stats"]: print(f"SOUND ready=1 dropped=0\\nSOUND slot=0 playing={int(playing>0)}\\nSOUND slot=1 playing={int(playing>1)}\\nSOUND ram voices=10 queue=20 stack=30 mixer=40\\nSOUND stack_min_free=5 margin=2")\n'
                'elif args[:1]==["monitor"]: print("noise\\nI (41179) shell: ms/frame avg/worst: present 9.0/10.0 | total 17.0 || sound_mix 0.052/0.091 ms/block n=2")\n')
            wrapper = tree / ("autana.cmd" if os.name == "nt" else "autana")
            wrapper.write_text(f'@"{sys.executable}" "{stub}" %*\n' if os.name == "nt" else
                               f'#!/bin/sh\nexec "{sys.executable}" "{stub}" "$@"\n')
            wrapper.chmod(0o755)
            args = argparse.Namespace(autana=str(wrapper), tree=tree, out=tree / "capture.txt",
                                      board="TEST", sounds="tap,tap_pcm16", voices="0,1,2", seconds=1)
            block = (capture.read_define(tree / "launcher/main/audio/audio_codec.h", "AUDIO_CODEC_BLOCK_SAMPLES")
                     / capture.read_define(tree / "launcher/main/audio/audio_out.h", "AUDIO_OUT_SAMPLE_RATE_HZ"))
            waits = []
            if leftovers > capture.STOP_ATTEMPTS:
                with self.assertRaisesRegex(ValueError, "stop all"):
                    capture.capture(args, sleep=waits.append)
                commands = [json.loads(line) for line in log.read_text().splitlines()]
                self.assertEqual(commands.count(["sound", "stop", "all"]), capture.STOP_ATTEMPTS + 1)
                self.assertEqual(waits, [block] * capture.STOP_ATTEMPTS)
                return
            capture.capture(args, sleep=waits.append)
            commands = [json.loads(line) for line in log.read_text().splitlines()]
            expected = [["sound", "stats"], ["buildid", "--json"]]
            expected += ([["sound", "stop", "all"], ["sound", "stats"]] * (leftovers + 1)
                         + [["sound", "stats"], ["monitor", "1", "--stream"], ["sound", "stats"]])
            for sound in ["tap", "tap_pcm16"]:
                for count in [1, 2]:
                    expected += [["sound", "stop", "all"], ["sound", "stats"]] + [["sound", "play", sound, "loop"]] * count
                    expected += [["sound", "stats"], ["monitor", "1", "--stream"], ["sound", "stats"]]
            expected += [["sound", "stats"], ["sound", "stop", "all"], ["buildid", "--json"]]
            self.assertEqual(commands, expected)
            self.assertEqual(waits, [block] * (leftovers + 1) + [5.0]
                             + [block, 3.25] * 4)
            records = dict(line.split("\t", 1) for line in args.out.read_text().splitlines()[1:])
            self.assertEqual(records["stage mix"], "sound_mix")
            self.assertEqual(records["stage present"], "present")
            self.assertAlmostEqual(float(records["period_us"]), block * capture.US_PER_SECOND)
            self.assertEqual(float(records["window_seconds"]), 1.5)
            header = tree / "launcher/main/sound/sound_engine.h"
            original = header.read_bytes()
            header.write_bytes(original.replace(b"AUDIO_CODEC_BLOCK_SAMPLES", b"128u"))
            with self.assertRaisesRegex(ValueError, "identifier alias"):
                capture.capture(args, sleep=waits.append)
            header.write_bytes(original)
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

    def test_capture_fake_autana_sequence(self):
        self.capture_fake_autana_sequence()

    def test_leftover_retries_then_succeeds(self):
        self.capture_fake_autana_sequence(leftovers=1)

    def test_persistent_leftover_fails(self):
        self.capture_fake_autana_sequence(leftovers=100)


if __name__ == "__main__":
    unittest.main()
