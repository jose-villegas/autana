"""Skips for the r3d tests that trace rays: numpy alone does not bring Mitsuba."""

import ctypes
import sys
import unittest

# Windows LLVM's C API library crashes the process if it is unloaded before exit, which Dr.Jit does at shutdown.
LLVM_LIBRARY = "LLVM-C.dll"
GET_MODULE_HANDLE_EX_FLAG_PIN = 0x1


def pin_llvm():
    """Keeps Dr.Jit's LLVM loaded until the test process ends, on Windows and once Dr.Jit has loaded it. Returns
    whether it is pinned."""
    if sys.platform != "win32":
        return False
    module = ctypes.c_void_p()
    get_module = ctypes.windll.kernel32.GetModuleHandleExW
    return bool(get_module(GET_MODULE_HANDLE_EX_FLAG_PIN, LLVM_LIBRARY, ctypes.byref(module)))


try:
    from r3d import mitsuba_reference

    HAVE_MITSUBA = mitsuba_reference.import_mitsuba() is not None
    pin_llvm()
except ImportError:
    HAVE_MITSUBA = False

needs_mitsuba = unittest.skipIf(not HAVE_MITSUBA, "needs Mitsuba: pip install -r launcher/tools/r3d/requirements.txt")
