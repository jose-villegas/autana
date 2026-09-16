# Shading and Colour

How a cell's material and variant become a pixel, and the rules that keep
the two from drifting apart. Read
[`Sand-Simulation.md`](Sand-Simulation.md) first if you have not - this
assumes you already know what a cell byte and a material row are.
[`Adding-a-Material.md`](Adding-a-Material.md) is the sibling checklist for
adding a whole new material; this document is narrower and deeper, entirely
about how an *existing* material is painted.

Every number below was measured, either on a host-side probe against the
real palette and the real `material_colours()`, or on the board itself -
none of it is reasoned from a desk. See "How to test a shading change" at
the end for the harness these measurements came from.

---

## The pipeline

One byte per cell: a material id in the high nibble, a variant in the low
one.

```
┌───────────────┬───────────────┐
│  material id  │    variant    │   one cell = one uint8_t
│   (4 bits)    │   (4 bits)    │
└───────────────┴───────────────┘
```

`material_palette()` (`material.c`) is a flat, `const` 256-entry table
indexed by the *raw cell byte*. For a `MATERIAL_FLAT` material, painting a
cell really is one array read, no branch, no arithmetic - the fast path,
and it stays free. Everything below is about the cells that need more.

`material_colours(cell_t c, unsigned hash, unsigned mask, unsigned depth,
gfx_color_t out[3])` is the one function every non-trivial cell goes
through, called from `paint_row_n()` in `app_sand.c` - the hottest loop in
the app, once per cell per dirty row. Its inputs:

- **`hash`** - `material_grain_hash(cx, cy)`, a stable per-cell scramble so
  a speckled material shows the same grain in the same place frame to
  frame. Computed once per cell: a couple of multiplies and shift-xors.
  See "Hash and speckle" below for the one property it has to have.
- **`mask`** - an 8-bit "which of my neighbours are empty" reading: 4
  cardinal bits (`MATERIAL_EDGE_LEFT/RIGHT/UP/DOWN`), computed for every
  cell, and 4 diagonal bits computed only when the cell is water AND
  already a cardinal edge - see "Rim, specular and foam" below.
- **`depth`** - not liquid-exclusive. `paint_row_n()` decides what this
  slot carries *before* calling `material_colours()`, by material identity:
  a root cell (`MATX_ROOT`) gets its live neighbour-root count
  (`material_root_neighbours()`); a leaf or a wood cell next to one gets
  `material_wood_leaf_wave()`'s gust fraction **plus one** (the +1 reserves
  0 as "not near the wave at all" - the fraction's own trough is
  legitimately 0, and that must still select the tint branch); every other
  liquid cell gets the local-depth count described below. Three unrelated
  meanings share one parameter slot, resolved by the caller, never by
  `material_colours()` inspecting anything but the material id.
- **`out[3]`** - `out[0]` is a cell's body colour; `out[1]` today always
  equals `out[0]` (a leftover slot from a two-diagonal weave that no
  longer exists - `test_metal_shine_does_not_vary_between_cells`,
  `suite_sand_roots.c`, pins the mirror rather than a second colour);
  `out[2]` is the one HATCHED cell reads for its shine band. A flat or
  speckled material sets all three the same.

The return value, `material_pattern_t`, is `MATERIAL_FLAT`,
`MATERIAL_SPECKLED`, or `MATERIAL_HATCHED` - a hint to the painter about
how much per-pixel work this cell needs, never read for anything else. See
"Patterns" below for which material draws which.

**Edges are softened.** A cell with empty space cardinally beside it is
drawn most of the way back toward its own resting colour rather than its
current one - a wall going hot or cold otherwise changes its whole
silhouette the moment any part of it heats up. The outline still shifts,
just by much less than the body does.

**The cost discipline that governs every change in this file:** `depth`,
`hash`, and the diagonal half of `mask` are each computed *once per frame*
where possible (a gravity-derived value, a blend weight) and reduced, per
cell, to a plain read, comparison, or multiply-add-shift - never a per-cell
divide, never a per-cell trig call, never a second full-grid pass. A new
signal that cannot state its per-cell cost in one sentence has probably not
found the frame-level version of it yet.

### Two conventions that are easy to get backwards

- `sand_at()` reads an out-of-bounds cell as **stone**, never empty - walls
  at the screen edge are solid for free, and `mask`'s cardinal bits rely on
  exactly this: a wall against the edge gets no outline there.
