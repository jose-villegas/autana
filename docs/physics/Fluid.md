# Fluid

Water in a closed box, stepped by any acceleration: `physics/fluid/`, in the
[physics package](../../launcher/packages/physics/README.md).

## flip3.h

Water in a closed box: FLIP particles on a MAC grid<sup>[[91]](../Citations.md#harlow-1965)</sup><sup>[[92]](../Citations.md#brackbill-1986)</sup>,
driven by any acceleration, in caller-owned grid and particle blocks.

| Function | Purpose |
|---|---|
| `flip3_config_t` | Box size, nominal cell and particle capacity |
| `flip3_tuning_t` | FLIP share, pressure sweeps, over-relaxation, drift gain, red-black order for a pressure solve split across two cores, and the speed a particle outside the surface needs to show in the field |
| `flip3_t` | Grid, particles, cell and seed spacing per axis, rest density |
| `flip3_grid_bytes`, `flip3_particle_bytes` | The two blocks a configuration needs; 0 when refused |
| `flip3_init` | Carves the blocks; empty, at the default tuning |
| `flip3_fill` | Refills a share of the volume at rest, lowest first along a down vector |
| `flip3_step` | Advances one substep under an acceleration |
| `flip3_time_stages` | Adds each step stage's microseconds, by a caller's clock, to `FLIP3_STAGE_COUNT` sums; `flip3_stage_names` names them |
| `flip3_split_with` | Hands each particle pass and `flip3_field`'s work to a caller's split as a range of items; the split may cut it anywhere and run the parts on two cores, with the same results as one core |
| `flip3_peak_speed_sq` | The fastest particle's speed squared, 0 at rest |
| `flip3_fraction` | The share of the volume the particles stand for |
| `flip3_particle` | A particle in box coordinates |
| `flip3_field_count` | The floats a surface field at that many samples per cell needs; the rate may be fractional, each axis rounding its own count |
| `flip3_field` | The surface field: density over the box, 1 at rest, smoothed once with the walls mirrored and padded with empty samples, then every particle put inside the surface; returns the `iso_field_t` that `iso_surface_nets()` meshes ([Geometry](../math/Geometry.md#iso_surface)) |

| Constant | Purpose |
|---|---|
| `FLIP3_RADIUS_CELLS` | Closest a particle comes to a wall |
| `FLIP3_SURFACE_LEVEL` | The field value the surface is at: half the rest density |
| `FLIP3_SEED_SPACING_CELLS` | Seed lattice spacing; a cell holds about 4.6 seeds |
| `FLIP3_FLIP_RATIO`, `FLIP3_PRESSURE_ITERS`, `FLIP3_OVER_RELAXATION` | Defaults of the Ten Minute Physics tank<sup>[[94]](../Citations.md#muller-2022)</sup> |
| `FLIP3_DRIFT_GAIN` | Share of a cell's density error turned into flow per step |
| `FLIP3_SHOW_SPEED` | 0: the field shows every particle outside the surface, since a speed depends on the length unit |
| `FLIP3_TUNING_DEFAULT` | All six together, index order |

### The step

Each step runs in Bridson's order<sup>[[93]](../Citations.md#bridson-2015)</sup>:

| Stage | What it does |
|---|---|
| Particles to grid | Trilinear splat of each velocity component onto its faces, and the density at the cell centres, each its own item of a split; cells holding a particle are water |
| Acceleration | Added to every face not on a wall |
| Pressure | Gauss-Seidel on the divergence (red-black when `red_black` is set), cells of any aspect, plus the drift push |
| Grid to particles | PIC and FLIP blended by the FLIP share |
| Move | Particles advect with the projected velocity, then stop a radius from each wall |

The cell is rounded per axis so whole cells fill the box, so no wall cell
leaks pressure as a false free surface. The drift push turns a cell's
density error $`\rho-\rho_0`$ into flow: always when the cell is packed,
and also when thin if water surrounds it, since a divergence-free field never
closes a gap and a resting pool would slowly turn to foam. The Ten Minute
Physics tank instead moves particles before the pressure solve and pushes
overlapping pairs apart; that sinks a pool by $`a\Delta t^2`$ each step and
holds heaps up the way grains do, so flip3 does neither.

The surface field averages a film under half a sample thick below the
level, so each particle whose nearest sample reads below it, and that moves
at least the tuning's `show_speed` fast, then raises the
samples of its field cell to 1 less their distance in samples, the nearest
to at least 0.75, so even a lone drop midway between samples stays inside.
A slower one rests on the surface rather than flying off it, and would only
hold a one-sample spike there, so it blends in.
