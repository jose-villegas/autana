"""Keeps the device and autana test suites out of the real device records.

Every test module under scripts/device/tests and scripts/autana/tests
imports this before anything else. Once per test process it makes one
temporary project whose autana.local.toml sends the records to a temporary
folder and names it the project every child process acts on, and points the
lock root (_AUTANA_DEVICE_LOCK_ROOT, test only) and the system temporary
folder, where flash snapshots are made, at temporary directories too. A
project with no lock hook notifies nobody. An audit hook then refuses any
write this process makes under the roots it replaced, and the run exits
non-zero if one was attempted, even where the code under test swallowed the
error - or if a snapshot folder outlived the run.
"""

import atexit
import contextlib
import os
import shutil
import sys
import tempfile
import threading
from pathlib import Path

CHECKOUT = Path(__file__).resolve().parents[3]
TEMP = Path(tempfile.mkdtemp(prefix="autana-device-tests-"))


def replaced_roots():
    roots = [CHECKOUT / ".records" / "device", Path(tempfile.gettempdir()) / "autana-device"]
    if hasattr(os, "getuid"):
        roots.append(Path("/tmp") / f"autana-device-{os.getuid()}")
    if os.environ.get("_AUTANA_DEVICE_LOCK_ROOT"):
        roots.append(Path(os.environ["_AUTANA_DEVICE_LOCK_ROOT"]))
    # The records a real run of this checkout, or the project this process
    # was started for, would write to.
    sys.path.insert(0, str(CHECKOUT / "scripts" / "lib"))
    import autana_config
    for real in (os.environ.get("_AUTANA_PROJECT"), CHECKOUT):
        try:
            named = autana_config.load(real).get("records") if real else None
        except autana_config.ConfigError:
            named = None
        if named:
            roots.append(autana_config.path_value(named, real))
    return [root.resolve() for root in roots]


REAL_ROOTS = replaced_roots()


def write_config(project, **settings):
    """`project`'s autana.local.toml holding `settings` ({"lock_hook": "cmd"})."""
    def quoted(text):
        return '"' + str(text).replace("\\", "\\\\").replace('"', '\\"') + '"'

    lines = []
    for key, value in settings.items():
        text = ("[" + ", ".join(quoted(item) for item in value) + "]"
                if isinstance(value, (list, tuple)) else quoted(value))
        lines.append(f"{key} = {text}")
    Path(project).mkdir(parents=True, exist_ok=True)
    (Path(project) / "autana.local.toml").write_text("\n".join(lines) + "\n", encoding="utf-8")


@contextlib.contextmanager
def project(**settings):
    """A fresh project folder holding `settings` as its autana.local.toml -
    no file at all when there are none - that every autana script in this
    process and its children treats as the project while the block runs."""
    with tempfile.TemporaryDirectory(dir=TEMP) as directory:
        if settings:
            write_config(directory, **settings)
        previous = os.environ["_AUTANA_PROJECT"]
        os.environ["_AUTANA_PROJECT"] = directory
        try:
            yield Path(directory)
        finally:
            os.environ["_AUTANA_PROJECT"] = previous


RECORDS = TEMP / "records"
BASE_PROJECT = TEMP / "project"
write_config(BASE_PROJECT, records=str(RECORDS))
os.environ["_AUTANA_PROJECT"] = str(BASE_PROJECT)
os.environ["_AUTANA_DEVICE_LOCK_ROOT"] = str(TEMP / "locks")
# In-process autana derives its project from the cwd, so tests run in the
# isolated one.
os.chdir(BASE_PROJECT)
IMAGES = TEMP / "images"
IMAGES.mkdir()
# Flash snapshots are made under the system's temporary folder, which is
# where the leftover check looks; children inherit the variables.
tempfile.tempdir = str(IMAGES)
for name in ("TMPDIR", "TEMP", "TMP"):
    os.environ[name] = str(IMAGES)
os.environ.pop("_AUTANA_BOARD", None)

violations = []
checking = threading.local()


def written_paths(event, args):
    if event == "open":
        path, mode, flags = args
        writing = (any(letter in mode for letter in "wax+") if isinstance(mode, str)
                   else bool(flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_APPEND)))
        return [path] if writing else []
    if event in ("os.remove", "os.rmdir", "os.mkdir", "os.truncate", "shutil.rmtree"):
        return [args[0]]
    if event == "os.rename":
        return [args[0], args[1]]
    return []


def refuse_real_roots(event, args):
    if getattr(checking, "active", False):
        return
    checking.active = True
    try:
        for path in written_paths(event, args):
            if path is None or isinstance(path, int):
                continue
            resolved = Path(os.fsdecode(path)).resolve()
            if any(resolved == root or root in resolved.parents for root in REAL_ROOTS):
                violations.append(str(resolved))
                raise PermissionError("a test wrote into a real device root: " + str(resolved))
    finally:
        checking.active = False


def leftover_snapshots():
    return sorted(path.name for path in IMAGES.glob("autana-image-*"))


def finish():
    left = leftover_snapshots()
    shutil.rmtree(TEMP, ignore_errors=True)
    if left:
        sys.stderr.write("FAIL: the test run left snapshot folders behind:\n  " +
                         "\n  ".join(left) + "\n")
        sys.stderr.flush()
        os._exit(1)
    if violations:
        sys.stderr.write("FAIL: the test run wrote into real device roots:\n  " +
                         "\n  ".join(sorted(set(violations))) + "\n")
        sys.stderr.flush()
        os._exit(1)


sys.addaudithook(refuse_real_roots)
atexit.register(finish)
