"""3-vector and quaternion maths on plain tuples, for tools that need no NumPy.

A quaternion is (x, y, z, w), glTF's order. Standard library only.
"""

import math


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def unit(v):
    """`v` scaled to length 1; a zero vector raises ZeroDivisionError."""
    length = math.sqrt(sum(c * c for c in v))
    return tuple(c / length for c in v)


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def qrot(q, v):
    """`v` rotated by the unit quaternion `q`."""
    return qmul(qmul(q, (v[0], v[1], v[2], 0.0)), (-q[0], -q[1], -q[2], q[3]))[:3]