- Grid/screen coordinates have **y increasing DOWN the screen** throughout
  this app. `MATERIAL_EDGE_UP` is `-y`; local depth's "toward the surface"
  direction and its row-order reversal both hinge on this sign, and it
  stays silently backwards until someone tilts the device upside down.

---

## The variant, by material kind

The low nibble's meaning is fixed per material *kind* (`Sand-Simulation.md`
has the full table) and that meaning is *why* each material paints the way
it does:

| Kind | Variant means | How it paints |
|---|---|---|
| Powder (sand, snow) | a shade, picked once when painted and carried with the grain | `MATERIAL_SPECKLED`, indexed by `hash` |
| Liquid (water, oil, lava, acid) | a fill level, 1-15 | flat body colour in the interior, fill-indexed + specular on the rim |
| Transient (gas, fire, steam, ember) | life remaining | flat, indexed straight by variant - dying is one end of the ramp, fresh the other |
| Glass | heat, 0-15 | `MATERIAL_SPECKLED` with a live per-cell shimmer; the ramp doubles as a temperature gauge, because the palette is already indexed by the whole cell |
| Stone | a texture temperature/speckle hybrid | `MATERIAL_SPECKLED` |
| Soil (dirt) | dry tone (0-7) or moisture level 1-7, told apart **by state**, not a fixed bit split - `CELL_SOIL_TONE()`/`CELL_SOIL_MOISTURE()`, `material.h` | one dusty-tan-to-damp-earth ramp, indexed directly by the variant |

The recurring mistake this table exists to prevent: **treating a variant's
colour ramp as if it meant something the variant does not actually carry.**
Building a ramp around "how much is here" when the variant actually records
a solver residual, a screen position, or a state code only looks right in
whichever one scene it was tuned against - see "Why the interior ignores
fill level" below for the worked example.

Extended statics (metal, ice, plant, leaf, root - `MAT_EXTENDED`, id 15,
`0xF0`-`0xF7`) do not fit this table at all: their whole byte names their
*identity*, not a variant, so they carry no ramp. Their look comes from
`hash` (a fixed grain table per material) and, for leaf and root, the
reused `depth` slot described above.

---

## Patterns: flat, speckled, hatched

| Pattern | Cost | Who draws it |
|---|---|---|
| `MATERIAL_FLAT` | one colour, whole cell block | empty, most transients (gas, fire, steam, smoke, oil, lava, acid interiors), a liquid's rim body, wood while burning, cullet |
| `MATERIAL_SPECKLED` | one colour per cell, by `hash` (or by `hash` + a live blend) | sand, stone, wood (unlit), glass, plant, ice, root, an unlit wood cell shaded by a nearby leaf's wind |
| `MATERIAL_HATCHED` | `MATERIAL_SPECKLED`'s per-cell colour, plus a second colour swept along a gravity-relative diagonal | metal only |

Glass is **not** hatched today, despite the ramp/gauge similarity to
metal's shine - `material_palette.c`'s `MAT_GLASS` case returns
`MATERIAL_SPECKLED`. Its apparent shimmer is a live `LERP8` blend toward a
brighter target, keyed on `hash` plus a phase that drifts with gravity's
own bearing (`glass_phase` - see "Gravity-aware shine" below), not a second
diagonal colour. Metal is the only `MATERIAL_HATCHED` material in the tree.

A `MATERIAL_HATCHED` cell paints a single band, not a crossed weave:

```
one grid cell, upscaled to the panel

┌─────────────┐
│ col[0] col[0] │   background = col[0] (the grain colour)
│ col[0]▓▓▓▓▓▓▓ │   ▓ = col[2] (metal_shine) wherever the band,
│▓▓▓▓▓ col[0] col[0]│   swept at material_shine_direction()'s angle, crosses
└─────────────┘
```

In `GFX_PIXFMT_INDEXED8` mode the band is sampled once at the cell's own
centre instead of per pixel - see "Indexed colour modes" below.

---

## Liquids: interior, rim, and local depth

### Why the interior ignores fill level

A liquid's variant is a fill level, 1-15 - a **solver transient**, not a
measure of how deep a cell sits in its own pool. Measured directly against
settled scenes: 0 of 747 interior cells were anything but full at 40
degrees tilted and settled, 0 of 720 settled flat, and only about 5% varied
even mid-tilt. A ramp built on "shallow is pale, deep is dark" therefore
painted almost nothing, almost all the time - and while a pool is actively
levelling, neighbouring interior cells can land on different fill values
for purely numerical reasons, so the same ramp reads as a noise comb
instead of noise-free depth.

