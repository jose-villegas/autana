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

FLOOR_SHARE = 0.05
# The simplifier takes a float32 error.
ERROR_RESOLUTION = np.finfo(np.float32).eps
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
    def _measured_curve(self):
        """The best measured count available at each error in the piece's grid."""
        local = self.positions - self.positions[0]
        diagonal = bounds_diagonal(self.positions)
        errors = [0.0]
        if diagonal > 0:
            errors.extend(diagonal * np.geomspace(ERROR_RESOLUTION, FLOOR_SHARE, MEASURED_GRID_SIZE))
        counts = [len(level(local, self.triangles, error)) for error in errors]
        return errors, np.minimum.accumulate(counts)


@dataclass(frozen=True)
class Allocation:
    """The selected error and each piece's nominal triangle count."""

    error: float
    counts: dict[str, int]


def piece_error(piece, error) -> float:
    """Maximum surface error in source units, capped by the piece's own bounds."""
    return min(error, FLOOR_SHARE * bounds_diagonal(piece.positions))


def piece_count(piece, error) -> int:
    """Count a prototype or an entire source object at its capped error."""
    error = piece_error(piece, error)
    if piece.measured:
        errors, counts = piece._measured_curve
        return int(counts[bisect_right(errors, error) - 1])
    local = piece.positions - piece.positions[0]
    return len(simplify_to_error(local, piece.triangles, error, REST_OPTIONS))


def allocate(pieces, drawn, stored, fixed_drawn=0, fixed_stored=0) -> Allocation:
    """Find the finest shared error that fits both budgets."""
    return allocate_with_costs(pieces, drawn, stored, fixed_drawn, fixed_stored, {})


def allocate_with_costs(pieces, drawn, stored, fixed_drawn, fixed_stored, costs):
    """Allow for measured stand-in growth and additional stored meshes."""
    pieces = tuple(pieces)
    floors = {piece.name: piece_error(piece, math.inf) for piece in pieces}
    capped = {}

    def trial(error):
        counts = {}
        for piece in pieces:
            if error < floors[piece.name]:
                counts[piece.name] = piece_count(piece, error)
            else:
                if piece.name not in capped:
                    capped[piece.name] = piece_count(piece, error)
                counts[piece.name] = capped[piece.name]
        actual_drawn, actual_stored = fixed_drawn, fixed_stored
        for piece in pieces:
            count = counts[piece.name]
            extra, stored_meshes = costs.get(piece.name, (0, 1))
            actual_drawn += piece.copies * (count + extra)
            actual_stored += stored_meshes * (count + extra)
        return Allocation(error, counts), actual_drawn, actual_stored

    def fits(result):
        return result[1] <= drawn and (stored is None or result[2] <= stored)

    zero = trial(0.0)
    if fits(zero):
        return zero[0]
    high = max(floors.values(), default=0.0)
    best = trial(high)
    if not fits(best):
        raise SettingsError(f"shape floors need {best[1]} drawn triangles (budget {drawn}) and "
                            f"{best[2]} stored triangles (ceiling {stored})")

    # At zero a log bracket has no lower end. Halve until the first failing trial.
    low = high / 2
    while low > 0:
        result = trial(low)
        if not fits(result):
            break
        high, best = low, result
        low /= 2
    while high - low > high * ERROR_RESOLUTION:
        middle = math.sqrt(low) * math.sqrt(high) if low > 0 else high / 2
        result = trial(middle)
        if fits(result):
            high, best = middle, result
        else:
            low = middle
    return best[0]
