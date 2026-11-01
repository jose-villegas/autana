"""Synthetic EQ probe measurements and failure cleanup."""
import json
import math
import cmath
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / 'launcher/tools'))
sys.path.insert(0, str(ROOT / 'launcher/tools/audio'))
from audio.mic import eq_probe_sweep as sweep
from audio.mic import mic_eq_probe_tables as tables
from audio.mic.tests.fake_recorder import FakeRecorder


class EqProbeTest(unittest.TestCase):
    def test_ram_clear_after_load(self):
        for owner, control, ram_reg, mask in (
                ('adc', sweep.ADC_CONTROL, 0x16, 0x08),
                ('dac', sweep.DAC_CONTROL, 0x31, 0x08)):
            calls = []
            def record(*words):
                calls.append(words)
                if words[0] == 'codec':
                    reg = int(words[1], 0)
                    value = int(words[2], 0) if len(words) == 3 else 0xA0
                    return f'CODEC reg=0x{reg:02X} value=0x{value:02X}'
                return 'ok'
            record.note = lambda *words: None
            sweep.program(record, {control: 0}, owner, sweep.designs()[owner]['half'])
            pulse = [('codec', hex(ram_reg), hex(0xA0 | mask)),
                     ('codec', hex(ram_reg), hex(0xA0))]
            self.assertEqual(calls[-2:], pulse)
            self.assertLess(calls.index(('codec', hex(control), '0x0')), calls.index(pulse[0]))

    def test_slot_classification_requires_every_value_and_tone(self):
        from audio.mic.eq_design import eq_classify_slots
        points = {reg: {value: {hz: 20 * math.log10(value)
                               for hz in (100, 1000, 4000)} for value in (.25, .5)}
                  for reg in (0x1D, 0x21)}
        for value in (.25, .5):
            points[0x21][value] = dict.fromkeys((100, 1000, 4000), -80)
        self.assertEqual(eq_classify_slots(points), {0x1D: 'numerator', 0x21: 'feedback'})
        points[0x1D][.5][4000] += 2
        self.assertEqual(eq_classify_slots(points)[0x1D], 'unresolved')
        del points[0x21][.25][100]
        self.assertEqual(eq_classify_slots(points)[0x21], 'unresolved')

    def test_slots_capture_tables_and_figures(self):
        from audio_doc_tables import read_capture
        from audio.mic.mic_eq_probe_figures import render
        import audio_doc_tables as audio
        from audio.mic.eq_design import SLOT_HZ, SCALE_VALUES
        layout = {'adc': {0x1D: ('feedback', 2, -1), 0x21: ('numerator', 0, 1),
                          0x25: ('feedback', 1, 1), 0x29: ('numerator', 2, 1), 0x2D: ('numerator', 1, 1)},
                  'dac': {0x38: ('feedback', 1, -1), 0x3C: ('numerator', 0, 1), 0x40: ('numerator', 1, 1)}}
        numerator = {owner: {reg for reg, (role, delay, sign) in slots.items() if role == 'numerator'}
                     for owner, slots in layout.items()}
        registers = dict.fromkeys(sweep.REGISTERS, 0)
        registers[sweep.CSM_REG] = sweep.CSM_ON
        def tone(words):
            level = -20.0
            for owner, control, mask in (('adc', sweep.ADC_CONTROL, sweep.ADC_BYPASS),
                                          ('dac', sweep.DAC_CONTROL, sweep.DAC_BYPASS)):
                if not registers[control] & mask:
                    from audio.mic.eq_design import eq_decode, SAMPLE_RATE_HZ
                    sums = {'numerator': 0j, 'feedback': 1 + 0j}
                    for reg, (role, delay, sign) in layout[owner].items():
                        encoded = int.from_bytes(bytes(registers[reg + i] for i in range(sweep.COEFF_BYTES)), 'big')
                        value = eq_decode(encoded)
                        phase = cmath.exp(-2j * math.pi * int(words[1]) * delay / SAMPLE_RATE_HZ)
                        sums[role] += value * sign * phase
                    gain = abs(sums['numerator'] / sums['feedback'])
                    level += 20 * math.log10(gain) if gain > 0 else -100
            return dict(hz=int(words[1]), tone_db=level)
        runner = FakeRecorder(registers, 'synthetic-slots', tone)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            args = sweep.parser().parse_args(['--board', 'TEST', '--mode', 'slots',
                                             '--out', str(output / 'slots.txt')])
            with mock.patch.object(sweep, 'autana_run', side_effect=runner):
                sweep.capture(args)
            _, records, _ = read_capture(args.out)
            data = tables.probe_data(records)
            for owner in numerator:
                points, roles = tables.slot_results(data, owner)
                self.assertEqual(tables.decided_layout(data, owner), layout[owner])
                self.assertIn((owner, 'rerun-highpass', 'acoustic'), data)
                self.assertEqual({r for r, role in roles.items() if role == 'numerator'}, numerator[owner])
                self.assertEqual(set(points), set(sweep.ADC_REGS if owner == 'adc' else sweep.DAC_REGS))
                scale = [(case, tones) for (kind, case, path), tones in data.items()
                         if kind == owner and case.startswith('scale-')]
                self.assertEqual(len(scale), len(SCALE_VALUES))
                scale_fit = tables.scale_result(data, owner)
                self.assertAlmostEqual(scale_fit[1], 20 * math.log10(2))
                self.assertAlmostEqual(scale_fit[2], 0)
                self.assertTrue(all(set(tones) == {sweep.SCALE_HZ} for case, tones in scale))
            self.assertTrue(any(command[:2] == ['probe', 'adc'] and command[2].startswith('delay-')
                                for command, reply in records))
            self.assertFalse(any('--internal' in command for command, reply in records))
            self.assertIn(('tune', 'audio.drc', 0), runner.calls)
            self.assertIn(('tune', 'reset', 'audio.drc'), runner.calls)
            self.assertEqual(registers, dict.fromkeys(sweep.REGISTERS, 0) | {sweep.CSM_REG: sweep.CSM_ON})
            document = output / 'probe.md'
            document.write_text('<!-- generated: mic-eq-probe -->\n<!-- /generated: mic-eq-probe -->\n')
            audio.update_eq_probe(output / 'absent.txt', document, slots_capture=args.out)
            self.assertIn('0x21: numerator', document.read_text())
            self.assertIn('0x1D: feedback', document.read_text())
            render(args.out, output)
            self.assertTrue((output / 'mic-eq-slots-adc.png').exists())
            self.assertTrue((output / 'mic-eq-slots-dac-dark.png').exists())

    def test_order_and_sign_decisions(self):
        from audio.mic.eq_design import (eq_decide_layout, eq_order_response,
                                         eq_layout_coefficients, SLOT_VALUES, SLOT_HZ)
        roles = {0x1D: ('feedback', 2, -1), 0x21: ('numerator', 0, 1),
                 0x25: ('feedback', 1, 1), 0x29: ('numerator', 2, 1), 0x2D: ('numerator', 1, 1)}
        points = {reg: {value: dict.fromkeys(SLOT_HZ, 20 * math.log10(value) if role == 'numerator' else -80)
                        for value in SLOT_VALUES} for reg, (role, delay, sign) in roles.items()}
        order = {reg: eq_order_response(role, delay, sign)
                 for reg, (role, delay, sign) in roles.items() if delay}
        layout = eq_decide_layout(points, order, 3)
        self.assertEqual(layout, roles)
        self.assertEqual(eq_layout_coefficients(sorted(roles), (1, 2, 3, 4, 5), layout, 3), [-5, 1, 4, 3, 2])
        order[0x29][SLOT_HZ[0]] += 10
        self.assertIsNone(eq_decide_layout(points, order, 3))

    def test_synthetic_response_and_cutoff(self):
        from audio_doc_tables import read_capture
        _, records, _ = read_capture(ROOT / 'launcher/test/fixtures/mic_eq_probe_capture.txt')
        data = tables.probe_data(records)
        self.assertAlmostEqual(data['adc', 'half', 'acoustic'][100], -6.0206, places=3)
        self.assertAlmostEqual(data['hpf', '4', 'acoustic'][100], -13)
        self.assertAlmostEqual(data['hpf', '10', 'acoustic'][100], -1)
        self.assertAlmostEqual(data['hpf', '12', 'acoustic'][100], 0)
        self.assertAlmostEqual(tables.cutoff({100: -9, 200: -3, 400: 0}), 200)
        self.assertIsNone(tables.cutoff({100: -1, 200: 0}))
        broken = [(cmd, dict(reply, hz=999) if cmd[:1] == ['tone'] else reply) for cmd, reply in records]
        with self.assertRaises(ValueError):
            tables.probe_data(broken)

    def test_generated_block_and_figures(self):
        import audio_doc_tables as audio
        from audio.mic.mic_eq_probe_figures import render
        fixture = ROOT / 'launcher/test/fixtures/mic_eq_probe_capture.txt'
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            document = output / 'probe.md'
            document.write_bytes(b'<!-- generated: mic-eq-probe -->\r\n<!-- /generated: mic-eq-probe -->\r\n')
            self.assertTrue(audio.update_eq_probe(output / 'absent.txt', document))
            self.assertIn('No capture yet.', document.read_text())
            self.assertTrue(audio.update_eq_probe(fixture, document))
            self.assertFalse(audio.update_eq_probe(fixture, document, check=True))
            self.assertIn('Error (dB)', document.read_text())
            self.assertIn('Cutoff relative to code 12', document.read_text())
            self.assertIn('100 Hz raw (dBFS)', document.read_text())
            self.assertNotIn(b'\n', document.read_bytes().replace(b'\r\n', b''))
            render(fixture, output)
            for owner in ('adc', 'dac', 'hpf'):
                light = output / ('mic-eq-probe-' + owner + '.png')
                dark = output / ('mic-eq-probe-' + owner + '-dark.png')
                self.assertTrue(light.exists())
                self.assertNotEqual(light.read_bytes(), dark.read_bytes())

    def test_capture_and_failed_write_restore(self):
        for fail in (False, True):
            with self.subTest(fail=fail), tempfile.TemporaryDirectory() as directory:
                args = sweep.parser().parse_args(['--board', 'TEST', '--out', str(Path(directory) / 'raw.txt'),
                                                 '--hz', '100,1000', '--hpf-hz', '100,200', '--codes', '4',
                                                 '--runs', '1', '--take', str(Path(directory) / 'take.wav')])
                calls = []
                failed = False
                def runner(command, *words):
                    nonlocal failed
                    self.assertEqual(words[:2], ('--board', 'TEST'))
                    words = words[2:]
                    calls.append(words)
                    if words[0] == 'buildid':
                        return '{"build_id":"synthetic"}'
                    if words[0] == 'codec':
                        reg = int(words[1], 0)
                        if len(words) == 3 and fail and not failed:
                            failed = True
                            raise OSError('write failed')
                        value = int(words[2], 0) if len(words) == 3 else reg
                        return f'CODEC reg=0x{reg:02X} value=0x{value:02X}'
                    if words[0] == 'tone':
                        return json.dumps(dict(hz=int(words[1]), tone_db=-20,
                                               path='internal' if '--internal' in words else 'acoustic'))
                    return 'ok'
                with mock.patch.object(sweep, 'autana_run', side_effect=runner):
                    if fail:
                        with self.assertRaises(OSError):
                            sweep.capture(args)
                    else:
                        sweep.capture(args)
                writes = [(int(w[1], 0), int(w[2], 0)) for w in calls if w[0] == 'codec' and len(w) == 3]
                restored = dict(writes)
                expected = {reg: reg for reg in sweep.REGISTERS}
                expected[sweep.CSM_REG] |= sweep.CSM_ON
                self.assertEqual(restored, expected)
                self.assertEqual(writes[-1], (sweep.CSM_REG, expected[sweep.CSM_REG]))
                self.assertTrue(all(('tune', 'reset', 'mic.' + name) in calls for name in sweep.TUNABLES))
                text = args.out.read_text()
                self.assertIn('buildid\t', text)
                if not fail:
                    self.assertIn('--internal', text)
                    self.assertIn('mic record 2000', text)


if __name__ == '__main__':
    unittest.main()
