"""Capture-derived audio tables and their regeneration contract."""
import importlib.util
import io
import tempfile
import argparse
import json
import os
import re
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
            self.assertNotIn("A308", body)
            self.assertNotIn("164511c54388-dev", body)
            self.assertIn("[raw replies]", body)

    def test_all_blocks_keep_provenance_in_raw_data(self):
        gate = load_tool("sound_gate_tables")
        captures = [ROOT / audio.CAPTURE, ROOT / audio.DRC_CAPTURE,
                    *sorted((ROOT / audio.MIX_CAPTURES).glob("sound_mix_capture*.txt"))]
        bodies = list(audio.render(captures[0]).values()) + [audio.render_drc(captures[1])]
        bodies += [audio.render_mix(path) for path in captures[2:]]
        expected_images = {Path(name).name for body in bodies
                           for name in re.findall(r'\.\./images/([^" ]+\.png)', body)}
        data = json.loads(gate.DATA.read_text())
        bodies += list(gate.render(gate.DATA).values())
        identifiers = [data["board"], data["board"].replace(":", "")[-4:]]
        dates = list(data["capture_date"])
        identifiers += [build for side in data["sides"].values() for build in side["build_ids"]]
        identifiers.append(str(data["rng_seed"]))
        for path in captures:
            header = path.read_text().splitlines()[0].split(" on ", 1)[1]
            board, date, _ = header.split(", ", 2)
            identifiers.append(board)
            dates.append(date)
        for body in bodies:
            self.assertRegex(body, r"\[raw (?:capture|replies)\]\(data/")
            self.assertNotRegex(body, r"\b[0-9a-f]{7,40}-(?:dev|diag|rel)\b")
            for value in identifiers + dates:
                self.assertNotIn(value, body)
        for name in ("audio-frequency", "audio-volume"):
            self.assertIn("<details>", audio.render(captures[0])[name])
            self.assertIn("<picture>", audio.render(captures[0])[name])
        for body in bodies[len(audio.render(captures[0])) + 1:-2]:
            self.assertIn("<details>", body)
            self.assertIn("<picture>", body)

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            for module, capture in (("audio_path_figures", captures[0]), ("audio_drc_figures", captures[1]),
                                    *(("sound_mix_figures", path) for path in captures[2:])):
                load_tool(module).render(capture, output)
            images = sorted(output.glob("*.png"))
            self.assertEqual({path.name for path in images}, expected_images)
            for path in images:
                if not path.stem.endswith("-dark"):
                    self.assertNotEqual(path.read_bytes(), path.with_stem(path.stem + "-dark").read_bytes())

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
            self.assertEqual({name: body.replace(path.name, capture.name) for name, body in audio.render(path).items()},
                             audio.render(capture))

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


class AudioLoopbackTablesTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.capture = Path(temporary.name) / "capture.txt"
        self.document = Path(temporary.name) / "Audio.md"
        self.records = [("buildid", {"build_id": "fixture-dev"}),
                        ("heap", {"free": 10000, "largest": 8000})]
        for command, hz, path in (("tone 300 500", 300, "acoustic"),
                                   ("tone 1000 500", 1000, "acoustic"),
                                   ("tone 1000 500 30", 1000, "acoustic"),
                                   ("tone 300 500 --internal", 300, "internal"),
                                   ("tone 1000 500 --json --internal", 1000, "internal")):
            internal = path == "internal"
            self.records.append((command, dict(hz=hz, tone_db=-12.3 if internal else -21.5,
                silence_db=-110.0, heard=True, open_dma_bytes=3040, path=path,
                measured_hz=hz + (0.02 if internal else -0.03), hz_error=0.02 if internal else -0.03)))
        self.records.append(("heap", {"free": 10000, "largest": 8000}))
        self.save()

    def save(self):
        self.capture.write_text("# autana --json replies on TEST, 2026-10-19, fixture\n" +
            "\n".join(command + "\t" + json.dumps(reply) for command, reply in self.records))

    def test_comparison_and_regeneration(self):
        rendered = audio.render(self.capture)
        comparison = rendered["audio-loopback"]
        self.assertIn("Duration 500 ms; volume 30%.", comparison)
        self.assertNotIn("Internal vs acoustic loopback", comparison)
        self.assertIn("| Frequency (Hz) | Acoustic level (dBFS) | Internal level (dBFS) | "
                      "Acoustic error (Hz) | Internal error (Hz) |", comparison)
        self.assertIn("| 300 | -21.5 | -12.3 | -0.03 | +0.02 |", comparison)
        self.assertIn("| 1000 | -21.5 | -12.3 | -0.03 | +0.02 |", comparison)
        self.document.write_bytes("\r\n".join(f"<!-- generated: {name} -->\r\n<!-- /generated: {name} -->"
            for name in rendered).encode())
        self.assertTrue(audio.update(self.capture, self.document))
        self.assertFalse(audio.update(self.capture, self.document, check=True))
        self.assertNotIn(b"\n", self.document.read_bytes().replace(b"\r\n", b""))

    def test_missing_internal_is_rejected_when_block_exists(self):
        self.records = [(command, reply) for command, reply in self.records if "--internal" not in command]
        self.save()
        self.document.write_text("<!-- generated: audio-loopback -->\n<!-- /generated: audio-loopback -->\n")
        with self.assertRaisesRegex(ValueError, "internal"):
            audio.update(self.capture, self.document)

    def test_explicit_default_volume_matches_implicit(self):
        self.records = [(command.replace("tone 300 500 --internal", "tone 300 500 30 --internal"), reply)
                        for command, reply in self.records]
        self.save()
        self.assertIn("| 300 |", audio.render(self.capture)["audio-loopback"])

    def test_null_frequency_is_rendered_as_unavailable(self):
        for command, reply in self.records:
            if command.startswith("tone"):
                reply["measured_hz"] = None
                reply["hz_error"] = None
        self.save()
        self.assertIn("| 300 | -21.5 | -12.3 | n/a | n/a |", audio.render(self.capture)["audio-loopback"])

    def test_invalid_comparison_records(self):
        original = json.loads(json.dumps(self.records))
        cases = [("duplicate", "duplicate internal"), ("path", "path disagree"),
                 ("unknown", "flags"), ("flag_duplicate", "flags"),
                 ("duration", "duration and volume"), ("volume", "duration and volume"),
                 ("volume_sweep", "volume sweep"), ("heaps", "heaps"),
                 ("header", "header"), ("build", "build id"),
                 ("measured_hz", "measured_hz"), ("hz_error", "hz_error")]
        for case, error in cases:
            with self.subTest(case=case):
                self.records = json.loads(json.dumps(original))
                if case == "duplicate":
                    self.records.append(self.records[5])
                elif case == "path":
                    self.records[5][1]["path"] = "acoustic"
                elif case in ("unknown", "flag_duplicate"):
                    self.records[5][0] += " --bad" if case == "unknown" else " --internal"
                elif case in ("duration", "volume"):
                    self.records[5][0] = "tone 300 600 --internal" if case == "duration" else "tone 300 500 31 --internal"
                elif case == "volume_sweep":
                    self.records.append(["tone 1000 600 30", self.records[4][1]])
                elif case == "heaps":
                    self.records.pop()
                elif case == "build":
                    self.records.append(["buildid", {"build_id": "other-dev"}])
                elif case in ("measured_hz", "hz_error"):
                    del self.records[5][1][case]
                self.save()
                if case == "header":
                    self.capture.write_text(self.capture.read_text().replace("# autana", "# invalid"))
                with self.assertRaisesRegex(ValueError, error):
                    audio.render(self.capture)

    def test_null_frequency_is_independent_for_each_path(self):
        for path, expected in (("internal", "| 300 | -21.5 | -12.3 | -0.03 | n/a |"),
                               ("acoustic", "| 300 | -21.5 | -12.3 | n/a | +0.02 |")):
            with self.subTest(path=path):
                for command, reply in self.records:
                    if command.startswith("tone"):
                        missing = reply["path"] == path
                        reply["hz_error"] = None if missing else (0.02 if path == "acoustic" else -0.03)
                        reply["measured_hz"] = None if missing else reply["hz"] + reply["hz_error"]
                self.save()
                self.assertIn(expected, audio.render(self.capture)["audio-loopback"])

    def test_capture_without_internal_omits_loopback(self):
        self.records = [(command, reply) for command, reply in self.records if "--internal" not in command]
        self.save()
        rendered = audio.render(self.capture)
        self.assertNotIn("audio-loopback", rendered)
        self.document.write_text("\n".join(f"<!-- generated: {name} -->\n<!-- /generated: {name} -->"
                                           for name in rendered))
        self.assertTrue(audio.update(self.capture, self.document))
        self.assertFalse(audio.update(self.capture, self.document, check=True))
        self.assertNotIn("audio-loopback", self.document.read_text())


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
            with mock.patch.object(image.subprocess, "run", return_value=argparse.Namespace(stdout="")), \
                    mock.patch.object(image, "lock_variant") as lock:
                image.prepare(tree, "chime", "pcm16", 32)
                lock.assert_called_with(tree.resolve(), "chime_pcm16.sond")
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

    def capture_fake_autana_sequence(self, leftovers=0, fill=None):
        capture = load_tool("sound_mix_capture")
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            sounds = tree / "launcher/main/engine/sounds"
            sounds.mkdir(parents=True)
            (sounds.parent / "engine.pack.toml").write_text("")
            for sound, fmt in [("tap", "adpcm"), ("tap_pcm16", "pcm16")]:
                (sounds / f"{sound}.sound.toml").write_text(f'format = "{fmt}"\n')
            for owner in ["shell/shell.c", "audio/audio_out_device.h", "sound/sound_mixer.h",
                          "sound/sound_engine.h", "audio/audio_codec.h", "audio/audio_out.h",
                          "sound/sound_mixer_device.c", "shell/shell_apps.c", "asset/asset_store.h"]:
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
                                      board="TEST", sounds="engine/tap,engine/tap_pcm16" if fill else "tap,tap_pcm16",
                                      fill_sound=fill, voices="0,1,2", seconds=1)
            block = (capture.read_define(tree / "launcher/main/audio/audio_codec.h", "AUDIO_CODEC_BLOCK_SAMPLES")
                     / capture.read_define(tree / "launcher/main/audio/audio_out.h", "AUDIO_OUT_SAMPLE_RATE_HZ"))
            window = (capture.read_define(tree / "launcher/main/shell/shell.c", "SHELL_REPORT_WINDOW_US")
                      / capture.US_PER_SECOND)
            startup = (capture.read_define(tree / "launcher/main/audio/audio_out_device.h", "AUDIO_OUT_AMP_STARTUP_MS")
                       / capture.MS_PER_SECOND)
            settle = startup + capture.REPORT_WINDOWS * window
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
            for sound in args.sounds.split(","):
                for count in [1, 2]:
                    expected += [["sound", "stop", "all"], ["sound", "stats"]]
                    expected += [["sound", "play", fill if fill and index else sound, "loop"] for index in range(count)]
                    expected += [["sound", "stats"], ["monitor", "1", "--stream"], ["sound", "stats"]]
            expected += [["sound", "stats"], ["sound", "stop", "all"], ["buildid", "--json"]]
            self.assertEqual(commands, expected)
            self.assertEqual(waits, [block] * (leftovers + 1) + [settle]
                             + [block, settle] * 4)
            records = dict(line.split("\t", 1) for line in args.out.read_text().splitlines()[1:])
            self.assertEqual(records["stage mix"], "sound_mix")
            self.assertEqual(records["stage present"], "present")
            self.assertAlmostEqual(float(records["period_us"]), block * capture.US_PER_SECOND)
            self.assertEqual(float(records["window_seconds"]), window)
            header = tree / "launcher/main/sound/sound_engine.h"
            original = header.read_bytes()
            header.write_bytes(original.replace(b"AUDIO_CODEC_BLOCK_SAMPLES", b"128u"))
            with self.assertRaisesRegex(ValueError, "identifier alias"):
                capture.capture(args, sleep=waits.append)
            header.write_bytes(original)
            body = audio.render_mix(args.out)
            self.assertNotIn("fake-dev", body)
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

    def test_qualified_sounds_and_fill_voice_sequence(self):
        self.capture_fake_autana_sequence(fill="engine/tap")

    def test_leftover_retries_then_succeeds(self):
        self.capture_fake_autana_sequence(leftovers=1)

    def test_persistent_leftover_fails(self):
        self.capture_fake_autana_sequence(leftovers=100)


