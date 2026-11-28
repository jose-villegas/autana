"""Mocks for tests of the appearance fit pipeline."""


def stub_seal(seal):
    """Configure a seal mock to preserve each mesh and report zero changes."""
    seal.snap_back.side_effect = lambda start, fitted, copies, probes: (fitted, 0, 0, 0, 0, 0)
    seal.split_t_junctions.side_effect = lambda mesh: (mesh, 0)
    return seal