The fix: a liquid's **interior** (no empty cardinal neighbour) always
paints one flat body colour, regardless of its own fill level. Fill level
is read only at the **rim**, the one place it is actually meaningful (see
below). Depth into the pool - the actual cue the interior needs - comes
from a separate signal, walked fresh every frame: local depth.

A related trap: **coverage and depth are different questions.** "How much
of this pixel is filled" (a partially-filled rim cell rendering at full
opacity in its brightest colour, reading as the single brightest thing in
the pool) wants compositing against the background by coverage - `MIX`-
style - not the depth ramp below. Reaching for local depth to answer a
coverage question, or the reverse, produces a ramp that is internally
consistent and still wrong.

### Local depth: distance to the surface, along gravity

Local depth is the count of liquid cells of the *same material* between
this cell and the nearest non-liquid boundary, walked along gravity's own
ray (Bresenham), not along a screen axis - an obstacle sitting inside a
pool must cast a depth shadow along the true tilt, not straight down or
sideways, and only a ray that actually follows gravity can do that. The
walk switches between two regimes at 45 degrees, on `|gy| >= |gx|`:

```
Vertical-dominant (|gy| >= |gx|)         Horizontal-dominant (|gx| > |gy|)
one ROW per step, drifting sideways      one COLUMN per step, drifting up/down
by a fixed offset shared by the          by +/-1 row wherever gravity's own
whole row (gravity doesn't change        diagonal drift crosses a row
column to column)                        boundary (a per-cell Bresenham error)
```

Both regimes measure the *same* quantity - cells along the ray - so a
regime flip changes only how the count is computed, never what it means;
the two agree exactly at the 45-degree crossing by construction. This is
why the flip is safe where an earlier, now-removed design's axis flip was
not: that design blended two *different* quantities (a plain vertical
count and a plain horizontal count), so its own flip was a jump in the
reported value, however rarely it fired.

**Storage is a plain, gravity-agnostic step count, clamped at
`LOCAL_DEPTH_COUNT_CEILING` (== `MATERIAL_LIQUID_DEPTH_BAND`, 24),
projected to a cell distance fresh every frame** by the current frame's own
gravity (`count * local_depth_scale_q8 >> 8`, `app_sand.c`). This is
deliberate, not incidental: gravity can rotate between the frame that wrote
a stored value and the frame that reads it, and a plain count survives
being re-projected under new gravity, while a value that already has an
angle baked into it does not - there is no way to un-bake an angle later.
Whenever this mechanism is touched again, keep the stored quantity
angle-free and do the projection at the point of use.

**Clamp each input to the scale it renders on *before* combining it with
anything else, never after.** `material_colours()` clamps `depth` at 24 -
past that the panel cannot tell one depth from another - and that clamp
has to happen before any blending, averaging, or further arithmetic touches
the value, not after: a value combined while still free to run past its
render range gets dragged toward whatever the far-out input would show, a
result neither input alone would ever produce. This is a standing rule for
any future signal built the same way, not only this one.

**A liquid's own `MATERIAL_LIQUID_DEPTH_BAND` (24) is shared, on purpose,
by three unrelated call sites** - `material_colours()`'s clamp,
`local_depth_row_a[]`/`local_depth_row_b[]`'s ceiling, and
`mark_depth_band()`'s (`sand_priv.h`) dirty-span width, which widens a
pour's repaint radius by exactly this many cells along gravity's dominant
axis on the assumption that anything farther already renders the same
saturated tone whether its true depth is 25 or 250. If the render clamp
ever moves, this radius has to move with it, or the renderer's own
assumption stops matching what it dirties.

**A cross-row carry is trusted only when it is known to describe the
immediately-preceding row - and only on the boundary path.**
`local_depth_prev_cy` records which row `local_depth_prev_row[]` actually
holds; `paint_row_n()` compares it against the expected row once per row.
A same-material climb trusts a stale-but-saturated carry deliberately -
that is what lets an isolated repaint deep inside a settled pool render at
full depth instead of re-climbing from 1, the property
`test_a_sparse_repaint_does_not_band_a_tall_liquid_column` pins. A
*boundary* cell (this cell touches a different material or air) cannot
make the same assumption: if the carry does not describe the row one step
back, the honest value is zero, not whatever a different part of the pool last
left there. Distrusting the carry on both paths reintroduces re-climb
banding; trusting it on both paths lets a stale row's saturated value leak
across a real boundary and, once, rendered an entire settled pool at
maximum depth for a frame.

