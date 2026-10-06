"""Scripted serial handles for device command tests; no port is opened."""


class FakeConnection:
    def __init__(self, chunks=()):
        self.chunks = list(chunks)
        self.writes = []

    def read(self, unused_size):
        return self.chunks.pop(0) if self.chunks else b""

    def write(self, data):
        self.writes.append(data)

    def close(self):
        pass

    def flush(self):
        pass

    def reset_input_buffer(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        return False


class Replies(FakeConnection):
    """One reply, with a writable pending buffer for console subclasses."""

    def __init__(self, data):
        super().__init__()
        self.data = data

    def read(self, unused_size):
        data, self.data = self.data, b""
        return data
