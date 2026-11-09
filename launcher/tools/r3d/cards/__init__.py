"""Billboard-cloud import settings and limits."""

# Model units until import scale is normalised; both defaults are provisional pending a sweep.
CARD_EPSILON = 5.0
CARD_AREA_COST = 0.03
# Provisional model-unit bound for small close-range affine UV error; tune from closest-card render_compare crops.
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
