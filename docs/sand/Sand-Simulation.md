# The Falling-Sand Simulation

What `main/apps/sand/` actually is: a cellular automaton with a handful of
materials, tilt-steered by the accelerometer, that has to run inside a
memory and time budget tight enough that most of its design decisions were
forced by measurement rather than chosen for elegance.

This document is the "why" behind it - the material encoding, the movement
rules, the water model, and the performance discipline that shaped all of
them. [`Architecture.md`](Architecture.md) is the "what": a single-page map
of the same app's shape, with the byte layout, the material table and the
per-step pipeline diagram this document assumes rather than repeats.

For the app-registration mechanics (how `main/apps/*` plugs into the
shell), see `docs/Launcher-Architecture.md`. For the hardware constraints
underneath everything here, see `docs/notes/README.md`.

---

## The grid is one byte per cell

The cell size is a quality setting on the app's boot menu, and the grid
shrinks or grows with it:

| Setting | Cell | Grid | Cells (= bytes) |
|---|---|---|---:|
| ULTRA | 2 px | 184 x 224 | 41,216 (~41 KB) |
| HIGH | 3 px | 122 x 149 | 18,178 (~18 KB) |
| NORMAL (default) | 4 px | 92 x 112 | 10,304 (~10 KB) |
| LOW | 6 px | 61 x 74 | 4,514 (~4.5 KB) |
| VERY LOW | 8 px | 46 x 56 | 2,576 (~2.5 KB) |

ULTRA's 41,216-byte grid is, in `docs/notes/Board-and-Memory.md`'s own
words, "the largest single contiguous allocation of interest" the sand app
makes. It is unchanged since this board moved to a PSRAM framebuffer.

That move is worth stating plainly: the 322 KiB framebuffer now lives
entirely in PSRAM (`BOARD_FRAMEBUFFER_CAPS`, `board.h`) and no longer
competes with the sand grid, or anything else, for internal SRAM.

Measured internal (non-PSRAM) free heap after `gfx_init()` is 311,775
bytes - comfortably more than even a two-byte-per-cell grid at ULTRA
(82 KB) would need.

The one-byte encoding predates that move: it was chosen when the
framebuffer still lived in the same internal pool as everything else,
leaving far less headroom to work with. The port to a PSRAM board loosened
that specific constraint, but the byte layout itself was never revisited.

Every other per-cell trick in this file - reusing the nibble, doubling
the table only where a material actually needs it - still follows the
discipline that byte was chosen under. Unwinding it for memory that is no
longer scarce would touch every material's encoding for no behavioural
gain.

