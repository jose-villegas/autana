"""Query the closest point on a mesh for the part shape measure."""
import numpy as np


class PartSurface:
    """A libigl triangle AABB queried in float64."""

    def __init__(self, positions, triangles):
        try:
            from igl import AABB
        except ImportError as error:
            raise RuntimeError("install the scorecard dependencies with python -m pip install "
                               "-r launcher/tools/render/scorecard/requirements.txt") from error

        self.positions = np.ascontiguousarray(positions, dtype=np.float64)
        self.triangles = np.ascontiguousarray(triangles, dtype=np.int64)
        self.index = AABB()
        self.index.init(self.positions, self.triangles)

    def distances(self, points):
        """One distance per sample, including empty batches."""
        if not len(points):
            return np.empty(0)
        squared, _, _ = self.index.squared_distance(self.positions, self.triangles, points)
        return np.sqrt(squared)
