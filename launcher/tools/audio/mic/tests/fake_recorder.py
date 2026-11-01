"""Fake autana capture transport with mutable codec registers and tone replies."""
import json


class FakeRecorder:
    def __init__(self, registers, build_id, tone, fail_command=None, tunables=None):
        self.registers = registers
        self.build_id = build_id
        self.tone = tone
        self.calls = []
        self.fail_command = fail_command
        self.tunables = tunables

    def __call__(self, command, *words):
        words = words[2:]
        self.calls.append(words)
        if self.fail_command and words[:len(self.fail_command)] == self.fail_command:
            raise OSError("injected capture failure")
        if words[0] == 'buildid':
            return json.dumps(dict(build_id=self.build_id))
        if words[0] == 'codec':
            reg = int(words[1], 0)
            if len(words) == 3:
                self.registers[reg] = int(words[2], 0)
            return f'CODEC reg=0x{reg:02X} value=0x{self.registers[reg]:02X}'
        if words[0] == 'tune' and self.tunables is not None:
            if words[1:] == ('--json',):
                return json.dumps(dict(tunables=[dict(name=k, value=v) for k, v in self.tunables.items()]))
            if len(words) == 3 and words[1] != 'reset':
                self.tunables[words[1]] = int(words[2])
        if words[0] == 'tone':
            return json.dumps(self.tone(words))
        return 'ok'


class FakeHostPlayer:
    def __init__(self, events, returncode=0):
        self.events = events
        self.returncode = returncode
        self.calls = []

    def __call__(self, command, **kwargs):
        self.calls.append(command)
        self.events.append(('host-play', *command))
        return self

    def wait(self, timeout=None):
        return self.returncode

    def poll(self):
        return self.returncode

    def terminate(self):
        pass
