"""Tests the stdlib mic capture client: framing, base64 decode and the WAV it writes."""
import base64
import json
import sys
import tempfile
import unittest
import wave
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'device'))
import mic

class FrameTests(unittest.TestCase):
    def test_chunked_serial_and_wav(self):
        pcm = b'\x00\x80\xff\x7f'
        state = dict(rate=22050, samples=2, automute=[[0, 1]], regs={'0x14': 0})
        payload = (mic.BEGIN + json.dumps(dict(rate=22050, samples=2)) + '\nlog line\n' +
                   mic.DATA + base64.b64encode(pcm).decode() + '\n' + mic.STATE + json.dumps(state) + '\n' + mic.END + '\n').encode()
        class Port:
            def reset_input_buffer(self): pass
            def write(self, data): self.command = data
            def read(self, count):
                nonlocal payload
                out, payload = payload[:7], payload[7:]
                return out
        port = Port()
        self.assertEqual(mic.read_mic(port, 1, 0), (pcm, state))
        self.assertEqual(port.command, b'mic record 1 0\n')
        with tempfile.TemporaryDirectory() as directory:
            out = mic.write_take(Path(directory) / 'take.wav', pcm, state)
            with wave.open(str(out)) as wav:
                self.assertEqual(wav.readframes(2), pcm)
                self.assertEqual(wav.getframerate(), state['rate'])
            self.assertEqual(json.loads(out.with_suffix('.json').read_text()), state)
    def test_reject_incomplete_or_corrupt(self):
        for body in (mic.DATA + '!!!!', mic.END):
            frame = mic.MicFrame()
            frame.feed(mic.BEGIN + '{"rate":22050,"samples":2}')
            with self.assertRaises(ValueError): frame.feed(body)
        with self.assertRaises(RuntimeError): mic.MicFrame().feed(mic.ERROR + 'capture failed')

if __name__ == '__main__': unittest.main()

class WriteTests(unittest.TestCase):
    def test_scheduled_write_wire_and_bounds(self):
        from unittest.mock import Mock
        port = Mock()
        port.read.return_value = (mic.BEGIN + '{"rate":22050,"samples":1}\n' + mic.DATA + 'AAA=\n' +
                                  mic.STATE + '{"rate":22050,"samples":1,"writes":[[0,50,40]]}\n' + mic.END + '\n').encode()
        _, state = mic.read_mic(port, 1000, at=[(0, 0x32, 40), (900, 0x32, 0)])
        port.write.assert_called_once_with(b'mic record 1000 at 0 50 40 at 900 50 0\n')
        self.assertEqual(state['writes'], [[0, 50, 40]])
        for writes in ([(1000, 0, 0)], [(1001, 0, 0)], [(0, 256, 0)], [(0, 0, 256)], [(0, 0, 0)] * (mic.WRITES_MAX + 1)):
            with self.assertRaises(ValueError):
                mic.read_mic(port, 1000, at=writes)
