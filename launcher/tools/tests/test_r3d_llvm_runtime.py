"""Checks the Windows LLVM runtime: which library `select` names to Dr.Jit, that `install` keeps only a checked
archive's LLVM-C.dll, and, on Windows with it installed, that a real ray query loads that very file."""

import hashlib
import io
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import llvm_runtime, mitsuba_reference  # noqa: E402

try:
    from compression import zstd
except ImportError:
    zstd = None

TOOLS = pathlib.Path(__file__).resolve().parents[1]
LIBRARY_BYTES = b"not really a DLL"


def archive_with(members):
    """A .tar.zst like LLVM's release archive, holding `members` ({name: bytes})."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w") as tar:
        for name, data in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    return zstd.compress(raw.getvalue())


class SelectTest(unittest.TestCase):
    def setUp(self):
        self.root = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)

    def installed(self):
        path = llvm_runtime.library(self.root)
        path.parent.mkdir(parents=True)
        path.write_bytes(LIBRARY_BYTES)
        return path

    def test_an_installed_library_is_named_on_windows(self):
        path = self.installed()
        environ = {}
        self.assertEqual(llvm_runtime.select(environ, "win32", self.root), path)
        self.assertEqual(environ, {llvm_runtime.ENV: str(path)})

    def test_a_library_already_named_wins(self):
        self.installed()
        environ = {llvm_runtime.ENV: "elsewhere/LLVM-C.dll"}
        self.assertEqual(llvm_runtime.select(environ, "win32", self.root), pathlib.Path("elsewhere/LLVM-C.dll"))
        self.assertEqual(environ, {llvm_runtime.ENV: "elsewhere/LLVM-C.dll"})

    def test_nothing_is_named_without_an_install(self):
        environ = {}
        self.assertIsNone(llvm_runtime.select(environ, "win32", self.root))
        self.assertEqual(environ, {})

    def test_an_empty_name_counts_as_none(self):
        path = self.installed()
        environ = {llvm_runtime.ENV: ""}
        self.assertEqual(llvm_runtime.select(environ, "win32", self.root), path)

    def where(self, library):
        """`where`'s exit status and output on Windows with `library` as the cache's copy and nothing named."""
        with (mock.patch.object(llvm_runtime, "library", return_value=library),
              mock.patch.dict(os.environ, {}, clear=True),
              mock.patch.object(sys, "platform", "win32"),
              mock.patch("sys.stdout", new_callable=io.StringIO) as out):
            status = llvm_runtime.main(["where"])
            self.assertNotIn(llvm_runtime.ENV, os.environ)
        return status, out.getvalue().strip()

    def test_where_exits_by_whether_a_library_is_named(self):
        path = self.installed()
        self.assertEqual(self.where(path), (0, str(path)))
        status, said = self.where(self.root / "absent")
        self.assertEqual(status, 1)
        self.assertIn("none", said)

    def test_drjit_is_imported_after_the_library_is_named(self):
        order = []
        fake = type(sys)("mitsuba")
        with (mock.patch.object(mitsuba_reference.importlib.util, "find_spec", return_value=object()),
              mock.patch.object(mitsuba_reference.llvm_runtime, "select", side_effect=lambda: order.append("select")),
              mock.patch.object(mitsuba_reference.isa, "pin", side_effect=lambda: order.append("pin")),
              mock.patch.dict(sys.modules, {"mitsuba": fake})):
            self.assertIs(mitsuba_reference.import_mitsuba(), fake)
        self.assertEqual(order, ["select", "pin"])

    def test_other_systems_keep_their_own_libllvm(self):
        self.installed()
        environ = {}
        self.assertIsNone(llvm_runtime.select(environ, "linux", self.root))
        self.assertEqual(environ, {})


@unittest.skipIf(zstd is None, "needs Python 3.14's compression.zstd")
class InstallTest(unittest.TestCase):
    def setUp(self):
        self.root = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)

    def serve(self, data):
        """A file:// URL for `data` and its SHA-256."""
        path = self.root / "served.tar.zst"
        path.write_bytes(data)
        return path.as_uri(), hashlib.sha256(data).hexdigest()

    def test_only_the_library_is_kept(self):
        url, digest = self.serve(archive_with({"clang+llvm/bin/clang.exe": b"clang",
                                               "clang+llvm/bin/LLVM-C.dll": LIBRARY_BYTES}))
        path = llvm_runtime.install(self.root, url, digest)
        self.assertEqual(path, llvm_runtime.library(self.root))
        self.assertEqual(path.read_bytes(), LIBRARY_BYTES)
        self.assertEqual(sorted(p.name for p in path.parent.iterdir()), [llvm_runtime.LIBRARY])

    def test_an_archive_off_its_pin_installs_nothing(self):
        url, _ = self.serve(archive_with({"clang+llvm/bin/LLVM-C.dll": LIBRARY_BYTES}))
        with self.assertRaisesRegex(RuntimeError, "SHA-256"):
            llvm_runtime.install(self.root, url, "0" * 64)
        self.assertEqual(list(llvm_runtime.library(self.root).parent.iterdir()), [])

    def test_an_archive_without_the_library_is_refused(self):
        url, digest = self.serve(archive_with({"clang+llvm/bin/clang.exe": b"clang"}))
        with self.assertRaisesRegex(RuntimeError, "no bin/LLVM-C.dll"):
            llvm_runtime.install(self.root, url, digest)
        self.assertEqual(list(llvm_runtime.library(self.root).parent.iterdir()), [])

    def test_a_failed_download_leaves_nothing(self):
        with self.assertRaises(OSError):
            llvm_runtime.install(self.root, (self.root / "absent.tar.zst").as_uri(), "0" * 64)
        self.assertEqual(list(llvm_runtime.library(self.root).parent.iterdir()), [])

    def test_an_installed_library_is_not_fetched_again(self):
        path = llvm_runtime.library(self.root)
        path.parent.mkdir(parents=True)
        path.write_bytes(LIBRARY_BYTES)
        self.assertEqual(llvm_runtime.install(self.root, (self.root / "absent").as_uri(), "0" * 64), path)


