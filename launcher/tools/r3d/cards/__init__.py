"""Billboard-cloud import settings and limits."""

# Slab half-depth in source units.
CARD_EPSILON = 5.0
# Score penalty per square source unit of card area.
CARD_AREA_COST = 0.03
# Maximum long side in source units, bounding affine UV error at close range.
CARD_MAX_SIDE = 128.0
# Surface samples drawn per source triangle.
SAMPLES_PER_TRIANGLE = 6
# Maximum spatial patches from which planes are proposed.
CANDIDATE_PATCHES = 400
# Minimum samples per candidate patch.
CANDIDATE_MIN_SAMPLES = 20
# Neighbourhood radius in source units for scoring candidate planes.
CARD_RADIUS = 90.0
# Maximum planes selected per material.
CARD_LIMIT = 256
# Score multiplier for projected area just outside the slab.
W_PENALTY = 10.0
# Per-card score floor in squared slab half-depths.
CARD_FLOOR_EPS2 = 16.0
# Minimum share of a material's surface samples its alpha test cuts, for automatic selection.
CARD_SEE_THROUGH_MIN = 1 / 3
# Probe weight's distance floor, in probe spacings: a sample nearer a probe weighs as one this far away.
PROBE_WEIGHT_FLOOR = 0.5
# Minimum probe-weighted share of opaque samples covered by baked slabs, rectangles and mask texels.
CARD_COVERED_MIN = 2 / 3
# Texels along the median parent card's long side.
CARD_MASK_TEXELS = 32
# Stored MASK1 levels, including level zero.
CARD_MIP_LEVELS = 3
