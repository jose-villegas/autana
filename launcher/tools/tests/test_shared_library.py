"""native/shared_library.py: a process that cannot replace a library another process has loaded loads that one."""

import pathlib
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from native import shared_library  # noqa: E402


class BuildSharedTests(unittest.TestCase):
    def build(self, cache, replace_error):
        """build_shared of one source into `cache`, the compiler writing its output, os.replace raising
        `replace_error`; returns what it loaded."""
        source = cache.parent / "lib.c"
        source.write_text("int one(void) { return 1; }\n")

        def compile(command, check):
            pathlib.Path(command[command.index("-o") + 1]).write_bytes(b"ours")

        with mock.patch.object(shared_library.subprocess, "run", side_effect=compile), \
                mock.patch.object(shared_library.os, "replace", side_effect=replace_error), \
                mock.patch.object(shared_library.ctypes, "CDLL", side_effect=lambda path, mode: path):
            return shared_library.build_shared("lib", cache, (source,), "cc", log=lambda line: None)

    def test_a_library_another_process_holds_is_loaded_and_this_copy_dropped(self):
        with tempfile.TemporaryDirectory() as folder:
            cache = pathlib.Path(folder) / ".cache"

            def held(building, path):
                path.write_bytes(b"theirs")  # the other process put it in place and loaded it
                raise PermissionError("in use")

            loaded = pathlib.Path(self.build(cache, held))
            self.assertEqual(loaded.read_bytes(), b"theirs")
            self.assertEqual([path.name for path in cache.iterdir()], [loaded.name], "our copy is dropped")

    def test_a_permission_error_with_no_library_in_place_still_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(PermissionError):
                self.build(pathlib.Path(folder) / ".cache", PermissionError("denied"))


if __name__ == "__main__":
    unittest.main()
