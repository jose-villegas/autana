"""Mocks for tests of the appearance fit pipeline."""


def stub_seal(seal):
    """Configure a seal mock to preserve each mesh and report zero changes."""
    def snap_back(start, fitted, copies, probes, *, record):
        record.rounds = 0
        return fitted, 0, 0

    seal.snap_back.side_effect = snap_back
    seal.split_t_junctions.side_effect = lambda mesh: (mesh, 0)
    return seal
