"""Checks that Mitsuba's CPU backend traces the same bits on every x86-64 Linux host of one vendor (r3d/isa): Dr.Jit
compiles for x86-64-v3 at 8 lanes, Embree runs capped at AVX2, and the hit distances of a fixed triangle soup and the
bounced light of a fixed corridor hash to what every pinned host of that vendor makes. Unpinned, an AVX-512 host moves
both by an ulp; AMD and Intel differ because AVX2's reciprocal estimates are the vendor's own."""

import hashlib
import pathlib
import platform
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

try:
    import numpy as np

    from r3d import isa, mitsuba_reference, ray_query
    from r3d.ray_query import RayQuery
    from tests.test_r3d_path_bake import SUN, corridor, have_llvm, path_light
except ImportError:
    np = None

from tests.r3d_env import HAVE_MITSUBA  # noqa: E402

PINNED = HAVE_MITSUBA and sys.platform == "linux" and platform.machine() == "x86_64" and isa.pin()
needs_pin = unittest.skipIf(not (PINNED and have_llvm()), "the pin needs Linux on x86-64 with AVX2, FMA and libLLVM")

# (hits, bounce) digests a pinned host of each vendor makes; a change to Mitsuba, Dr.Jit or these scenes changes them:
# rerun on an x86-64 Linux host of each vendor and record the digests its failure prints.
PINNED_SHA256 = {
    "AuthenticAMD": ("9964d47fe885de30264b20514999d430405205570e324d60534c2b403e75245f",
                     "81e5826f8e3441459cad0d3e1589a3e8081b8474dee550271f22359f1239ae14"),
    "GenuineIntel": ("6e829de472b2bbba8a9b90115f072a73b3f8fa6e44f9476060ef34f8facc7487",
                     "c7032957b091c7ea5737a32bdd4064401bc9ebd59c6a013878daf9f91d786d2b"),
}


def vendor():
    """The CPU vendor /proc/cpuinfo names, None when it names none."""
    for line in pathlib.Path("/proc/cpuinfo").read_text().splitlines():
        if line.startswith("vendor_id"):
            return line.split(":", 1)[1].strip()
    return None


def digest(*arrays):
    h = hashlib.sha256()
    for array in arrays:
        h.update(np.ascontiguousarray(array).tobytes())
    return h.hexdigest()


def soup_hits():
    """(hit, distance, triangle) of 200000 random rays through 5000 random triangles: grazing hits included."""
    rng = np.random.default_rng(5)
    positions, tris = rng.random((3000, 3)) * 100.0, rng.integers(0, 3000, (5000, 3))
    origins = rng.random((200000, 3)) * 100.0
    directions = rng.normal(size=(200000, 3))
    directions /= np.linalg.norm(directions, axis=1, keepdims=True)
    hit, distance, tri = RayQuery(positions, tris, variant="llvm_ad_rgb").first_hits(origins, directions)
    return hit, np.where(hit, distance, 0.0), np.where(hit, tri, -1)


def corridor_bounce():
    """The bounced light at a grid of floor points of the corridor, two bounces of 64 rays each."""
    x, z = np.meshgrid(np.linspace(10.0, 790.0, 40), np.linspace(-390.0, 390.0, 40))
    points = np.stack([x.ravel(), np.zeros(x.size), z.ravel()], axis=1)
    normals = np.tile([0.0, 1.0, 0.0], (len(points), 1))
    with mock.patch.object(ray_query, "VARIANT", "llvm_ad_rgb"):
        return path_light(corridor(), bounces=2, rays=64, lights=(SUN,)).bounce(points, normals, 0.5)


@unittest.skipIf(np is None, "needs numpy")
@needs_pin
class PinnedIsa(unittest.TestCase):
    def test_jit_compiles_for_the_pinned_target(self):
        mitsuba_reference.import_mitsuba()
        self.assertEqual(isa.jit_target(), (isa.JIT_CPU, isa.JIT_LANES))

    def test_embree_is_capped(self):
        soup_hits()
        self.assertTrue(isa.embree_config().endswith("," + isa.EMBREE_CAP.decode()), isa.embree_config())

    def pinned_sha256(self):
        if vendor() not in PINNED_SHA256:
            self.skipTest(f"no digests recorded for {vendor()}")
        return PINNED_SHA256[vendor()]

    def test_hit_distances_are_the_pinned_bits(self):
        self.assertEqual(digest(*soup_hits()), self.pinned_sha256()[0])

    def test_bounced_light_is_the_pinned_bits(self):
        self.assertEqual(digest(corridor_bounce()), self.pinned_sha256()[1])

    def test_pinning_after_mitsuba_is_refused(self):
        with mock.patch.object(isa, "_state", None), self.assertRaisesRegex(RuntimeError, "before mitsuba"):
            isa.pin()


if __name__ == "__main__":
    unittest.main()
