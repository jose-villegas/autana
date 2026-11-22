"""Round rendering values to the representation stored in float32 assets."""

from array import array


def f32(value):
    """Return value rounded to the nearest float32."""
    return array("f", [value])[0]
