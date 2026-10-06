"""Skips for the r3d tests that trace rays: numpy alone does not bring Mitsuba."""

import unittest

try:
    from r3d import mitsuba_reference

    HAVE_MITSUBA = mitsuba_reference.import_mitsuba() is not None
except ImportError:
    HAVE_MITSUBA = False

needs_mitsuba = unittest.skipIf(not HAVE_MITSUBA, "needs Mitsuba: pip install -r launcher/tools/r3d/requirements.txt")
