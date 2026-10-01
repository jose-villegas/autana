"""The lit mesh pack entry's layout, which main/render/r3d_lit_mesh.c checks the
C structs against: the entry type, then a header of counts, the position scale
and the offset of each array from the entry's first byte (0 for an array the
mesh has none of), then the cluster and node rows. Standard library only, so
the pack builder runs without the numeric environment."""

import struct

TYPE = b"LMSH"
BLOB_HEADER = struct.Struct("<11I")
CLUSTER = struct.Struct("<4H6hBx")
NODE = struct.Struct("<6hHBB")