[`Architecture.md`](Architecture.md#the-grid-in-one-byte) has the byte
layout itself and the full table of what the low nibble means for each
material `kind`. The short version: nothing about that nibble is fixed -
it is reused per material, on purpose, and none of the reuses are free
accidents:

- **A powder's nibble is a shade**, so a pile has texture rather than
  reading as one flat block of colour. It travels with the grain rather
  than being derived from its position, because position-derived colour
  makes a falling pile shimmer as it moves.
- **A liquid's nibble is a fill level**, 1-15 (`MASS_MAX`). This is what
  lets water level itself using only its immediate neighbours - see
  [The water model](#the-water-model) below.
- **A transient material's nibble is life remaining** (gas, fire, steam),
  counting down to nothing. Reusing the nibble is what makes that free
  instead of needing a second byte - see [Gas: a biased random
  walk](#gas-a-biased-random-walk) and [Fire
  chemistry](#fire-chemistry-wood-embers-steam-and-a-working-boiler).
- **Gunpowder splits its own nibble by its own top bit** instead of
  spending it on a shade: `0xF8`-`0xFF` is gunpowder, sharing material id
  15 (`MAT_EXTENDED`) with the extended statics but claiming the other
  half of that nibble for a real `KIND_POWDER` material. Its remaining 3
  bits are a state split like dirt's, just narrower - codes 0-2 are dry
  tones, 3-6 are moisture 1-4, and 7 is a burning state (`GUNPOWDER_LIT`).
  See [Fire chemistry](#fire-chemistry-wood-embers-steam-and-a-working-boiler)'s
  gunpowder passage for what lit does.

## Materials are a flash-resident table, not code

`materials[MATERIAL_ROWS]` (`material.c`) is `const`, so it is
memory-mapped from flash and costs **zero bytes of RAM** - confirmed via
`idf.py size`, not assumed. Adding a material is a row in that table, not
a branch in the movement code.

Every field on `material_t` - `kind`, `density`, `slip`, `repose`,
`scatter`, `decay`, `mobility`, `sight`, plus the cold `name` string - is
read from the innermost loop, several times per cell per step. That is
why the struct stays small. See `material.h`'s own header and struct
comments for the field-by-field reasoning, and
[`Architecture.md`](Architecture.md#the-material-table-today) for the
current table of all 16 material slots.

`MATERIAL_ROWS` is **32**, not 16 - doubled once gunpowder split material id
15's nibble in two. That doubled the hot table's flash footprint from 192 B
to 384 B, but left the sweep with the same one shift, one indexed load per
cell - see [`Architecture.md`'s "Getting more than sixteen materials out of
one nibble"](Architecture.md#getting-more-than-sixteen-materials-out-of-one-nibble)
for how the doubling works and what it cost.

The 256-entry colour palette (`material_palette()`) is built the same way -
16 shades per material, interpolated at compile time into another `const`
table, so drawing a cell is one array index and zero colour maths at
runtime. It lives in `material_palette.c`/`.h`, split from `material.c`/`.h`
for the same reason `sand.c`/`sand_liquid.c` are two files below - colour is
a different concern from identity and behaviour.

## Movement: one rule, and one invariant that has to be right

Every grain gets the same rule: try to move the way gravity points; failing
that, try the two directions either side of it. Angle of repose, heaps that
collapse when undermined, sand pouring through a gap - none of that is
modelled explicitly. It all falls out of those three attempts. (Gas is the
one exception - the same rule, run against a negated direction, from its
own pass - see [Gas: a biased random walk](#gas-a-biased-random-walk).)

**The sweep order is the one thing that cannot be wrong.** A grain only
ever moves into a cell in the gravity-ward half of its neighbourhood. So
sweeping the grid *against* the direction of travel guarantees a cell's
destination has already been visited this step - a grain that moves
cannot be picked up and moved again in the same frame.

Sweep the other way and a falling grain teleports to the floor in one
frame instead of falling one cell at a time. Every pass in this codebase
(the main sweep, and the liquid cross-flow pass below) is built around
this same guarantee, each with its own sweep order derived from whichever
direction *it* moves things in.

**Friction is two separate things**, because burial alone does not explain
the reported symptom (a floor of sand skating sideways on the faintest
tilt):

| Mechanism | Rule | What a liquid does instead |
|---|---|---|
| **Angle of repose** | A grain on a slope stays put until the slope exceeds the friction angle: `descent > mu * lateral`, the same test as a block on an incline. `mu` is `repose / 10`; sand's `repose = 7` is ~35 degrees. | `repose = 0` means no angle of repose exists at all - it slides sideways however level the surface is, which is what makes it a liquid. |
| **Burial** | A grain counts how many grains are stacked directly against gravity above it, and each one halves its chance of sliding (`SAND_SLIP_CHANCE = 96` in 256, halving per grain, capped hard past `SAND_LOAD_CAP = 5`). | `slip = 255` means load never holds it at all - water at the bottom of a pool flows exactly as freely as water at the top. |

**Gravity's direction is dithered, not snapped to nearest-of-eight.**
Snapping makes a slow tilt arrive in 45-degree jerks. Instead, each step
randomly picks one of the two octants bracketing the true angle, weighted so
the long-run average matches it - the same trick as dithering a colour ramp,
applied to a direction. At 60 fps the eye integrates the two directions into
one smooth angle.

## The water model

A liquid cell carries an amount, 1-15 (`MASS_MAX`), not a plain on/off
state. Two rules run in the main sweep, in order:

- **Down, then down-the-slope** (`move_liquid_grain()`, `sand_liquid.c`):
  fill the cell below if it has room, then share what is left with the two
  diagonal downhill neighbours. Still only immediate neighbours, still
  gravity-ward, so it carries the same no-double-move guarantee every other
  move in the main sweep does.
- **Cross-flow** (`equalise_liquids()`, `sand_liquid.c`): a *second*,
  separate sweep that moves mass sideways, along the surface, looking
  further than one cell.

That second rule exists because the obvious designs do not work:

| Design | Problem |
|---|---|
| Cell-count, no amount - a cell is either water or not | Two adjacent cells at "full" and "empty" have no legal move that leaves a valid state in between: a wide pool freezes into a staircase. A genuine fixed point of any rule shaped like this, not a bug in one attempt at it. |
| Local mass diffusion only - an amount per cell, shared with immediate neighbours alone | Correct per the literature, and measurably too slow: levelling a 184-cell pool by neighbour-to-neighbour diffusion takes tens of thousands of steps, because information moves only one cell per step. Measured: a real-width pool was still 6 cells proud after 5,000 steps. An `O(n^2)` bound on any rule of this shape, not a tuning problem. |
| Mass, plus a bounded surface scan - what shipped | See below. |

### Cross-flow: a cheap stand-in for pressure

`equalise_liquids()` walks up to `SAND_LIQUID_SIGHT` (8) cells along the
surface, looking for somewhere shallower, and moves half the difference in
*level* there - mass adjusted for how much deeper one cell sits than
another along gravity, not raw mass. It is deliberately not local: in real
water, pressure travels far faster than the water itself does, and this is
the cheap stand-in every falling-sand game has some version of.

**Why this cannot live in the main sweep.** The main sweep's no-double-move
guarantee depends on sweeping against gravity. Once gravity is tilted,
that pins the sweep on *both* axes, and of the two directions across the
flow, only the one the sweep has already passed is safe to use.

Water could cross a slope one way and never back: a tilted pool did not
level at all, it walked into the low corner and sat there. Cross-flow
needs its own pass, with its own sweep order chosen to fit whichever
direction it is using that step - the two alternate every step, so both
directions become available over time.

**Why 8, not more.** A longer sight distance hands mass directly to a cell
far away, skipping everything between - water visibly vanishes from one
spot and reappears in another, and because the sweep direction alternates
every step, it sloshes straight back the next. Measured residual
unevenness after settling:

| `SAND_LIQUID_SIGHT` | Residual unevenness |
|---:|---|
| 4 | 1.6 cells |
| 8 (shipped) | 0.8 cells |
| 16 | 0.5 cells (visibly wrong while getting there - "huge waves") |
| 32 | 0.2 cells |

8 is the point where a 4x reduction in cost costs only ~0.6 cells of extra
unevenness - smaller than a pixel at this grid's resolution.

### Two rays, chosen by position, not by time

A pool's true perpendicular to gravity rarely lines up with one of the
eight ring directions. Cross-flow brackets it between an **axis ray**
(perpendicular to the dominant axis) and the **diagonal ray** beside it,
and picks which one a given column takes on a fixed pattern in *space*
(`xflow_t`, `sand_priv.h`):

```
tilted pool, gravity down-and-right:

  column:     A     B     A     B     A     B
              |      \    |      \    |      \
   axis ray → |       \   |       \   |       \  ← diagonal ray
              v        v  v        v  v        v

  every column checks "is there a shallower cell along my ray";
  A-columns and B-columns disagree on which ray that is,
  but the MIX across the pool still reads as the true angle
```

Two bugs, both fixed and both worth knowing if this code is touched again.

**Bug one: picking the ray from the dithered direction flickered a
settled pool.** Gravity's direction is dithered every step (see
[Movement](#movement-one-rule-and-one-invariant-that-has-to-be-right)
above). If cross-flow's axis followed that dither, "is this level" was
asked along a different axis almost every step.

A settled pool then read as wildly unbalanced along whichever direction
it wasn't just checked against - large amounts of mass swinging back and
forth every step or two, visible as flashing and resettling. Fixed by
taking the axis from the *nearest* (non-dithered) direction instead - the
same fix friction's burial check already used. See
`test_a_settled_pool_does_not_flicker` (`suite_sand_materials.c`).

**Bug two: pinning to one ray flattened every surface to 0/45/90
degrees.** That was the correct fix for the flicker, but it had an
unpriced cost: a pool could then only settle perpendicular to one of
eight directions. The near-vertical octant lost the most - its axis ray
is horizontal, and a horizontal ray can only move mass within a row, so
that octant couldn't tilt its surface at all.

The fix shown above - dithering between the axis ray and the diagonal
beside it, by column, in space rather than in time - restored real tilt
angles without bringing the flicker back.
`test_a_pool_settles_at_the_angle_it_is_tilted_to`
(`suite_sand_materials.c`) guards the second property; the flicker test
above still guards the first. Neither alone is enough.

## Gas: a biased random walk

`MAT_GAS`/`KIND_GAS` (`sand_gas.c`) rises. Each cell takes **one draw and
one probe per step**, regardless of how boxed in it is: a draw picks an
offset around the ring direction pointing away from gravity, weighted out
of 256 (`gas_walk_weights[]`, `sand_gas.c`):

| Pick | Weight (/256) |
|---|---:|
| Straight up | 70 |
| Upper diagonal, either side | 68 each (136 total) |
| Sideways, either side | 11 each (22 total) |
| Lower diagonal, either side | 2 each (4 total) |
| Straight down | 24 |

Almost nothing is spent on the five downward-ish directions: a particle
that drifts down does so bluntly, and giving most of the ring a downward
component reads as smoke *sinking* rather than swirling.

**Why a walk rather than the inverted powder mover.** Gas used to reuse
`try_fall_or_scatter()`/`try_slide()` with the direction negated - the
water model reflected. That mover is exhaustive: it tries each candidate
in turn, so its cost rises exactly when the grid is full and every
candidate is blocked.

The walk is O(1) per cell and looks better, since real hot gas is chaotic
rather than uniformly upward. The exhaustive mover is still reachable
through `sand_set_gas_walk(false)` so the two can be compared, which is
the only thing that still uses it.

**Buoyancy is part of the walk, not a separate pass**, and only on the
upward picks (straight up or an upper diagonal). A blocked upward pick
whose blocker is a liquid *denser* than the gas swaps through it - gas
rises through the liquid instead of sitting trapped in it, because
`can_enter()` only ever admits a liquid to something denser, never to
something lighter.

Sideways and downward picks never bubble: a bubble rises, and a downward
swap would also break the sweep's one-move-per-cell guarantee.

**Why it needs its own pass.** The main sweep's no-double-move guarantee
depends on sweeping *against* the direction things move - right for
gravity-ward materials, exactly backwards for anything moving against
gravity. `sand_step_gas()` runs after the main sweep in reverse row and
column order, so a rising move lands in already-visited territory rather
than teleporting to the ceiling in one step.

**`equalise_gas()`** is gas's counterpart to the liquid cross-flow pass,
for the same reason: rising alone piles gas against a ceiling without
ever spreading it along one.

See [`Adding-a-Material.md`](Adding-a-Material.md) for the end-to-end
walkthrough of building a material like this.

## Fire chemistry: wood, embers, steam, and a working boiler

Fire's reactions live in `sand_reactions.c`, driven by a second table -
`reaction_t reactions[]` (`material.h`) - kept separate from
`materials[]`. `materials[]` is read several times per cell per step by
the main sweep; fire chemistry is read only by the reactions pass, gated
behind `may_have_burning`. Folding `flammability`, `ignites_to`,
`conducts`, `quench_to` and the rest into the hot table would widen its
stride for every step that never touches fire.

### Being alight is a state, not a material

A lit cell keeps its own material and records the fact in its variant
nibble (`reaction_t.burn_decay`). That is what makes "water puts a log
out" expressible: the log is still there, just no longer alight.

`MAT_FIRE` is `KIND_GAS`, so a wood cell that turned *into* fire would
float away on the next `sand_step_gas()`, leaving a hole where the log
was.

Instead a lit log stays `KIND_STATIC`, igniting neighbours and counting
down, while the flame licking off it (`reaction_t.flare`) is ordinary
separate `MAT_FIRE` - there for looks and for reaching fuel stacked
above. "Burning below, flame above" falls out of two simple things
rather than one material trying to be a heat source and a moving flame
at once.

Wood's `flammability` of 6 in 256 makes catching a negotiation - roughly
43 steps of contact with a single flame - so a log burns rather than
vanishing on first touch. A lit log is essentially never `smothered()`:
that predicate needs all four cardinal neighbours *strictly* denser, and
at density 150 only stone (200) qualifies, so burying one in sand will
not put it out. Only decay or water ends it.

### Two exhausts, deliberately different materials

| | What it is | Where it comes from |
|---|---|---|
| `MAT_STEAM` | water that got hot | boiled through a conductor, or flashed off a quenched fire |
| `MAT_SMOKE` | fuel that burned out | a fire or a lit log reaching the end of its life |

Quenching costs the water a unit of its own mass, so a pot boiled dry
eventually runs dry. The two rows are nearly identical in `materials[]`;
the split is really about palettes - steam is cool and bright, smoke warm
and dim, and a fresh puff of smoke tops out dimmer than a dying wisp of
steam so they stay separable where they overlap.

The reason to keep them apart is visual, not physical: a lone fire
burning out in mid-air, nowhere near water, puffing bright kettle-steam
reads as a bug to anyone watching.
`test_quenching_makes_steam_but_burning_out_makes_smoke` guards against
re-merging them on the correct observation that their rows look the
same.

### Gas under standing liquid needs buoyancy to escape

`can_enter()` displaces in one direction only - denser displaces lighter -
so steam (density 5) cannot enter water (30) above it; and a liquid never
consults `can_enter()` at all, while `room_in()` (`sand_liquid.c`) refuses
any cell holding a different material, so the water will not fall into the
steam either. Between the two rules, a gas cell under standing liquid has
no legal move in either direction.

Both movers therefore carry a buoyancy case: `gas_walk_once()` handles it
inline for the walk, `try_bubble()` for the exhaustive mover. Each is a
two-cell swap gated on `KIND_GAS` and an *inverted* density test, so only
something lighter than the liquid rises through it, and water mass is
conserved exactly because a swap carries the liquid's variant nibble
across untouched.

It is deliberately not in `can_enter()` itself. That predicate is the
hottest thing in the project, read several times per cell per step by the
main sweep; a mobility special case there would be paid by every falling
grain of sand forever.

In the gas pass it costs one comparison, only for gas cells, only in a
pass already gated behind `may_have_gas`, and only where the ordinary
rise was already blocked. Sweep order makes it safe for free: the gas
pass sweeps so the destination is already-visited territory, and both
liquid passes ran earlier in the same `sand_step()`, so the displaced
liquid still gets exactly one move.

### Boiling happens at the heat source

`conduct_heat()` converts the very cell touching the hot conductor, and
the steam climbs out from there by itself. A pot on a hot stone reads as a
column of bubbles rising off its base.

**Building one in the app:** a wood floor, a stone basin over it as thick
as one drag of the pour brush, water poured in, and a spark. The wood
catches, chars, and its heat conducts up through the basin floor, boiling
the water above - all without the fire ever leaving the space below the
stone.

### Gunpowder: a fuse, not a detonator

Gunpowder catches and burns like wood, and it is the burning-out that can
end in a blast - there is no separate detonator mechanism.

**Lighting the fuse.** A flame or hot lava touching dry powder ignites it
in the usual way (`flammability` 200, so it catches almost every time it
is rolled). Heat alone, with nothing burning yet, can also reach the same
trigger from the heat-transform path - lava resting beside it, or heat
conducted through stone or metal, once the wet stage below has steamed
any moisture off.

Either path writes code 7, the cell's **lit** state (`GUNPOWDER_LIT`), in
place of the plain `MAT_FIRE` a less flammable fuel would get. A lit cell
is a heat source in its own right, exactly like a burning log:

- it ignites neighbouring dry powder, so a trail burns along, cell by
  cell
- it can boil adjacent water
- it counts down its own `burn_decay` (16, roughly sixteen steps of
  fuse) every step, via the same `tick_decay_at()` wood already uses

That countdown is generalised by `reaction_t.lit_from`, the first
variant code a `burn_decay` material treats as "burning" (wood: 1;
gunpowder: 7, since gunpowder's other six codes are already spoken for by
dry tone and moisture).

A lit cell is not smothered by its own neighbours the way a buried wood
fire would be - `explodes != 0` opts a material out of that check,
because gunpowder carries its own oxidiser and a fuse buried in the
middle of a pile has to keep burning regardless. Water quenches a lit
cell to **soaked** (moisture pinned at `moist_max`), not to the unlit
code, or it would simply relight from an adjacent lit neighbour on the
very next step.

**Burning out: the blast and its cooldown.** Only when a lit cell burns
out - its countdown reaching `lit_from` - does the blast radius
(`reaction_t.explodes`, 20 cells, `SAND_GUNPOWDER_BLAST_RADIUS`) get read
at all.

If the cell is one corner of a 2x2 whose other three cells (the board
edge counts as not-lit) are also lit gunpowder, and the impulse buffer is
live, it detonates (`sand_explode()`). Otherwise it simply becomes an
ordinary `MAT_FIRE` cell, the same no-buffer fallback the confined-gas
blast already relies on. A one-wide trail or a lone lit cell never blasts
at all - there is no lit 2x2 to be part of.

A thick pile's blasts land one at a time, spread across several frames,
rather than all going off together - guaranteed by
`SAND_GUNPOWDER_BLAST_COOLDOWN` (8, board-wide, `sand_reactions.c`), the
number of steps the board waits after a detonation before another may
fire, ticked down once per reactions pass. Raising it spreads a pile's
blasts further apart without making any one of them smaller; 0 lifts the
limit.

A blast also takes the lit cells around it out of every 2x2 they belonged
to - they are fire or flying grains by their own burn-out - which helps
the spreading-out along independently. That is a side effect of the
geometry, though, not what bounds the cost: the cooldown does that.

The 2x2 rule started as a fully-lit 3x3 and was loosened after device
testing: with independent burn-out rolls, the neighbours lit before a cell
are usually already fire by the time it goes, and blasts became rare
enough to look broken. Two designs before that were tried and measured no
cheaper: an immediate per-cell blast on ignition, and a boundary-only
check. The 2x2 fuse model is what shipped.

**Moisture damps ignition.** For any `dries != 0` material, the ignition
roll is scaled down per moisture level: `f >>= SAND_DAMP_IGNITION_SHIFT *
moisture` (shift 2), so gunpowder's 200-in-256 base chance runs
200 -> 50 -> 12 -> 3 -> inert at moisture 4.

A fully wet charge (moisture pinned at `moist_max`, 4) cannot ignite at
all until something dries it out - either heat driving a level off as
steam (the same wet-earth stage `try_heat_transform_given()` already uses
for dirt), or simple time (`dries = 1`, half dirt's own rate of 2 -
powder holds water longer than soil does).

A saturated cell additionally has a small chance per step
(`soaked_to`/`soaked_chance`, 16 in 256) to give up being powder
altogether and become a full `MAT_OIL` cell instead - the same "one grain
plus its water becomes one liquid cell" shape other saturation reactions
already use.

That rate was tuned once against a measured target: on pre-saturated
powder sitting under standing water, the board took 1,542 steps to lose
half its powder to oil at a chance of 8, and 835 steps at 16 - the
shipped value.

Acid dissolves gunpowder at the same rate it dissolves sand
(`dissolvable = 200`); nothing about being explosive changes how a cell
disappears once acid is what is touching it.

**Gunpowder is not soil.** Every plant/root site that used to test
`dries != 0` to mean "this is ground a root can use" now tests a separate
field, `reaction_t.soil`, instead. Dirt sets `soil = 1`; gunpowder does
not, even though it has a moisture codec of its own and would otherwise
match every one of those checks.

Moisture diffusion between same-species cells and percolation still read
`dries`, unchanged - "can this variant mean wetness" and "is this ground"
are genuinely different questions once more than one material can be wet.
A fuse sitting in a garden bed was never meant to be something a tree
could root into, drink from, or drain moisture out of.

### Heat banking on stone and glass

Stone and glass bank heat in the low nibble their `KIND_STATIC` never
otherwise needed, 0-15 with `SAND_AMBIENT_HEAT` sitting in the middle
rather than at the floor. That way a pane has somewhere to go both when
it warms and when it chills (`step_one_tempered_cell()`, `sand_reactions.c`).

Left alone, that variant relaxes back toward ambient on its own, at a
rate (`reaction_t.cools`) that gets harder to outrun the further off
ambient the cell already sits. A single brush of fire makes a pane
fragile quickly, while cooking it all the way to molten stays a long
exposure.

The full mechanism (`heat_ramp`, `cools`, `chills`, shattering) is
covered in [Temperature: glass, snow, and a scale that has room for
cold](#temperature-glass-snow-and-a-scale-that-has-room-for-cold) below;
this section is only the two reactions that touch it from outside -
quenching and lava.

**Quenching multiplies the drain, but only downhill of ambient.** A
quenching liquid (`PAIR_QUENCHES` - water and acid, never lava or oil)
sitting against a heat-ramping cell multiplies the ordinary cooling drain
by `SAND_WET_COOLING_FACTOR` (8, `sand.h`), but only in the above-ambient
half of the ramp. Pouring water on a glowing wall is what brings its
banked heat back down in a reasonable number of steps, rather than the
many dozens plain ambient cooling alone would take.

The asymmetry is deliberate: nothing about being wet can push a cell
*below* `SAND_AMBIENT_HEAT`. That is chilling's job alone (snow, ice,
`reaction_t.chills`), and letting water do it too would let an ordinary
splash thermally shock glass the same way a deliberately-placed snowbank
does.

**Lava quenched into stone can take a neighbour down with it.** The
one-touch quench any water-on-lava contact has always done
(`neighbor_quenches()`, `quench_to`).

It also rolls a small, deliberately rare chance
(`SAND_LAVA_COOLOFF_CHANCE`, sand.h, 32 in 256) to freeze one further
adjacent lava cell too, which rolls again in turn - an iterative walk
(`cool_off_chain()`, `sand_reactions.c`), bounded at
`SAND_LAVA_COOLOFF_MAX_CHAIN` (8) links so one lucky roll can never run
the whole pool to stone in a single event.

This is what lets a sustained pour eat into a pool rather than only ever
sealing its surface. Stop pouring, and the crust that formed simply sits
there - nothing about it keeps spreading on its own.

The same small chance also gates the actual *work* of melting a neighbour
into something else - sand fusing to lava, a thermal-shock crack, a real
material change, not merely banking one more level of heat (which stone
and glass do on nearly every step they touch lava, for free).

Getting this distinction right mattered enough to be its own guarded test
(`test_a_lava_pool_in_a_dry_stone_bowl_does_not_freeze_itself`,
`suite_sand_reaction_encoding.c`): gating on whether the heat-transform
probe merely *returned true*, rather than on whether `CELL_MATERIAL`
actually changed, would have made a lava pool sitting in an ordinary
stone bowl slowly self-extinguish with no water and no fuel anywhere on
the board.

### A sufficiently covered lava cell can burst

Independent of water: a lava cell with a complete gravity-relative lid
over it (`covered_at()`, `sand_priv.h`) gets a tiny, deliberately rare
per-step chance (`SAND_LAVA_BURST_CHANCE`, 1 in 256 - the rarest a
single byte-wide roll can express) to convert to `MAT_STONE` and
immediately `sand_explode()` at that spot, fire included. This reopens a
sealed pool's own crust, so a sustained pour can keep reaching lava rather
than the pour's own cool-off chain armouring the surface shut.

A lid is not the same test as `smothered()`'s all-four-cardinals rule - a
pocket with an open side still qualifies, which is what lets an ordinary
hand-drawn vessel actually reopen over time rather than needing a fully
sealed cell to do anything.

#### The shared "am I covered" primitive

`cover_mask()`/`covered_at()` (`sand_priv.h`, beside `ring_dir()`/
`ring_of()`) are a general "is there a lid over this cell" primitive, not
a burst-private helper; the confined-gas ignition check is a candidate to
migrate onto it.

The lid is the **three cells centred on anti-gravity**: the cell directly
opposite gravity and the two diagonals either side of it. A cell is
covered when **all three** are covering (non-liquid, strictly denser, in
bounds - the board edge is never a container):

```
gravity points down; the lid is the three cells marked L

        L   L   L
          \ | /
            X      <- the cell being asked about

  what is BELOW X supports it; what is BESIDE X walls it in;
  neither one covers it
```

Gravity is read from the *settled* direction (`s->last_load_dx/dy`, stable
while the board is held still), not the per-step dithered one - dithering
would swing the lid between two adjacent orientations every step a tilt
fell between two eighths, turning the rule into orientation noise.

This shape replaced an earlier five-cell **semi-disc** (the same three
cells plus the two perpendiculars, needing three covered in a contiguous
run). The perpendiculars were the defect.

A finger-drawn stone wall bulges one cell past itself every brush step,
giving its inner face a notch. Lava settling into each notch saw wall to
its side, wall on the diagonal above that side, and wall directly above -
three cells, contiguous, "sealed" - while the pool's surface sat wide
open one cell over. Every hand-drawn basin blew its own sides out as the
lava settled.

Measured: a clean one-cell wall never produced a single eligible cell in
5,000 steps (no test caught it, since every basin in the suite is drawn
clean); a brush-drawn one did within 16 steps and breached by step 62 at
natural odds. Dropping the perpendiculars removed every eligible cell in
that scene at brush radii 2-4, left the wide-pool-under-a-crust case
exactly as it was, and is cheaper besides: three probes, no count, no
contiguity walk.

The one shape it still fires on, that a player might not expect, is a
two-cell-wide overhang, where the innermost cell does have a complete
lid. See `test_lava_in_a_wall_notch_never_bursts` and
`test_cover_primitive_matches_the_exhaustive_shape_table`
(`suite_sand_lava_burial.c`).

**`sand_explode()` fills a core of radius `radius / SAND_EXPLODE_CORE_
DIVISOR` with fire before it queues a single flight entry.** At
`SAND_LAVA_BURST_RADIUS` (12) and divisor 5, that core radius is 2, so
the `MAT_STONE` this feature just wrote at the centre is immediately
overwritten by fresh fire - pinned, expected behaviour (see
`test_buried_lava_bursts_into_stone_and_fire`,
`suite_sand_lava_burial.c`), not a bug.

The radius itself moved twice before settling. It started at the
confined-gas burst's own figure (8), then was raised to 16 once a
radius-8 burst read as a barely-visible flicker on device (core radius 1,
about five cells of flame).

It was then brought back down to 12 once gunpowder existed: the one
material whose whole point is to go off (`SAND_GUNPOWDER_BLAST_RADIUS`,
20) should own the biggest reaction-driven blast on the board. 12 still
gives a core radius of 2, about thirteen cells of flame, well past the
original flicker.

Not gated on `sand_enable_impulses()` having been called: `sand_explode()`
is a documented no-op without it, so with impulses off the cell simply
becomes stone and nothing is thrown - correct, since no impulses means no
explosions anywhere else in the simulation either.

**The rate is per covered cell, per step, not per pool or per event.** A
figure that reads as vanishingly rare in isolation is common in
aggregate, because a large sealed pool has many covered cells, each
independently rolling every step it stays covered.

A stress-tested "pool under a hand-drawn floor" scene (many one-cell
dimples across a wide ceiling, packed tighter than the blast radius)
confirmed the mechanism is self-limiting rather than a runaway chain: a
burst's own explosion destroys the cover around it as it clears the
pocket, so a freshly-uncovered neighbour is usually blown open rather
than left standing and re-eligible.

At the real production chance the scene lost roughly a sixth of its lava
over 3,000 steps in a slow trickle, never more than a handful of dimples
in a single step. Pinned to the maximum chance, the same scene lost about
half its lava in the first 20 steps and then plateaued as the remaining
pockets thinned out and scattered.

The tapering is an emergent consequence of the blast itself clearing
cover, not a cap anyone added.

## Temperature: glass, snow, and a scale that has room for cold

Four fields, kept off `reactions[]`'s main table because they describe a
*quantity* a cell carries rather than a reaction that fires once. Only
glass and stone carry a temperature today (`heat_ramp != 0`); glass, snow
and ice (`MATX_ICE`) act on one - snow and ice both chill a neighbour and
thaw in any liquid, at their own separate rates.

| Field | On | Meaning |
| --- | --- | --- |
| `heat_ramp` | glass, 64 | chance/256 per step per adjacent heat source to climb one level. Non-zero is what makes the variant a temperature rather than a shade |
| `cools` | glass, 5 | chance/256 to move one level towards `SAND_AMBIENT_HEAT`, **scaled** by how far above ambient the cell already is |
| `chills` | snow, 40 | chance/256 to pull a level out of a neighbour that has a temperature, down to 0; non-zero also marks the material cold |
| `shatters_to` | glass -> sand | on contact, no roll, in either direction: at or above `SAND_SHOCK_HEAT` when something cold touches it, or at or below `SAND_SHOCK_COLD` when heat reaches it |
| `thaws` | snow, 4 | chance/256 per step per adjacent liquid cell that it gives up and becomes `heats_to` |
| `SAND_AMBIENT_HEAT` | 3 | not a field - where room temperature sits on the 0-15 scale, so cold has somewhere to go |
| `conducts >> SPREAD_SHIFT` | glass, 220>>1 = 110 | chance/256 that a cell off ambient drags a same-material neighbour one level towards itself, only across a gap of 2 or more |

**`heat_ramp` and `heat_chance` are alternatives, not partners.**
`heat_chance` is a memoryless roll - sand fuses to glass the first time it
wins one, nothing remembered between attempts. `heat_ramp` banks progress
in the cell instead, the only way to express *sustained* exposure: under
a memoryless roll, a candle lit for one step a day would melt a pane
exactly as surely as a furnace, just later.

`cools` is what makes the ramp measure duration rather than lifetime
total - without a drain, exposure would only ever accumulate.

**`chills` and `cools` do the same thing in the same units and are still
two fields**, because they sit on different materials and cannot share a
number: `cools` belongs to the hot one and drains it to nothing, `chills`
belongs to the cold one and drains a neighbour - snow's 40 against glass's
5-6 is what lets a snowbank win a race that ambient cooling alone would
lose.

**The drain scales with distance from ambient** so that one constant can
serve two jobs that pull opposite ways: getting a pane *warm* stays easy,
and getting it *molten* stays hard. A flat drain cannot do both at once -
raising it enough to make melting reachable in reasonable time also makes
a pane fragile almost the instant a flame touches it.

**Room temperature sits in the middle of the scale, not at the bottom**,
entirely so cold has somewhere to be seen: with ambient at 0, chilling a
resting pane would change no number and so no colour, and a snowbank
sitting on glass would look identical to a snowbank sitting on nothing.

With ambient at 3, 0-2 is frost - pale, near white - and it fades on its
own, because `cools` moves a cell *towards* ambient from either side, not
only downward.

Chilling is driven from the **cold** cell (the way fire reaches its
neighbours), not the warm one, so a pane at rest still gets chilled by
snow sitting on it; a warm-cell-driven design only gets a turn when the
pane is already off ambient.

**Temperature spreads along the material itself**, not only from
whatever heat source touches it - `conducts >> SPREAD_SHIFT`, applied
*within* the material rather than only to whatever is on the far side of
it, so a chilled cell drags its neighbours down and a heated one pulls
them up.

Two things keep this from erasing the mechanic it is meant to support.
It is heavily scaled down (`SPREAD_SHIFT` is 1, so 110 in 256 rather than
220 - at the full value a pane goes isothermal within a step or two, and
a wall that is all one temperature cannot be hot inside and cold at the
rim). And it is gated to a gap of 2 or more - a difference of one is left
alone, so a smooth gradient across a wall survives rather than collapsing
flat.

It is derived from `conducts` rather than given its own field because it
is the same physical property - a material that carries a fire's heat
well carries its own temperature well too.

**Chilling something above room temperature costs the cold material its
own `heats_to`**; pushing cold into something at or below room temperature
costs nothing, because nothing was absorbed. Without that asymmetry, snow
would melt on contact with ordinary cold glass at the rate tuned for
standing beside a fire, making a snowbank impossible to keep anywhere near
the material it exists to be used against.

**Shattering is instant, not gradual, and converts the whole connected
run of the material at once**, up to `CRACK_MAX` (256) cells - a pane
breaks as a pane, not grain by grain, because the stress a crack releases
belongs to the whole sheet rather than to the one cell that started it.

The crack follows material identity (two panes not touching are two
panes) and does not re-check temperature as it spreads - the test
belongs only at the cell where the crack starts.

Because the trigger is instant, and the two directions (cold-onto-hot in
`step_one_cold_cell()`, hot-onto-cold in `try_heat_transform()`) share
one threshold each, **the player has to be able to see which side of the
line a pane is on**. Glass's palette is not a smooth ramp but a flat
neutral over the safe levels that jumps into a glow right at
`SAND_SHOCK_HEAT`, so the largest colour change on the whole ramp lands
exactly on the threshold.

A `_Static_assert` ties the constant and the palette together, and a
test asserts the ramp's widest colour step still lands on it.

## Roots: how a tree stays anchored, and how the root system it grows takes shape

The plant/wood/leaf family (`MATX_PLANT`, `MAT_WOOD`, `MATX_LEAF`) is not
otherwise covered in this document - its growth, hardening and budding
rules live in the extensive comments on `extended_reactions[MATX_PLANT]`
and `reactions[MAT_WOOD]` in `material.c`, which is the source of truth
for how a tree grows tall, thickens, and buds new limbs.

This section covers one narrower piece: how a tree stays connected to
the ground it drinks from once the ground itself starts moving, and the
shape that grows out of that connection.

**The problem.** Dirt is a powder and shifts. A tree finds water by
walking down its own stem to the ground and on down into the soil
(`find_water()`, `sand_plants.c`) - and when the soil directly under the
tree's collar (where the trunk actually touches ground) slides away, that
walk finds neither more stem nor ground below it and simply returns
failure. The tree is stranded, sometimes with plenty of water two rows
down, because the one cell it needed to reach it is gone.

### How a root forms

As a plant or a trunk spends the soil moisture it grows, buds or
sprouts on, there is a small chance (`reaction_t.roots` on the PLANT/WOOD
rows, 40 in 256) that the **contact cell** - the collar itself, not
wherever the moisture actually came from - welds into a `MATX_ROOT` cell
instead of staying an ordinary grain of dirt.

A root is `KIND_STATIC`: it does not fall, slide, or get displaced the
way loose dirt does, so once a collar has rooted, nothing about the bed
shifting can carry it away from under the tree.

This roll fires **only once per tree**, gated in `spend_soil_moisture()`
to `root_depth == 0`, meaning `find_water()`'s stem walk crossed no root
at all on the way down. Once the first root exists, growth (below) takes
over shaping the system; without that gate, every later grow/bud/sprout
event would keep re-rolling here too, seeding fresh disconnected root
cells at whatever the current deepest contact happens to be, fighting the
growth rule over the same collar.

### What a root does once it exists

A root cell is not otherwise inert - it eats. Every step,
`step_one_rooting_cell()` scans its own eight neighbours for one that is
dirt and still holds moisture, rolls a small chance (`reaction_t.roots`
on `MATX_ROOT`'s own row, 8 in 256 - the same field, a second reading of
it), and converts it into more root.

The conversion *is* the water cost: `place_reacted()` overwrites the
whole cell with a fresh root byte, so the dirt's moisture nibble is
simply gone along with everything else the cell used to be, rather than
being separately debited.

```
a root eating outward:

   . . . .        . . . .        . . R .
   . R . .   -->  . R R .   -->  . R R .
   . . . .        . . . .        . . . .

   R = root, . = moist dirt      each step, one eligible
                                  neighbour rolls into root
```

Almost no direction weights are needed, because moisture itself already
has a shape: it percolates down through a bed and diffuses out from
anything drinking or pouring nearby. A root that simply reaches for
whichever neighbour still has water in it spreads wide near a wet surface
and fingers downward through a bed drying from the top.

The first cut had no weights at all on exactly that reasoning. On the
device, the sideways spread near a wet surface won so completely that
depth only happened at the angles the geometry favoured.

So there is one small skew, read off the grid with no state: a candidate
that continues **away** from the cell's own root neighbours weighs +2,
and one that reaches **gravity-ward** also weighs +2, both over a base of
1 (`ROOT_WEIGHT_AWAY`, `ROOT_WEIGHT_DOWN`, `sand_plants.c`).

A tip has one parent and keeps going the way it was going - what
`holds_line` does for a stem, without remembering anything. A junction's
neighbours partly cancel and it is free to turn. The trunk a root grew
from counts as a parent too (`clings_to`), so the very first root under a
tree heads down and out from under the wood rather than tossing a coin
along the wet surface.

Measured over thirty seeds (six was per-seed scatter of +/-10 rows): mean
deepest root 3.5 rows with the weights zeroed, 7.2-7.4 with them; systems
about twice the size (22 roots against 42), because a directed tip keeps
finding fresh moist cells instead of re-hitting the crowd.

**Depth is bounded by water, not by the weights.** Raising the gravity
term and adding the trunk term narrowed systems (mean half-width 8.5 ->
7.9) and did not deepen them (7.2 -> 7.4) - after 20,000 steps every
saturated bed in the harness was 98-99% dry and not one live tip had
moist dirt beside it.

A root can only eat moist soil, a bed watered from the top dries from the
top, and the moist front the fingers chase is gone before they reach the
floor. The weights decide which moist cell a tip takes next; how deep the
water goes decides how deep the roots can.

### The roots carry the water down

A root cell is also a **conduit** (`step_one_conducting_cell()`,
`ROOT_CONDUCT_CHANCE` 64): each step it may move one level of moisture
from the wettest soil beside or above it into the driest soil beneath it.
Moves only, never makes - the same conservation percolation keeps - and
one way only, gravity-ward, so it cannot ping-pong against diffusion.

The sink is the next cell a tip wants to eat, and a fresh tip is itself a
conduit, so the moisture front and the root front move down together:
depth is earned a level of water at a time.

Sides count as sources, not only "above" - a column has more root above
each cell, not soil, so only its top cell would ever conduct otherwise.
Drawing from the wet soil flanking each cell is what lets a whole column
drain the surface layer downward.

Measured on a dry bed watered at the collar only (the device case), ten
seeds, mean deepest root of 19 rows, for three percolation rates:

| `SOIL_PERCOLATE_CHANCE` | Conduit off | Conduit on |
|---|---:|---:|
| 15 (current) | 4.6 | 15.0 |
| 30 | 7.0 | 18.0 |
| 60 (the old value) | 5.3 | 15.1 |
| 15, on a saturated bed | 9.6 | 15.4 |

Conduction is worth roughly three times what the percolation rate is -
even the old, fast percolation only reached 5-7 rows without it - so
slowing `SOIL_PERCOLATE_CHANCE` down did not make roots shallow, and
undoing it would not make them deep.

### What actually bounds the system's size

Three things, in the order that matters:

1. **The moisture itself.** A cell with nothing to spend has nothing to
   grow into, and the total on a board is finite unless something keeps
   pouring more in.
2. **`ROOT_SURFACE_MAX`, 2.** A root already touching more than 2 other
   roots does not roll to grow at all. This is what actually gives the
   system its shape - without it, a well-watered bed converts every
   moist cell it can reach into a solid slab of root rather than a
   filigree of it.
3. **`reaction_t.roots` itself, 8 in 256 on the root's own row** - a
   small chance, the same discipline every roll in `sand_plants.c`
   follows.

`ROOT_SURFACE_MAX` was measured against a runaway scene: a root
pre-planted so the rare first-root lottery cannot confound the reading,
its collar rewatered to `SOIL_MOISTURE_MAX` every step for 20,000 steps,
root count sampled every 2,000.

At `ROOT_SURFACE_MAX = 1` the system starved itself shut at 4 cells (a
bare stub). At 3 it never stopped growing - 99 roots by step 2,000, still
climbing at 220 by step 20,000. At 2 it climbed to the low forties by
step 2,000 and then sat there **byte-identical** through the remaining
18,000 steps, seed after seed - a genuine fixed point.

No depth or spread cap turned out to be needed. An earlier walk-shaped
design (reusing `step_one_growing_cell()`'s stem-walk machinery, with its
own depth cap) was replaced by this local eating rule, because a root
does not need a stem's machinery to look like a root.

Eating rests on the same scarce-resource philosophy the rest of this
feature already uses, rather than adding depth and spread caps as a
second, separate kind of bound beside it. `ROOT_SURFACE_MAX` alone
already produces a genuine fixed point at the scale this feature runs
at.

**Eight neighbours, not four.** `step_one_rooting_cell()`'s scan walks
all eight ring directions rather than the four cardinals
`reaction_dirs[]` uses elsewhere in this file.

Compared directly, both ways, over the same six seeds: four gave a
near-straight taproot, one or two cells wide, that only fanned out where
moisture happened to pool against the stone floor; eight let a root step
diagonally as it reaches for water, producing the wandering, forking
shape a root system is supposed to have. The difference was qualitative,
not a rounding error.

### Shade follows structure, not age

A root darkens from fresh tan toward a wood-like brown (`ROOT_OLD`,
`material_palette.c`) as more root grows around it: the painter hands
`material_colours()` the count of root neighbours in the `depth` slot
only a liquid's interior otherwise reads (`material_root_neighbours()`,
`material_palette.h`), and that count picks one of `ROOT_SHADES` steps.

A tip touching one other root wears the fresh colour; a cell that has put
out children steps darker; the collar, touched on most sides, wears the
darkest.

Not a lifetime, on purpose - an age would darken the tips too, and the
tips are the part meant to stay fresh. Lose a child to rot or lava and
the parent lightens again.

The eating rule above is what makes this visible at all: a straight
column is almost entirely two-neighbour cells, while a branching system
is full of the junctions the darker steps are keyed to.

### What the growth rule adds, measured

A fixed scene (60 wide, 70 tall; stone floor; 20 rows of saturated dirt;
one seed on the surface; the 13 cells around the collar rewatered every 20
steps; 20,000 steps; `sand_step(&s, 0, 1000, 0)`), run twice - once with
only the welding rule active, once with welding plus growth:

```
                     WELDING ALONE                 WELDING + GROWTH
seed    roots depth  half-width  wood      roots depth  half-width  wood
11        7     3        7       141         0     0        0       83
909       4     2        3       82         35    19        9       88
4242      3     2        1       78         48    19       12       69
77        1     1        1       71         76    19       13       58
5150      7     2        9       148        55    19       12      298
31337     2     2        2       116        29     8        7       104
```

Welding alone gives a short column near the collar, one to seven cells,
starved by how rarely a single un-replanted tree spends soil moisture at
all. With growth, the same scene grows a genuine branching system
reaching most of the bed's own depth, spreading well past the collar.

Seed 11 growing zero roots either side is not a regression - the welding
roll (~16% per eligible spend, and a single tree spends only a few dozen
times in its life) simply missed for that seed within 20,000 steps. The
suite's own tests replant every 40 steps specifically to give that roll
many independent tries, the way this raw harness does not.

One seed's soil, picture (`seed 4242`, `R` root, `W` wood, `.` dirt,
collar near the top centre):

```
.............................RR.RR..........................
.............................RR.RR..........................
.............................R..............................
.............................R..............................
.............................RR.............................
.............................R..............................
.............................R..............................
.............................RR.............................
.............................RR.............................
..............................R.............................
..............................R.............................
..............................RR............................
..............................R.............................
..............................R.............................
..............................RR............................
..............................R.............................
..............................R.............................
..............................RR..R..R.R.RR..................
..............................RRRRRRRRRRRRR..................
```

Wide near the collar, a wandering single-cell thread through the middle of
the bed, and a wider fan again at the bottom where moisture pools against
the stone floor - the shape moisture's own distribution gives it, for
free, with only the small away-from-parent and gravity-ward skew above
laid over it.

### Why a new material, not more wood

The obvious shortcut - give wood a "rooted" variant, the way glass spends
its variant on temperature - does not work, because wood's variant is
already spoken for: it is burn progress (`reaction_t.burn_decay`), and
`CELL_VARIANT(n) != 0` is what "on fire" means throughout the reactions
pass. A root wearing a wood variant would read as a nearly-burnt-out log.

Being its own material also buys two things a wood-based encoding could
not.

First, a root does not count against `TREE_LIFT`: `find_water()` tracks a
separate `root_depth` purely so it can **not** add to `lift` - a cell
below the water line lifts nothing, and charging it anyway would let a
handful of root cells eat a real share of every tree's height budget for
free.

Second, the stem walk can tell a root apart from ordinary ground well
enough to treat one buried under freshly-shifted dirt as transparent
rather than as a second dead end - reintroducing the very bug this
feature exists to fix, from the other side.

Flammability is zero on a root's own row, deliberately - not an omission.
A root is buried, and a fire that could reach down and burn out a tree's
own anchor from under it would undo the whole point of the feature: the
tree would be exactly as vulnerable to a shifting bed as it was before
roots existed, just one fire away.

### A root competing with its own tree

The growth rule created one new and genuinely surprising failure mode: a
root sitting directly on its tree's only reachable water can, given
enough steps, eat that exact cell itself and convert it to more root.
If nothing lies beyond it but stone, the tree's own water access is gone,
spent on growing the root system instead of ever reaching the trunk
above.

This is not a bug in `find_water()`'s transparency (untouched by root
growth, and still correct); it is root and tree genuinely competing for
the same scarce moisture, first roll wins.

`test_a_buried_root_does_not_cut_off_the_water_below_it`
(`suite_sand_roots.c`) used to rest on a single row of water directly
under the root, and the growth rule made that scene racy against this
exact competition (measured: failed, deterministically, for the suite's
fixed seed). The fix was a deeper wet reserve below the root, not a
change to the mechanism, since a real root system does not get to
consume literally every cell of water below it before the tree it
belongs to can use any.

## Performance discipline

Every number below came from `esp_timer_get_time()` on real hardware, via
the device-only tests in `suite_sand_perf.c` (`#ifdef DEVICE_BUILD`), not
estimated. A captured budget is a fact about one build on one board at one
point in time - `docs/sand/Testing-Sand.md` is where the last full capture
lives; the table below is the shape of the argument, not a promise the
exact microseconds still hold.

| Scenario | Cost | Budget |
|---|---|---|
| Settled screen of sand | ~19-24 us | (dwarfed by anything else) |
| Full-screen sand step, worst case | ~5.5-6 ms | 8 ms |
| Water cross-flow + rebound, worst case | up to ~15 ms | 16 ms |
| Full-screen panel blit | **~17 ms** | (fixed hardware cost) |

The blit dominates. One raw `esp_lcd_panel_draw_bitmap()` of the whole
framebuffer measures **16,998 us of bus time against 18,147 us for a
full `gfx_present()`** - the dirty-tracking path's own overhead is
1,149 us, 6%, and the frame is **94% bus-bound** (at 40 MHz). 80 MHz
halves it but is outside the panel's rating.

Sand's partial redraws are exactly what shows it: stray red pixels or
thin black lines through moving sand that stay until that region is
re-sent differently. See
[Display-and-Rendering.md](../notes/Display-and-Rendering.md), "The blit
is bus-bound", for the finding and the heal sand uses.

Water, not sand, is the bottleneck whenever a body of it is moving.

Three techniques account for most of the gap between "walk every cell
every step" and the numbers above:

- **Block sleeping.** A block that produced no movement under the current
  gravity direction, and none of whose neighbours moved either, is skipped
  entirely next step (`block_state`, in `sand.c`). Motionless sand costs
  roughly 1,000x less than the same sand while it is actually falling. See
  [Architecture.md](Architecture.md#block-and-row-sleeping) for why it is
  block-shaped rather than row-shaped, and how the block dimensions were
  chosen.
- **Not every skip structure earns its keep.** The liquid pass had one of
  its own for a long time - `ROW_NO_LIQUID`, a per-row "scanned and found
  dry" flag - deleted once the device measured the bookkeeping that kept
  it honest (a three-byte row_state wipe on every move of every material,
  anywhere on the grid) as costing far more than the row scans it avoided:
  a screen of water went from 17,860 us a step to 13,130 just from
  removing it. Worth reading before adding another one.
- **Bitmasks over flash-table reads, inside a hot loop.** Asking
  `materials[id].kind` per cell means a flash read every time.
  Precomputing a 16-bit "is this id a liquid" bitmask once per pass
  instead avoids that entirely, and it measurably mattered: it alone was
  the difference between a settled screen of sand costing 17 us and
  costing 5.5 ms.

`materials[]` is `const` data in flash, read through this chip's 32 KB
data cache - kept separate from the 16 KB instruction cache the sweep's
own code lives in, so the two no longer evict each other. A cache miss on
a cold line is still a real cost inside the tightest loop in the project,
which is what the bitmask above avoids paying per cell. See
[Optimization-Playbook.md](../notes/Optimization-Playbook.md#know-what-kind-of-memory-you-actually-have)
for the cache sizes and the general lesson.

## Two cores: a checkerboard sweep, and what stays serial

The device's own `present()` overlaps with `sand_step()` on the other
core already - see `docs/Launcher-Architecture.md`. Splitting the step
itself across cores is harder, for a reason that has nothing to do with
sweep order: `sand_t.rng` is one `xorshift32` word, drawn from a
data-dependent number of times per cell by every pass.

Two cores drawing from it at once race on that word; the fix is to stop
sharing it - see [The draw](#the-draw) below - which means the split can
no longer promise the byte-identical output the project held itself to
until this feature.

What it promises instead is **determinism**: the same seed gives the same
board every time, on any core, in any order, because no draw depends on
anything but (seed, step, cell, which draw).

The serial path is unchanged and still byte-identical - the fingerprint
suite still holds it to that - but a step run with
`sand_set_two_core_step(true)` is a **different, deliberately allowed**
simulation for the same seed, checked for its own determinism rather than
against the serial one.

### The reaches, measured

What can share a core-1 dispatch at all depends on how far one cell's
update can touch another's, in cells:

| Pass | Reach | Parallel? |
| --- | --- | --- |
| Main sweep (`step_one_grain`, `move_liquid_grain`) | 1 (Chebyshev - every move is one of the eight ring directions) | yes |
| Liquid cross-flow (`equalise_liquids`, `find_shallowest`) | `SAND_LIQUID_SIGHT`, 8, along a ray that can run diagonally through several rows | yes; private wake and repaint state |
| Gas walk (`gas_walk_once`) | 1, same shape as the sweep | yes; private wake and repaint state |
| Gas cross-flow (`equalise_gas`) | `material_of(c)->sight`: 5-24 rows across fire (5), gas (16), steam (20), and smoke (24); stripes would need 24-row guards | no |
| Heat conduction to a boiler (`try_heat_transform_given`'s `CONDUCT_REACH`) | 32, a directed walk, not a spread | no |
| Glass crack flood | up to `CRACK_MAX`, 256 | no |
| Lava cool-off chain | up to `SAND_LAVA_COOLOFF_MAX_CHAIN`, 8 links, each an arbitrary further cell | no |
| Explosions and thrown debris (`step_impulses`) | queued, crosses many steps, effectively unbounded | no |

The gravity sweep, gas walk and liquid cross-flow have fixed cell reaches
suitable for stripes. Gas cross-flow, reactions, liquid density sorting and
impulses remain serial. Reactions mix local rules with conduction and flood
walks; impulses can reach across the board.

### Stripes, not tiles

A 2-colour checkerboard of 2-D tiles was tried on paper first and
rejected: tiles diagonal to each other share a corner, and a reach of
even 1 cell can touch that corner from a same-coloured tile on the far
side of it - exactly the class of bug Noita's own write-up (GDC 2019)
solves with a 2x2, four-colour scheme and a per-cell "updated this frame"
stamp.

Stripes avoid that specific problem outright: a row-stripe has exactly
two neighbours, above and below, and colouring stripes by index means a
stripe's only neighbours are always the opposite colour, so two
same-coloured stripes are always a full stripe height apart - far past
the sweep's 1-cell reach. That does *not* mean the split needs no
boundary handling at all - see [The seam fix](#the-seam-fix) below for
the one it does need.

```
gravity down; stripe height = SWEEP_STRIPE_H (32 rows);
two phases, alternating colour, offset by half a stripe every step

  phase A (even stripes)     phase B (odd stripes)
  ┌──────────────┐           ┌──────────────┐
  │ stripe 0 (A) │  swept    │ stripe 0 (A) │  held
  ├──────────────┤           ├──────────────┤
  │ stripe 1 (B) │  held     │ stripe 1 (B) │  swept
  ├──────────────┤           ├──────────────┤
  │ stripe 2 (A) │  swept    │ stripe 2 (A) │  held
  └──────────────┘           └──────────────┘

  same-coloured stripes are never adjacent, so a 1-cell
  reach from one can never touch another being swept at once
```

`SWEEP_STRIPE_H` is `SAND_BLOCK_H` (32), reusing the sleep-tracking
grid's own row size rather than inventing a second one. Within a phase,
half the stripes run on a task pinned to core 1, the rest on the
caller's own core, joining before the next phase starts - which stripe
goes to which core does not matter, since none of them touch each other.

The stripe grid's own offset alternates by half a stripe height every
step (`s->step_phase & 1`), the same idea Margolus-style block automata
use to keep a boundary from sitting on the same rows long enough to
become a visible seam - `suite_sand_two_core.c`'s own seam test checks
exactly this, by histogramming a settled pile's row-to-row occupancy for
an outlier at stripe-boundary rows.

Below `SWEEP_CHECKERBOARD_MIN_ROWS` (four stripes' worth) the whole sweep
just runs on one core - a handful of stripes plus a hop to core 1 is not
worth it.

### The seam fix

The serial sweep's no-double-move guarantee rests on one property: every
row's possible destinations were already visited this step, so a grain
that lands there is never picked up again. That property is per **grid**,
not per stripe - a stripe boundary sits inside it, not outside it.

Two adjacent stripes are always different colours, so one of a stripe's
two neighbours belongs to whichever phase runs second - and a move that
crosses into that neighbour's boundary row lands somewhere that phase has
not swept yet. Once it does, it finds the just-arrived grain sitting
there and moves it again: two cells in one step, at roughly half of every
seam, every step.

`run_sweep_stripes()` excludes both boundary rows of every stripe from
the phases entirely - `step_one_grain()`'s reach is exactly one cell, so
a boundary row is the only one a move could reach past a stripe's edge,
and excluding it removes the crossing outright.

That alone is not the whole fix: an interior row directly beside a guard
row is still swept **during** its own phase, using the guard row's state
from before that phase ran. In an unstriped sweep the guard row would
already have had its own turn by then; here it has not, so a grain that
the interior row pushes into the guard row gets a second, unwanted move
once the guard pass finally reaches it.

The guard pass fixes this by snapshotting every guard row's content before
either phase runs, then comparing: a column that still matches its
snapshot got no phase-time write and takes its ordinary turn; a column
that changed already moved once this step, via the interior row beside it,
and is skipped. That removes the double-move without needing the two
guard rows' exact place relative to every other boundary in the grid.

Exact serial order is, in fact, provably out of reach for a plain
two-phase split once three or more stripes are active: tracing the
dependency chain across two adjacent boundaries shows a middle stripe
needs to run before the top stripe at one boundary and after the bottom
stripe at the other - but the top and bottom stripes share a colour and
are meant to run as a single phase.

No reordering of "all of colour A, then all of colour B" satisfies both
constraints at once. The per-seam moved stamp above sidesteps the
contradiction rather than solving it: a safety fix, not an
order-equivalence one.

What that buys, and what it doesn't. `suite_sand_two_core.c` places a
lone grain exactly on a seam, under all four axis-aligned gravity
directions and both stripe offsets, and checks it travels exactly one
cell in one step, an open fall and a slide alike - and, with scatter
forced to zero, that this matches the serial path's own fall distance
exactly. That case has nothing else nearby to contend with, so the guard
pass's snapshot always matches and the fix is exact.

A dense column or pile crossing several boundaries at once is different:
the same scatter-zero comparison on a full falling column and a settling
slab shows the two paths' final boards are **not** byte-identical once a
contested chain spans more than one seam - exactly the scenario the
dependency-chain argument above rules out.

What the guard pass still guarantees there, and what the suite checks
instead, is that the grain count never drifts: nothing is duplicated or
dropped, only reordered by up to the width of a stripe boundary.

### Liquid cross-flow stripes

Cross-flow uses the same 32-row stripes, with 8 guard rows on each side of
every internal boundary - `LIQUID_STRIPE_H` is `SAND_BLOCK_H`, and the
guard width matches `SAND_LIQUID_SIGHT`, the furthest a cell can read or
transfer in one pass. The offset alternates between 0 and 16 rows exactly
as the main sweep's does. Boards shorter than 128 rows, and scratch
allocation failures, fall back to the unchanged serial order.

A cell reads or transfers at most 8 rows away, but the bookkeeping
around it reaches further: depth-repaint marks extend another 24 rows
from a destination, and block wakes clear settled flags across a 3x3
neighbourhood. Those writes exceed the cell guards, so each worker owns a
private copy of the block flags, dirty spans, and its own movement and
probe counters, merged back in at the join.

An arrival bitmap prevents the guard pass from forwarding mass a cell
only just received during the phase it ran in. Isolated seam transfers
are serial-exact; contested pools can redistribute differently between
the two paths and are checked instead for exact mass conservation,
deterministic output, and no persistent seam jumps.

`tools/report_crossflow.sh` measures the liquid pass on host using the
shared water-slope and submerged-pile builders; every other pass stays
serial in that comparison, and the host worker itself dispatches inline,
so its timings measure overhead and changed work, not multicore speedup.
The device perf suite enables splitting for its own liquid tables;
`pass_us.liquid_us` there includes both phases, joins, metadata merges and
guards together.

### Gas walk stripes

The gas walk uses the main sweep's 32-row checkerboard and one guard row on
each side of a boundary. Gas rises, so its row order is the gravity sweep's
mirror: for ordinary downward gravity the guard above a boundary runs before
the one below it. A pre-phase snapshot keeps a gas received at the seam from
taking a second turn in the guard pass.

Block wakes and dirty spans extend beyond that one-cell guard, so both workers
write private copies and merge them after each phase. Boards shorter than 128
rows and scratch allocation failures retain the serial walk.

### The draw

`sand_rng_next_at(s, x, y, slot)` (`sand_priv.h`) replaces
`rng_next(&s->rng)` at every call site a split pass reaches -
`try_scatter()`, `try_slide_impl()`, `liquid_may_move()`, and the gas
decay, mobility and walk draws - while `s->rng_hashed` is armed. That is
true during the sweep, liquid cross-flow and gas-walk phases and guards.

Armed, it hashes `(s->rng_seed_base, s->step_phase, y * s->w + x, slot)`
through `rng_hash()` (`util/rng.h`); disarmed, it is `rng_next(&s->rng)`
unchanged, so reactions and every serial gas step retain sequential draws,
and the whole step with the switch off is unchanged.

`slot` is a fixed per-call-site constant (`SAND_RNG_SLOT_*`), not a
per-cell counter - a cell's scatter roll and its slide roll hash
different inputs because they are different constants, not because
anything counts draws. That is what makes a draw depend on nothing but
its own four inputs and nothing any other core is doing.

One case had no safe answer at all: `splash_displace()`'s hard-landing
splash queues into `s->impulse_buf`, one shared counter with no lock, so
it simply does not fire while `rng_hashed` is armed - a documented, narrow
behaviour loss rather than a race on that queue.

### Scheduling: below present, not around it

The core-1 task (`util/job.c`, shared by every engine client, not owned
by this app) runs at priority 3, below gfx's present task at 5 - not in a
window carved out before or after present, because `sand_step()` can run
while a previous frame is still presenting (`main.c`'s `step_app()`) and
present's own timing must never move for anything sand does.

A lower-priority task only gets the CPU while present is blocked on its
own strip-sent semaphore, which is most of a present since the transfer
itself is DMA, so present is never delayed and core 1 still does useful
work in gaps that would otherwise sit idle.

### Why the core-1 dispatch can never hang the shell

Nothing on this codebase's core-1 dispatch path may wait forever,
because `gfx_present_wait()`'s own `xSemaphoreTake(..., portMAX_DELAY)`
chain has no timeout anywhere in it either, and has always been one
wedged strip-sent interrupt away from hanging the whole frame loop. A
second task on core 1 must not risk exposing that same latent assumption.

`job_wait()` (`util/job.h`) takes a timeout instead - every sand call
site here passes 100 ms, far above any dispatch this file makes. A
timeout that fires falls back to inline dispatch only for as long as the
stuck job still holds the worker: the flag it leaves set routes every
`job_run_core1()` call straight down the inline path.

A later `job_wait()` takes the semaphore once that job actually finishes
and clears the flag - `job_reap_finished()` makes the same check on the
dispatch side - so core-1 dispatch resumes on its own rather than staying
disabled.

`job_run_core1()` copies its context into a static buffer before
returning, not merely pointing at the caller's, because a dispatch a
timed-out wait gave up on can still be read later by whatever core-1 is
doing, and a stack-allocated context would dangle the moment its caller
returned.

Device builds split the gravity sweep, the liquid cross-flow pass, and
`finalize_settling()` across both cores this way; host builds default to
the serial path, where `job_run_core1()` always runs its callback inline.
The unbounded waits in gfx.c's present pipeline are still unchanged.

## Why the liquid logic is its own file

`sand.c` and `sand_liquid.c` used to be one file. A complexity scan at the
time - a hand-rolled script since retired for going quietly blind on a
later reformat, see `docs/tools/Complexity-Gate.md` - found `sand_step()`
scoring far above Sonar's own "worth a look" line of 25: high enough on
its own to justify a split, though that tool cannot be trusted for the
exact figure and the file has been split twice more since, so there is no
reproducing it today.

Two things were already true at that point: `equalise_liquids()` and the
wall-rebound pass were both already separate *functions*, comfortably
over that same line themselves, just not yet a separate *domain*, since
both are liquid-only and already shared helpers with each other.

The split moved everything about a liquid that is **not** gravity-ward
(cross-flow, the rebound splash, the momentum accessors) into
`sand_liquid.c`, and extracted the one piece that had to stay in
`sand.c`'s sweep (`move_liquid_grain()`, since it obeys the same
gravity-ward guarantee every other move there does) into its own
function.

That extraction cut `sand_step()`'s complexity substantially - a real
complexity cut, not just relocated lines, because it collapsed nesting
that had been compounding the score.

`dest_row()` and `mark_rows()`, needed on both sides of the split, stay
`static inline` in a shared `sand_priv.h` rather than becoming ordinary
`extern` functions - both sit on the hottest path in the simulation, and a
call across translation units is not guaranteed to inline the way a call
within one file is. Confirmed on device rather than assumed: the
frame-budget tests above are what would have caught it if splitting the
file had cost anything.

The same reasoning later split `sand_impulse.c` out of `sand.c` too:
queued explosions, thrown debris and splash pushback move outward rather
than gravity-ward, so `step_impulses()` is called from `sand_step()`
exactly once, the same seam `sand_step_liquids()` and `sand_step_gas()`
use.

(The wall-rebound pass named above has since been removed entirely - see
[Performance discipline](#performance-discipline)'s neighbouring sections
for what liquids do today; this paragraph describes why the file split
happened, not a mechanism still in the tree.)

`sand_reactions.c` later split the same way: fire chemistry and the
tree/root/leaf growth system it also housed shared almost no call graph,
so the growth half moved into its own `sand_plants.c` - see that file's
own top comment for the rationale.

### Broken down further

Both were still well over Sonar's *default* line, which is 15, not the 25
used above - that line only ever applied to the standalone check, not to
what the project actually holds itself to. Both functions were later
broken down the same way again, one level deeper: nested per-cell and
per-row logic pulled into small named functions, until `sand_step()` and
`equalise_liquids()` themselves scored 15 or under. Newer code has since
pushed some functions - `sand_step()` itself included - back above that
line; `launcher/tools/complexity_baseline.txt` holds every function's
real, current score, not this paragraph, precisely so a claim here cannot
go stale the way this one already had.

The main sweep, per grain:

```mermaid
flowchart TB
    STEP["sand_step()"] --> ROW["step_one_row()<br/><i>once per row, gravity-ward order</i>"]
    ROW --> GRAIN["step_one_grain()<br/><i>once per grain in the row</i>"]

    GRAIN -->|"static or gas"| SKIP(("nothing to do"))
    GRAIN -->|"liquid"| LIQ["move_liquid_grain()<br/><i>sand_liquid.c</i>"]
    GRAIN -->|"powder, unblocked"| FALL["try_fall_or_scatter()"]
    GRAIN -->|"blocked, or shaken"| SLIDE["try_slide()"]

    FALL --> SCATTER["try_scatter()<br/><i>drift sideways, or lag</i>"]
    FALL -.->|"fall itself blocked"| SLIDE

    SLIDE --> ORDER["pick_slide_order()<br/><i>which side goes first</i>"]
    SLIDE --> PAIR["try_slide_pair()<br/><i>friction, then either slide</i>"]
```

The cross-flow pass, per liquid cell:

```mermaid
flowchart TB
    LIQSTEP["sand_step_liquids()<br/><i>after the main sweep finishes</i>"] --> EQ["equalise_liquids()"]

    EQ --> EROW["equalise_one_row()<br/><i>once per row that holds liquid</i>"]
    EROW --> ECELL["equalise_one_cell()<br/><i>once per liquid cell</i>"]

    ECELL --> ROOM["has_room_below()<br/><i>the common case - falls in the<br/>main sweep instead, nothing to do here</i>"]
    ECELL --> LOWER["neighbour_is_lower()<br/><i>next commonest - level already</i>"]
    ECELL --> FIND["find_shallowest()<br/><i>only reached along a real imbalance</i>"]
```

**The extraction was not free.** `has_room_below()`,
`neighbour_is_lower()`, `find_shallowest()`, `equalise_one_cell()` and
`give_mass()` are all on the per-cell path above, and none of them were
marked `inline` when they were pulled out - unlike `pour_into()`/`room_in()`,
the pair already living in that file.

A full screen of water went from the ~15 ms in the table above to 18 ms
against its 16 ms budget, caught directly by
`test_a_screen_of_water_fits_in_the_frame_budget` on device, not noticed
by eye.

Marking those five `inline` restored it. The lesson from the file split
above held a second time: a call this hot has to be confirmed on device,
not assumed free because the source now reads as several small functions
instead of one large one.

---

## Related

- `docs/Launcher-Architecture.md` - how an app (this one included) plugs
  into the shell; the folder layout every app follows.
- `docs/notes/` - the hardware constraints underneath all of this: the
  memory budget, the flash/RAM cache distinction, panel and touch gotchas.
  Start at `docs/notes/README.md`.
- `docs/sand/Adding-a-Material.md` - the practical how-to for adding a
  new material, worked through end to end against a real one (gas).
- `docs/sand/Shading-and-Colour.md` - how an existing material's variant
  actually becomes a pixel: the palette pipeline, the recurring shading
  mistakes and their fixes, and the one item still open (liquid depth is
  not yet gravity-continuous).
- [`Architecture.md`](Architecture.md) - a single-page, diagram-first map
  of the whole app: the pipeline, the sleeping system, the material
  table, and the exact hops to get a real number off the device.
- `docs/Testing-Guide.md` - how the host and device test suites work, and
  why release builds carry none of the test code.
- `docs/sand/Testing-Sand.md` - the frame-budget capture and the current
  state of `suite_sand_perf.c`'s numbers on this board.
- `docs/tools/Complexity-Gate.md` - the cognitive-complexity ratchet
  mentioned above: what it measures, what it cannot reach, and how the
  baseline it checks against works.