**A regime flip, or a reversal flag flip that this grid's walk can actually
observe, resets both row buffers and the debounce array wholesale.** The
gate is exact arithmetic, not a tuned deadband: a vertical-dominant walk's
sideways drift is observable only if `grid_h * |gx| >= |gy|`, and a
horizontal-dominant walk's cross-row read is observable only if
`grid_w * |gy| >= |gx|` (`update_local_depth_gravity()`, `app_sand.c`) -
otherwise the flipped flag cannot change any number the walk computes, and
gating on it would silently reintroduce a tuned dead zone this mechanism
already removed once. A regime flip itself is never gated - it always
changes what the array slot means, whatever the magnitudes are. Without
this gate, ordinary hand tremor at axis lock (`gx` or `gy` sitting near
zero) flips a reversal flag many times a second, and an ungated reset wipes
the debounce on nearly every frame - measured, 40 resets in 40 tremor
frames - which never lets a boundary commit, holding a settled pool's
surface at the shallowest shade forever instead of the true one.

**The debounce key means something different in each regime** - found by
testing, not designed in advance:

| Regime | `local_depth_top_row[cx]` holds | Commits when |
|---|---|---|
| Vertical-dominant | the row index of column `cx`'s most recent boundary request | the *same row* asks again on a later painted frame |
| Horizontal-dominant | a plain pending flag (any value other than 255) | any repeat boundary request - re-armed every time, cleared to 255 on a confirmed same-material climb |

Vertical-dominant can key on the row because a column's own sweep visits
that slot roughly once per frame, so "the row" identifies a stable
location across frames. Horizontal-dominant cannot reuse that key: every
row-call writes every column's slot, so a row-indexed key almost never
repeats even beside a permanent wall, and the debounce would hold forever
instead of ever committing. Both regimes share the *array*, not the
convention - a regime flip resets it wholesale, so neither has to interpret
a value the other one wrote.

Net cost of this mechanism: three `GRID_W_MAX` (184-byte) arrays -
`local_depth_row_a[]`, `local_depth_row_b[]`, `local_depth_top_row[]` - 552
bytes of `.bss`, plus a handful of per-frame scalars.

### Any distance-based signal must be scaled to what it currently measures

`DEPTH_SATURATE_CELLS` (== `MATERIAL_LIQUID_DEPTH_BAND`, 24) exists because
an earlier, screen-position version of this signal legitimately spanned
0-255, and the shade formula was tuned against that range. Once depth
became local (rarely exceeding a few dozen cells for any real pool),
the same formula stayed pinned near one extreme - measured, luminance flat
at 159 for local depth 0 through 20 cells before it started moving. 24 was
picked so a realistic pool's actual depth range (0-40 cells) spans the
visible brightness range instead of needing to approach 255 to show
anything. Whenever a signal's meaning changes again - local to something
else, relative to absolute - every constant tuned against its old range
has to be re-derived, not assumed to still fit.

---

## Rim, specular and foam

A liquid's **rim** (any cardinal neighbour empty) reads its own fill level
(1-15) directly, shifted by `liquid_spec[mask]` - a 16-entry table,
indexed by the 4-bit cardinal mask, filled once a frame by
`material_set_gravity()` from the current tilt. A rim cell whose open side
faces away from "up" (minus gravity) darkens; one facing toward it
brightens - a pool's lit top versus an overhang's shaded underside at the
same fill level.

**Foam is gated by rim curvature, not a motion flag.** `abs(neighbour_count
- 3)` is 0 on a straight edge, positive on either a concave crevice or a
convex protrusion - and a calm surface is smooth by construction while a
sloshing one is jagged along its whole length, so curvature alone is
already a disturbance detector with no simulation state added. Measured: a
still, flat pool has non-flat rim on only 4% of its cells; two steps into a
75-degree tilt, 94%. The same signal gives a waterfall its foam for free -
a lip, the falling stream's edges, and the plunge point all foam without
any code written for "waterfall." Curvature is dithered against a
per-curvature threshold table (`water_foam_threshold[]`: 0, 3, 5, 7 foaming
hash values out of 8, for curvature 0 through 3+) and animated by adding
(never XOR-ing - XOR leaves roughly half of all phase steps unable to
change the foam set at all) a foam phase to the per-cell hash.