sweep = load_tool("audio_drc_sweep")
import autana_runner as runner


class DrcSweepTest(unittest.TestCase):
    def test_failed_command_retries_and_records_once(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            stub = tree / "fake.py"
            log = tree / "commands.jsonl"
            stub.write_text(
                'import json,sys\nfrom pathlib import Path\n'
                f'log=Path({str(log)!r})\n'
                'calls=log.read_text().splitlines() if log.exists() else []\n'
                'args=sys.argv[1:]\n'
                'with log.open("a") as output: output.write(json.dumps(args)+"\\n")\n'
                'if args[0]=="tone" and not any(json.loads(call)[0]=="tone" for call in calls):\n'
                '    print("transient failure\\nplease retry",file=sys.stderr); sys.exit(1)\n'
                'print(json.dumps({"build_id":"fake-dev"}) if args[0]=="buildid" else "{}")\n')
            args = sweep.parser().parse_args([
                "--board", "TEST", "--out", str(tree / "capture.txt"),
                "--autana", f'"{Path(sys.executable).as_posix()}" "{stub.as_posix()}"',
                "--baseline", "30", "--codec", "50", "--max-levels", "0",
                "--volumes", "100", "--extra-hz", "", "--runs", "1"])
            with mock.patch.object(runner.time, "sleep") as sleep, mock.patch("sys.stderr", new_callable=io.StringIO) as stderr:
                sweep.capture(args)
            commands = [json.loads(line) for line in log.read_text().splitlines()]
            records = args.out.read_text().splitlines()
            tones = [line for line in records if line.startswith("tone ")]
            self.assertEqual(len(tones), len(list(sweep.configurations(args))))
            self.assertEqual(sum(command[0] == "tone" for command in commands), len(tones) + 1)
            self.assertEqual(len(stderr.getvalue().splitlines()), 1)
            self.assertIn("transient failure please retry", stderr.getvalue())
            sleep.assert_called_once_with(runner.AUTANA_RUN_RETRY_SECONDS)

    def test_command_failure_exhausts_attempts_with_stderr(self):
        for attempts in (runner.AUTANA_RUN_ATTEMPTS, runner.AUTANA_RUN_ATTEMPTS + 1):
            with self.subTest(attempts=attempts):
                failures = [runner.subprocess.CalledProcessError(1, ["autana"], stderr=f"failure {attempt}")
                            for attempt in range(attempts)]
                with mock.patch.object(runner, "AUTANA_RUN_ATTEMPTS", attempts), mock.patch.object(
                        runner.subprocess, "run", side_effect=failures) as run, mock.patch.object(
                        runner.time, "sleep") as sleep, mock.patch("sys.stderr", new_callable=io.StringIO) as stderr:
                    with self.assertRaisesRegex(runner.subprocess.CalledProcessError, f"failure {attempts - 1}") as raised:
                        runner.autana_run("autana", "tune", "audio.drc_max", 7)
                self.assertEqual(run.call_count, attempts)
                self.assertEqual(sleep.call_args_list, [mock.call(runner.AUTANA_RUN_RETRY_SECONDS)] * (attempts - 1))
                self.assertEqual(len(stderr.getvalue().splitlines()), attempts)
                self.assertEqual(raised.exception.stderr, failures[-1].stderr)

    def test_datasheet_and_tunable_contract(self):
        self.assertAlmostEqual(sweep.DRC_LEVEL_DB[0], -30.1, places=1)
        self.assertAlmostEqual(sweep.DRC_LEVEL_DB[-1], -6.0, places=1)
        import re
        source = (ROOT / "launcher/main/audio/audio_out.c").read_text()
        self.assertEqual(set(sweep.TUNABLES), set(re.findall(r"TUNE\(audio,\s*(\w+),", source)))

    def test_out_of_range_and_zero_max(self):
        for option, value in (("--baseline", "101"), ("--max-levels", "16"),
                              ("--hz", "22050"), ("--ms", "65536")):
            with self.subTest(option=option), tempfile.TemporaryDirectory() as directory:
                args = sweep.parser().parse_args(["--board", "TEST", "--out", str(Path(directory) / "out"),
                                                  option, value])
                with mock.patch.object(sweep, "autana_run", side_effect=AssertionError("must reject before commands")), self.assertRaises(ValueError):
                    sweep.capture(args)
        args = sweep.parser().parse_args(["--board", "TEST", "--max-levels", "0"])
        configs = list(sweep.configurations(args))
        self.assertEqual(len(configs), len({tuple(config.items()) for config in configs}))

    def test_default_plan(self):
        args = sweep.parser().parse_args(["--board", "TEST"])
        configs = list(sweep.configurations(args))
        self.assertEqual(len(configs), 7 + 2 * 4 * 2)
        self.assertEqual(configs[0], dict(codec_volume=30, drc=0, drc_max=0, drc_min=0, drc_win=4, drc_ramp=2))
        self.assertEqual(list(sweep.tones(args)), [(1000, 10), (1000, 30), (1000, 100)])
        self.assertEqual(args.codec, (70, 100))
        self.assertEqual(args.max_levels, (1, 2, 3, 4))
        self.assertEqual(args.winsize, (4,))
        self.assertEqual(args.ramp, (2,))
        self.assertEqual(args.runs, 3)
        self.assertEqual(configs[-1]["drc_min"], 0)

    def test_window_ramp_product(self):
        args = sweep.parser().parse_args(["--board", "TEST", "--baseline", "30", "--codec", "70,100",
            "--max-levels", "1,2", "--winsize", "1,4", "--ramp", "2,3", "--extra-hz", "500,2000"])
        self.assertEqual(args.winsize, (1, 4))
        self.assertEqual(args.ramp, (2, 3))
        configs = list(sweep.configurations(args))
        expected = {(codec, maximum, minimum, window, ramp)
                    for codec in (70, 100) for maximum in (1, 2) for minimum in (maximum, 0)
                    for window in args.winsize for ramp in args.ramp}
        actual = {(c["codec_volume"], c["drc_max"], c["drc_min"], c["drc_win"], c["drc_ramp"])
                  for c in configs if c["drc"]}
        self.assertEqual(actual, expected)
        self.assertEqual(len(configs), len(expected) + 1)
        self.assertEqual(list(sweep.tones(args))[-2:], [(500, 100), (2000, 100)])

    def test_invalid_window_ramp_lists(self):
        for option in ("--winsize", "--ramp"):
            for value in ("", "1,16", "-1,4"):
                with self.subTest(option=option, value=value):
                    args = sweep.parser().parse_args(["--board", "TEST", f"{option}={value}"])
                    with self.assertRaises(ValueError):
                        list(sweep.configurations(args))

    def test_capture_and_cleanup_after_failure(self):
        for fail in (False, True):
            with self.subTest(fail=fail), tempfile.TemporaryDirectory() as directory:
                out = Path(directory) / "capture.txt"
                args = sweep.parser().parse_args(["--board", "TEST", "--out", str(out),
                    "--autana", 'python "path with spaces/autana.py"', "--baseline", "30", "--codec", "50",
                    "--max-levels", "15", "--volumes", "10,30,100", "--extra-hz", "", "--runs", "1"])
                calls = []
                def run(command, **kwargs):
                    calls.append(command)
                    self.assertEqual(command[:2], [runner.shutil.which("python") or "python", "path with spaces/autana.py"])
                    if command[2] == "tone":
                        if fail:
                            raise RuntimeError("tone failed")
                        payload = dict(hz=int(command[3]), tone_db=-12.0, thd_db=-40.0, peak_dbfs=-0.4)
                    elif command[2] == "buildid":
                        payload = dict(build_id="test-dev")
                    else:
                        return mock.Mock(stdout="audio.test=0\n")
                    return mock.Mock(stdout=json.dumps(payload))
                with mock.patch.object(runner.subprocess, "run", side_effect=run):
                    if fail:
                        with self.assertRaisesRegex(RuntimeError, "tone failed"):
                            sweep.capture(args)
                    else:
                        sweep.capture(args)
                self.assertEqual(calls[-len(sweep.TUNABLES):],
                    [[runner.shutil.which("python") or "python", "path with spaces/autana.py", "tune", "reset", "audio." + name] for name in sweep.TUNABLES])
                records = out.read_text().splitlines()
                self.assertTrue(records[0].startswith("# autana --json replies on TEST,"))
                self.assertTrue(records[1].startswith("buildid\t"))
                if not fail:
                    self.assertIn("-6.0", audio.render_drc(out))
                    self.assertIn("yes", audio.render_drc(out))

    def test_fixture_medians(self):
        body = audio.render_drc(ROOT / "launcher/test/fixtures/audio_drc_capture.txt")
        self.assertIn("| 30 | -31.4 | off | 4 | 2 | -40.0 | -30.0 | -20.0 | -45.0 | -0.4 | yes |", body)
        self.assertIn("max -6.0 / min -30.1", body)

    def test_committed_capture_and_static_output(self):
        body = audio.render_drc(ROOT / audio.DRC_CAPTURE)
        self.assertNotIn("Board **2124**", body)
        self.assertIn("<picture>", body)
        self.assertIn("<details>", body)
        self.assertIn("| Default | -28.4 | -19.1 | -17.7 | -42.0 |", body)
        self.assertIn("Static full-scale DAC output (dBFS)", body)
        self.assertIn("| 50 | -21.4 | off |", body)
        self.assertIn("| 70 | -11.4 | max -20.6 / min -20.6 | 4 | 2 |", body)
        self.assertIn("-28.4 | -19.1 | -17.7 | -42.0", body)
        fixture = ROOT / "launcher/test/fixtures/audio_drc_capture.txt"
        with mock.patch.object(audio, "CODEC_VOLUME_MIN_DB", -40), \
             mock.patch.object(audio, "CODEC_VOLUME_MAX_DB", 10), \
             mock.patch.object(audio, "CODEC_PA_SUPPLY_VOLTS", 3.3):
            self.assertIn("| 30 | -25.0 | off |", audio.render_drc(fixture))

    def test_primary_frequency_and_volumes_from_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.txt"
            fixture = ROOT / "launcher/test/fixtures/audio_drc_capture.txt"
            capture.write_text(fixture.read_text().replace("1000", "1500").replace("500 10\t", "500 20\t"))
            body = audio.render_drc(capture)
            self.assertIn("1500 Hz", body)
            self.assertIn("Level 20% (dBFS) | Level 30% (dBFS) | Level 100% (dBFS) | THD 100% (dB)", body)

    def test_capture_markers(self):
        with tempfile.TemporaryDirectory() as directory:
            doc = Path(directory) / "Audio.md"
            missing = Path(directory) / "missing.txt"
            doc.write_bytes(b"# Audio\r\n\r\n<!-- generated: audio-drc -->\r\n<!-- /generated: audio-drc -->\r\n")
            self.assertTrue(audio.update_drc(missing, doc))
            self.assertIn("No capture yet.", doc.read_text())
            self.assertFalse(audio.update_drc(missing, doc, check=True))
            fixture = ROOT / "launcher/test/fixtures/audio_drc_capture.txt"
            self.assertTrue(audio.update_drc(fixture, doc))
            self.assertFalse(audio.update_drc(fixture, doc, check=True))
            self.assertNotIn(b"\n", doc.read_bytes().replace(b"\r\n", b""))
            doc.write_text("# Audio\n")
            with self.assertRaises((ValueError, RuntimeError)):
                audio.update_drc(fixture, doc)

    def test_measurement_selection_and_rejections(self):
        fixture = (ROOT / "launcher/test/fixtures/audio_drc_capture.txt").read_text()
        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "capture.txt"
            records = []
            for line in fixture.splitlines():
                if not line.startswith("tone"):
                    records.append(line)
                    continue
                command, payload = line.split("\t")
                reply = json.loads(payload)
                hz, _, volume = map(int, command.split()[1:])
                reply["thd_db"] = -42.0 if hz == 1000 and volume == 100 else -12.0
                reply["peak_dbfs"] = audio.DRC_CLIPPED_DBFS - 1
                records.append(command + "\t" + json.dumps(reply))
            capture.write_text("\n".join(records))
            body = audio.render_drc(capture)
            self.assertIn("-42.0 | -1.5 | no", body)
            command, payload = next(line.split("\t") for line in records if line.startswith("tone 1000 "))
            reply = json.loads(payload)
            reply.update(hz=500, thd_db=-12.0, peak_dbfs=audio.DRC_CLIPPED_DBFS)
            records.append("tone 500 500 100\t" + json.dumps(reply))
            capture.write_text("\n".join(records))
            self.assertIn("-42.0 | -0.5 | yes", audio.render_drc(capture))
            capture.write_text("\n".join(records).replace('"thd_db": -42.0', '"thd_db": null')
                               .replace('"thd_db": -12.0', '"thd_db": null'))
            self.assertIn("| n/a |", audio.render_drc(capture))
            for source, replacement, error in (("audio.drc_max", "audio.unknown", "unknown"),
                                                ('"hz": 1000', '"hz": 999', "disagree")):
                capture.write_text(fixture.replace(source, replacement))
                with self.assertRaisesRegex(ValueError, error):
                    audio.render_drc(capture)

    def test_failed_reset_attempts_remaining_tunables(self):
        with tempfile.TemporaryDirectory() as directory:
            args = sweep.parser().parse_args(["--board", "TEST", "--out", str(Path(directory) / "out")])
            calls = []
            def run(command, *words):
                calls.append(words)
                if words[0] == "buildid":
                    return '{"build_id": "test-dev"}'
                if words[:2] == ("tune", "reset") and words[2] != "audio." + sweep.TUNABLES[0]:
                    return "reset"
                raise OSError("failed command")
            with mock.patch.object(sweep, "autana_run", side_effect=run), self.assertRaisesRegex(RuntimeError, "reset"):
                sweep.capture(args)
            self.assertEqual(calls[-len(sweep.TUNABLES):],
                             [("tune", "reset", "audio." + name) for name in sweep.TUNABLES])



class AudioFiguresTest(unittest.TestCase):
    def test_onset_capture_medians_and_bounds(self):
        figures = load_tool("sound_onset_figures")
        settings = {"off": {figures.DECLICK_TUNABLE: 0}, "short": {figures.DECLICK_TUNABLE: 500}}
        captures = []
        for samples in ([0, 2], [4], [6, 8, 100]):
            points = {(arm, event): [(value, 0) for value in samples]
                      for arm in settings for event in audio.ONSET_EVENTS}
            captures.append((settings, points, len(samples)))
        times, curves = figures.figure_data(captures)
        self.assertEqual(times, [0, 0.5])
        self.assertEqual(tuple(curve[0] for curve in curves), figures.EVENT_ORDER)
        for event, medians, lows, highs in curves:
            self.assertEqual(medians, [4, 4])
            self.assertEqual(lows, [1, 1])
            self.assertEqual(highs, [8, 8])
        changed = dict(settings, short={figures.DECLICK_TUNABLE: audio.US_PER_MS})
        with self.assertRaisesRegex(ValueError, "arms"):
            figures.figure_data([captures[0], (changed, captures[1][1], 1)])

    def test_onset_themed_images(self):
        figures = load_tool("sound_onset_figures")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            figures.render(ROOT / audio.MIX_CAPTURES, output)
            light = output / "sound-onset.png"
            dark = output / "sound-onset-dark.png"
            self.assertTrue(light.is_file())
            self.assertNotEqual(light.read_bytes(), dark.read_bytes())

    def test_extraction_and_plot_selection(self):
        figures = load_tool("audio_drc_figures")
        _, records, _ = audio.read_capture(ROOT / audio.DRC_CAPTURE)
        hz, volumes, points = audio.drc_data(records)
        self.assertEqual(hz, 1000)
        self.assertEqual(volumes, [10, 30, 100])
        curves, thd, default = figures.figure_data(volumes, points)
        self.assertEqual(len(curves), 5)
        self.assertEqual([round(value, 4) for value in curves[0][1]], [-20.0, -10.4576, 0.0])
        self.assertEqual(curves[0][2], [-48.0, -38.5, -28.2])
        self.assertEqual([codec for codec, value in thd], [30, 40, 50, 60, 70, 85, 100])
        self.assertEqual(default, (70, -42.0))
        changed = dict(points)
        changed[audio.DRC_DEFAULT] = dict(points[audio.DRC_DEFAULT], levels=[-60, -40, -20], thd=-55)
        curves, _, default = figures.figure_data(volumes, changed)
        self.assertEqual(curves[-2][2], [-60, -40, -20])
        self.assertEqual(default, (70, -55))
        del changed[audio.DRC_DEFAULT]
        with self.assertRaisesRegex(ValueError, "default"):
            figures.figure_data(volumes, changed)

    def test_path_capture_uses_generator_commands(self):
        capture = load_tool("audio_path_capture")
        _, records, _ = audio.read_capture(ROOT / audio.CAPTURE)
        self.assertEqual([command for command, _ in records], [list(command) for command in audio.CAPTURE_COMMANDS])
        replies = [json.dumps(reply) for _, reply in records]
        with tempfile.TemporaryDirectory() as directory:
            args = capture.parser().parse_args(["--board", "TEST", "--autana", "fake", "--out", str(Path(directory) / "capture.txt")])
            with mock.patch.object(capture, "autana_run", side_effect=replies) as run:
                capture.capture(args)
            self.assertEqual(run.call_args_list, [mock.call("fake", *command, "--json") for command in audio.CAPTURE_COMMANDS])
            self.assertEqual({name: body.replace(args.out.name, audio.CAPTURE.name)
                              for name, body in audio.render(args.out).items()}, audio.render(ROOT / audio.CAPTURE))
            self.assertIn("on TEST,", args.out.read_text().splitlines()[0])
            with mock.patch.object(capture, "autana_run", side_effect=OSError("failed")), self.assertRaises(OSError):
                capture.capture(args)

if __name__ == "__main__":
    unittest.main()
