"""Runtime dependency checks for r3d tests that trace rays."""

import atexit
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
    # Dr.Jit loads LLVM on its first traced call, often after this import: pin it again at exit, which runs
    # before Dr.Jit's own exit handler since that one was registered first.
    pin_llvm()
    atexit.register(pin_llvm)
except ImportError:
    HAVE_MITSUBA = False


def have_llvm():
    if not HAVE_MITSUBA or not mitsuba_reference.LLVM_TRACES:
        return False
    import drjit as dr

    return "llvm_ad_rgb" in mitsuba_reference.import_mitsuba().variants() and dr.has_backend(dr.JitBackend.LLVM)


# Ray queries and the bake's light run as Dr.Jit kernels: on the LLVM variant in tests, which needs libLLVM.
needs_llvm = unittest.skipIf(not have_llvm(), "ray queries run on the LLVM variant: pip install -r "
                                               "launcher/tools/r3d/requirements.txt and libLLVM (not on Windows, "
                                               "whose host LLVM is unpinned and fails to link them: autana-dz1nq)")