**Water's foam hash is coarsened, and that is only safe while foam is its
one consumer.** `paint_row_n()` hands water a hash sampled at
`(cx >> FOAM_BLOB_SHIFT, cy >> FOAM_BLOB_SHIFT)` (`FOAM_BLOB_SHIFT` = 3) so
foam gathers in blobs bigger than a single cell - `material_colours()`
itself stays ignorant of coordinates. Stone's speckle, wood's grain, and
glass's shimmer all depend on the fine, per-cell hash; adjacent cells
disagreeing is the entire point of a speckle. The day water needs a second,
per-cell effect of its own, this coarsening has to move from "every water
cell" to "only where foam reads it," or the new effect stripes in blocks
with no test to explain why. Nothing in the type system enforces this
today - it is a fact about the file, which is why it is written down here.

---

## Gravity-aware shine

Three separate mechanisms read gravity's direction to shade something -
worth telling apart before reaching for any one as a template for a fourth
material:

| Material | Mechanism | Shape |
|---|---|---|
| Liquid rim | `liquid_spec[mask]` (`material_set_gravity()`) | a 16-entry table by cardinal mask, precomputed once a frame, read by index per rim cell |
| Metal (`MATERIAL_HATCHED`) | `material_shine_direction()` | a Q8 unit vector (minus gravity, turned 45 degrees) computed once a frame, walked per pixel (or sampled once per cell in indexed mode) to place the shine band |
| Glass (`MATERIAL_SPECKLED`) | `glass_phase` (`advance_glass_phase()`, gravity's own bearing angle, quantised) | a single phase added to `hash` before the live `LERP8` blend - a shimmer, not a band |

All three read the same frame's gravity; none of them share code, because
each solves a differently-shaped problem (a per-mask table, a swept
diagonal, a per-cell dither phase).

---

## Hash and speckle

`material_grain_hash(cx, cy)` needs a real avalanche - every output bit
depending on every input bit - or it stripes. A single shift-xor of two
multiplied words is not enough: measured over a 128x128 area, its low
three bits (the ones every speckled material actually reads) came out
nearly constant along a row, drawing stone and wood as flat horizontal
bands with no texture. The current hash adds a finalising round (multiply
by a high-avalanche odd constant, then another shift-xor); adjacent cells
now share a shade 2077 times out of 16256, against an ideal of 2032 - close
enough that stone reads as stone. Any new per-cell scramble needs the same
check: measure adjacency correlation over a real grid before trusting it to
speckle anything.

---

## Cullet's colour cycle

Cullet (sand's reserved top band, `SAND_CULLET_BASE` (12) through
`MATERIAL_VARIANTS - 1`, four shades - "sand that used to be glass") keeps
a fixed nibble per grain, the same as any other speckled shade, but each of
the four nibbles now names a **starting point**, a quarter-turn apart, on a
shared 16-entry cycle (`cullet_cycle[]`, `material.c`) that a per-frame
phase (`material_set_cullet_phase()`) steps through over real time. A heap
of broken glass shimmers through four pale tints instead of sitting on one.

The cycle is built from four pastel anchors chosen to stay close in both
hue and lightness - the point of cullet is to read as ground glass catching
the light, not as four materials taking turns.
`test_cullet_stays_pale_at_every_phase` (`suite_sand_tone.c`) guards this,
floored against the darkest dune shade's own luminance rather than a fixed
number, so it stays meaningful if the dune ramp is retuned. Rarely
(`CULLET_GLINT_ONE_IN` = 192, one grain in 192 per phase step) a cullet
cell flashes pure white instead of its cycle colour - a facet catching the
light - decided by the same hash-plus-phase mix foam's dither uses, at no
extra per-cell cost.

The simulation does not change for any of this: `material_colours()` is a
pure function of the cell byte plus whatever per-frame state has been set
(gravity, foam phase, cullet phase), so the fingerprint suite - which
hashes cell bytes, never rendered pixels - stays green while the display
shimmers. What *does* move is `app_sand.c`'s repaint bookkeeping: a cullet
cell's row needs the same periodic-wake treatment shine already gets -
`row_flags[]` carries a `ROW_FLAG_CULLET` bit, and `mark_wake_hits()` marks
every row holding one dirty whenever the phase advances, or a heap that has
stopped moving would repaint on nothing and freeze on whatever tint it held
the moment it went still.

---

## Indexed colour modes: 256 and 16

The sand launch menu's COLOUR option defaults to 256 and picks between FULL
(today's RGB565 path, byte-identical - this document's whole pipeline
above), 256, and 16. Both alternates request `GFX_PIXFMT_INDEXED8`
(`gfx/gfx_mode.h`): gfx frees the PSRAM framebuffer, as `GFX_LAYOUT_BANDS`
already does, and instead owns a persistent `grid_w x grid_h` byte image of
palette indices in internal RAM plus a 256-entry RGB565 LUT. Sand writes
indices, never pixels (`paint_row_n()` - the same function the RGB565 path
uses, forking only at the final write so hash, mask and local depth stay
one piece of code, not two that can drift apart) - one byte per changed
cell, not an `n x n` pixel block, which is why this mode skips the PSRAM
framebuffer's slow writes (see `docs/notes/Board-and-Memory.md` for those
numbers).

