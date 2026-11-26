"""Step shortening for triangle orientation constraints."""


def shorten_step(candidate, previous, turned, corners, rounds):
    """Shorten a NumPy or torch step in place and return any rows that cannot be repaired.

    turned(candidate, changed) returns all currently inverted row indices.
    changed is None for the first check and the indices just edited thereafter.
    A partial check must also keep any inverted rows unaffected by the edit. corners(rows) returns
    unique indices of the movable corners (or other points driving those rows).
    After the halving budget, restore offending moves exactly, including any
    neighbors that turn as a result. previous must be independent of candidate.
    """
    flipped = turned(candidate, None)
    for _ in range(rounds):
        if not len(flipped):
            return flipped
        indices = corners(flipped)
        if not len(indices):
            return flipped
        candidate[indices] = (candidate[indices] + previous[indices]) / 2
        flipped = turned(candidate, indices)

    while len(flipped):
        indices = corners(flipped)
        indices = indices[(candidate[indices] != previous[indices]).any(1)]
        if not len(indices):
            break
        candidate[indices] = previous[indices]
        flipped = turned(candidate, indices)
    return flipped
