"""Allocate drawn and stored triangles by a shared shape error."""

import math
from bisect import bisect_right
from dataclasses import dataclass
from functools import cached_property

import numpy as np

from r3d.geometry import bounds_diagonal
from r3d.import_settings import SettingsError
from r3d.meshopt import ERROR_ABSOLUTE, PRUNE, simplify_to_error
from r3d.objects.detail import level

# A global budget cannot spend more than this share of an individual piece's size.
FLOOR_SHARE = 0.05
# The simplifier takes a float32 error.
ERROR_RESOLUTION = np.finfo(np.float32).eps
BISECTION_TOLERANCE = 1e-3
REST_OPTIONS = ERROR_ABSOLUTE | PRUNE
MEASURED_GRID_STEP = math.sqrt(2)
MEASURED_GRID_SIZE = 1 + math.ceil(math.log(FLOOR_SHARE / ERROR_RESOLUTION, MEASURED_GRID_STEP))


@dataclass(frozen=True)
class Piece:
    """A source object or prototype with welded positions in source units."""

    name: str
    positions: np.ndarray
    triangles: np.ndarray
    copies: int
    measured: bool

    @cached_property
    def diagonal(self):
        return bounds_diagonal(self.positions)

    @cached_property
    def floor(self):
        return FLOOR_SHARE * self.diagonal

    @cached_property
    def floor_count(self):
        return piece_count(self, self.floor)

    @cached_property
    def _measured_curve(self):
        """The best measured count available at each error in the piece's grid."""
        local = self.positions - self.positions[0]
        diagonal = self.diagonal
        errors = [0.0]
        if diagonal > 0:
            errors.extend(diagonal * np.geomspace(ERROR_RESOLUTION, FLOOR_SHARE, MEASURED_GRID_SIZE))
        counts = [len(level(local, self.triangles, error)) for error in errors]
        return errors, np.minimum.accumulate(counts)


@dataclass(frozen=True)
class SharedError:
    """The selected error and each piece's nominal triangle count."""

    error: float
    counts: dict[str, int]


def piece_error(piece, error) -> float:
    """Maximum surface error in source units, capped by the piece's own bounds."""
    return min(error, piece.floor)


def piece_count(piece, error) -> int:
    """Count a prototype or an entire source object at its capped error."""
    error = piece_error(piece, error)
    if piece.measured:
        errors, counts = piece._measured_curve
        return int(counts[bisect_right(errors, error) - 1])
    local = piece.positions - piece.positions[0]
    return len(simplify_to_error(local, piece.triangles, error, REST_OPTIONS))


def within(drawn, stored, triangles, ceiling) -> bool:
    """Whether the drawn and stored totals both fit."""
    return (triangles is None or drawn <= triangles) and (ceiling is None or stored <= ceiling)


def check(drawn, stored, triangles, ceiling):
    """Report both totals and limits when either budget is exceeded."""
    if not within(drawn, stored, triangles, ceiling):
        raise SettingsError(f"scene emitted {drawn} drawn triangles (budget {triangles}) and "
                            f"{stored} stored triangles (ceiling {ceiling})")


def allocate(pieces, drawn, stored, fixed=0, costs=None) -> SharedError:
    """Find the finest shared error that fits, or the loosest if estimated costs overrun."""
    pieces = tuple(pieces)
    estimated = costs is not None
    costs = {} if costs is None else costs

    def trial(error, full=False):
        counts = {}
        actual_drawn = actual_stored = fixed
        for piece in pieces:
            if full:
                count = len(piece.triangles)
            elif error >= piece.floor:
                count = piece.floor_count
            else:
                count = piece_count(piece, error)
            counts[piece.name] = count
            extra, stored_meshes = costs.get(piece.name, (0, 1))
            actual_drawn += piece.copies * (count + extra)
            actual_stored += stored_meshes * (count + extra)
        return SharedError(error, counts), actual_drawn, actual_stored

    def fits(result):
        return within(result[1], result[2], drawn, stored)

    below = trial(0.0, full=True)
    if fits(below):
        return below[0]
    high = max((piece.floor for piece in pieces), default=0.0)
    best = trial(high)
    if not fits(best):
        if not estimated:
            check(best[1], best[2], drawn, stored)
        return best[0]

    # At zero a log bracket has no lower end.
    minimum = ERROR_RESOLUTION * min((piece.diagonal for piece in pieces if piece.diagonal > 0), default=0.0)
    low = high / 2
    while low > minimum:
        result = trial(low)
        if not fits(result):
            below = result
            break
        high, best = low, result
        low /= 2
    else:
        low = 0.0
    while (high - low > max(high * BISECTION_TOLERANCE, minimum)
           and below[0].counts != best[0].counts):
        middle = math.sqrt(low * high) if low > 0 else high / 2
        result = trial(middle)
        if fits(result):
            high, best = middle, result
        else:
            low, below = middle, result
    return best[0]