**The palette.** `main/apps/sand/tools/shading_palette.c` emits
`sand_palette256.h`: a 256-entry LUT (16 reserved UI entries, 240 sand
entries), a 65536-entry `sand_rgb565_to_index[]` reverse map keyed by
native RGB565 (the same per-group OKLab assignment the study computed, not
a fresh distance search - a key several materials produce keeps whichever
material's own on-scene pixel count is larger), and a 256 x 16-phase
`sand_palette16_dither_rgb[]` table - one precomputed RGB565 value per
(palette index, Bayer phase) pair, so 16-colour expansion is a single
lookup, never a per-pixel dither call. Regenerate both the study's report
and this header together with `main/apps/sand/tools/report_shading_palette.sh`.
`material_palette256_index()` (`material_palette.c`) is the colour-to-index
step: one `sand_rgb565_to_index[]` read, no search; a colour the study's
sweep never produced falls back to the table's own OKLab-nearest search,
paid once at generation time.

**`MATERIAL_HATCHED` adapts, rather than drops, in indexed modes.**
`paint_row_n()` samples the same shine line once at each cell's own centre
instead of per pixel: a cell the line crosses takes `col[2]`'s own index
(already one of the study's swept colours) instead of `col[0]`'s. Local
depth, root thickness and leaf wave all reach the index exactly as they
reach a pixel, since `paint_row_n()` computes `depth` once and every output
reads the same value. None of this touches the FULL path or the simulation
itself - `material_colours()` is unmodified and the fingerprint suite stays
green in every mode.

**Indexed mode must never be active when the launch menu draws** - it has
no indexed draw path and would touch a framebuffer that does not exist.
`sand_colour_state.h` is the small, host-tested state machine deciding when
a `gfx_mode_enter()`/`exit()` call is owed. **UI in 256/16 is scoped down,
not fully wired**: the always-on overlays a running sand screen draws
outside the grid (emitter markers, the mode label) are skipped while an
indexed mode is active, since drawing them today would touch a
framebuffer that does not exist in this mode. The palette and brush
screens instead have sand temporarily exit indexed mode back to FULL for
as long as either is open, repainting the RGB565 backdrop before dimming
it, and re-entering indexed mode on close. The reserved UI indices (0-15)
already present in `sand_palette256_lut` are for the eventual
indexed-target version of this; wiring `gfx_target.h`/microui through them
is future work.

---

## Reference: gravity's numbers, and who actually reads them

- `gx, gy` are signed ints, produced exactly once a frame by
  `read_gravity_input()` (`app_sand.c`) - the same smoothed pair,
  unmodified, feeds `sand_step()`, `material_set_gravity()`, and local
  depth's own per-frame setup. There is exactly one gravity source
  reaching shading, and it already goes through the tilt filter.
- Magnitude: `IMU_COUNTS_PER_G` is 4096; host-side test fixtures use
  gravity pairs of magnitude ~1000 (e.g. `(500, 866)` for 30 degrees) as
  "the same order of magnitude," not an exact unit match.
- The tilt filter (`tilt.h`): `TILT_TAU_STILL_MS` 260, `TILT_TAU_MOVING_MS`
  40 (an exponential-moving-average time constant, not a hard delay),
  `TILT_MAX_DT_MS` 100 (caps a stall from teleporting the filter), trust
  gate 70-130% of 1g, free-fall below 30%, shake threshold 50% of 1g,
  shake's own smoothing tau 120ms.
- **`gravity_quarter_turn()` (`app_sand.c`) is a separate, discrete (0-3)
  mechanism** - a plain snap-to-nearest-90-degrees, used only to turn the
  mode label's text to follow whichever edge is physically "up." It is not
  read anywhere in the shading pipeline; `display.h` has yet a third,
  independent orientation decision (its own hysteresis) for the whole
  shell's rotation. If "orientation" comes up again, check which of these
  three is meant before assuming they are the same thing.
- The "pick the single dominant axis" idiom recurs project-wide -
  `build_xflow()`'s liquid cross-flow levelling, `sand_gravity_direction()`'s
  load direction, and local depth's regime choice all use it. It is a
  cheap, accepted approximation everywhere it appears, but every call site
  has a seam at the tie point; whether that seam needs smoothing (as local
  depth's did) or is fine left sharp is a case-by-case, measured judgement.

---

## How to test a shading change

- **A host-side probe, no device needed.** Every shading investigation in
  this codebase used the same shape: a small throwaway `.c` file linking
  `sand.c`, `sand_liquid.c`, `sand_gas.c`, `sand_reactions.c`, `material.c`,
  `row_runs.c` directly -

  ```
  gcc -std=c11 -O1 -I <main> -I <main>/apps/sand probe.c \
      <main>/apps/sand/{sand,sand_liquid,sand_gas,sand_reactions,material,row_runs}.c \
      -o probe -lm
  ```

  Call `material_palette()`/`material_colours()` directly to inspect exact
  colours and luminance; build a real `sand_t`/`sand_step()` scene (a
  settled pool, an irregular one with an obstacle, a tilt sweep) to measure
  the actual signal a fix depends on before writing any code against it.
- **Match the metric to the complaint, and render a ground truth to measure
  against.** "Flickering" and "a line inside the water" are different
  measurements: the frame-to-frame swing in a pool's *mean* depth cannot
  see a line at all, since a mean does not move when brightness is
  shuffled between rows. For a spatial complaint, measure disagreement
  between adjacent interior cells against a reference render of the same
  frame - otherwise legitimate depth contours get counted as artifacts.
  Two cheap ablations before theorising: repaint every row every frame, and
  force one signal to zero. If the artifact survives both, it is not the
  mechanism suspected.
- **A probe that reproduces nothing may just be too tidy.** An
  exactly-filled rectangular pool at equilibrium settles to zero dirty
  rows, so every repaint comes from the periodic wake tick, which always
  walks a whole column in order - hiding every sparsity bug there is. Give
  the scene a free surface and a trickle, then check the actual number of
  rows repainted on a non-wake frame before trusting a null result.
- **`panel_luminance()`** (`suite_sand_common.c`) is the Rec.601 luminance
  helper already used throughout the suite - reuse it rather than writing a
  second one.
- **`app_sand.c` is not linked into the host suite** (`run_tests.sh`
  excludes every `app_*.c`) - anything living only in `paint_row_n()`'s own
  per-frame accumulators (foam phase, local depth's row-order logic) needs
  a test-local mirror of the algorithm, not a real link. A mirror that
  duplicates an algorithm instead of linking to it needs the same scrutiny
  the code it protects gets: re-verify it against the *current* shape of
  the mechanism after every change to it, not just whatever the mirror
  already agrees with, and re-run each test's own "temporarily break the
  fix, confirm red" proof again - a comment asserting *why* a mirror's
  behaviour is correct is a claim to re-check, not evidence in itself, and
  two independently-wrong implementations can still agree with each other
  and pass.
- **Prove every new cosmetic test load-bearing.** Verify red-then-green:
  make the minimal edit that should break the fix, confirm the new test
  actually fails and for the stated reason, restore the fix, confirm green.
  A palette or shading test that cannot be made to fail is no test at all
  on this project.
- **The device is still the final judge.** Host probes catch regressions
  and prove a mechanism does what it claims; whether a colour or gradient
  actually *looks right* is a question only the board can answer. Expect to
  flash and look before calling a shading change done.

---

## Related

- [`Sand-Simulation.md`](Sand-Simulation.md) - the "why" behind the
  material table, the water model, and the performance discipline all of
  this sits inside.
- [`Architecture.md`](Architecture.md) - the single-page shape of the
  whole app, including the exact hops from a `.c` change to a real number
  on the device.
- [`Adding-a-Material.md`](Adding-a-Material.md) - the checklist for a
  brand new material; read that first if the material in question does not
  exist yet, this document second once it does.
