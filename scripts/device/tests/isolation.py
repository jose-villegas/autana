"""Keeps the device and autana test suites out of the real device records.

Every test module under scripts/device/tests and scripts/autana/tests
imports this before anything else. Once per test process it points the
records root (AUTANA_RECORDS) and the lock root (AUTANA_DEVICE_LOCK_ROOT) at
one temporary directory, which child processes inherit, and drops an
inherited AUTANA_LOCK_HOOK so no test notifies anybody. An audit hook then
refuses any write this process makes under the roots it replaced, and the
run exits non-zero if one was attempted, even where the code under test
swallowed the error.
"""

import atexit
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
    for name in ("AUTANA_RECORDS", "AUTANA_DEVICE_LOCK_ROOT"):
        if os.environ.get(name):
            roots.append(Path(os.environ[name]))
    return [root.resolve() for root in roots]


REAL_ROOTS = replaced_roots()
os.environ["AUTANA_RECORDS"] = str(TEMP / "records")
os.environ["AUTANA_DEVICE_LOCK_ROOT"] = str(TEMP / "locks")
os.environ.pop("AUTANA_LOCK_HOOK", None)
os.environ.pop("AUTANA_BOARD", None)

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


def finish():
    shutil.rmtree(TEMP, ignore_errors=True)
    if violations:
        sys.stderr.write("FAIL: the test run wrote into real device roots:\n  " +
                         "\n  ".join(sorted(set(violations))) + "\n")
        sys.stderr.flush()
        os._exit(1)


sys.addaudithook(refuse_real_roots)
atexit.register(finish)
