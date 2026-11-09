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
# A material is a card candidate when at least this share of its triangles is mostly see-through (mean alpha under
# keep_alpha): cut-out leaves sit near half (Sponza leaf 48%), a solid shell with fringed texture near none (Bistro
# hedges 1%).
CARD_SEE_THROUGH_MIN = 1 / 3
# A candidate is carded only when its cards cover at least this share of its opaque surface (area-weighted samples
# within a card's slab and on a set mask texel); otherwise it stays fitted geometry. Between Sponza leaf (84%) and
# the best covered of Bistro's rejected materials (flowers 47%).
CARD_COVERED_MIN = 2 / 3
