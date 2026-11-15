"""Billboard-cloud import settings and limits."""

# Plane distances are in source units.
CARD_EPSILON = 5.0
CARD_AREA_COST = 0.08
# Splitting bounds affine UV error at close range.
CARD_MAX_SIDE = 128.0
SAMPLES_PER_TRIANGLE = 6
CANDIDATE_PATCHES = 400
CANDIDATE_MIN_SAMPLES = 20
CARD_RADIUS = 90.0
CARD_LIMIT = 256
W_PENALTY = 10.0
CARD_FLOOR_EPS2 = 16.0
# Cut-out foliage has many mostly see-through triangles; a solid shell with a fringed texture has few.
CARD_SEE_THROUGH_MIN = 1 / 3
# Area-weighted opaque samples must land within baked card slabs and on set mask texels.
CARD_COVERED_MIN = 2 / 3

CARD_MASK_TEXELS = 32
CARD_MIP_LEVELS = 3
CARD_PROBE_NEAR_SHARE = 1e-3