# A fresh process, so this suite's own imports cannot have loaded another LLVM first: one ray down onto a triangle
# on the LLVM variant, then the file the loaded LLVM-C.dll module came from.
TRACE = """
import ctypes, sys
sys.path.insert(0, sys.argv[1])
from r3d.ray_query import RayQuery
query = RayQuery([[-1, -1, 0], [1, -1, 0], [0, 1, 0]], [[0, 1, 2]], variant="llvm_ad_rgb")
hit, distance, _ = query.first_hits([[0, 0, 1], [5, 5, 1]], [[0, 0, -1], [0, 0, -1]])
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.GetModuleHandleW.restype = ctypes.c_void_p
name = ctypes.create_unicode_buffer(32768)
kernel.GetModuleFileNameW(ctypes.c_void_p(kernel.GetModuleHandleW("LLVM-C.dll")), name, len(name))
print(hit.tolist(), round(float(distance[0]), 4), name.value, sep="\\n")
"""


def installed_on_windows():
    return sys.platform == "win32" and llvm_runtime.library().is_file()


@unittest.skipUnless(installed_on_windows(), "needs Windows with `llvm_runtime.py install` run")
class WindowsRuntimeTest(unittest.TestCase):
    def test_a_ray_query_traces_on_the_installed_library(self):
        environ = {k: v for k, v in os.environ.items() if k != llvm_runtime.ENV}
        result = subprocess.run([sys.executable, "-c", TRACE, str(TOOLS)], capture_output=True, text=True,
                                env=environ, timeout=300)
        self.assertEqual(result.returncode, 0, result.stderr)
        hit, distance, loaded = result.stdout.strip().splitlines()[-3:]
        self.assertEqual(hit, "[True, False]")
        self.assertEqual(float(distance), 1.0)
        self.assertEqual(os.path.normcase(loaded), os.path.normcase(str(llvm_runtime.library())))


if __name__ == "__main__":
    unittest.main()
