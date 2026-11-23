"""Runtime dependency checks for r3d tests that trace rays."""

import unittest

try:
    from r3d import mitsuba_reference

    HAVE_MITSUBA = mitsuba_reference.import_mitsuba() is not None
except ImportError:
    HAVE_MITSUBA = False


def have_llvm():
    if not HAVE_MITSUBA:
        return False
    import drjit as dr

    return "llvm_ad_rgb" in mitsuba_reference.import_mitsuba().variants() and dr.has_backend(dr.JitBackend.LLVM)


needs_mitsuba = unittest.skipIf(not HAVE_MITSUBA, "needs Mitsuba: pip install -r launcher/tools/r3d/requirements.txt")
needs_llvm = unittest.skipIf(not have_llvm(), "the LLVM variant needs libLLVM")
