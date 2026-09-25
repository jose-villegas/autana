# Sand App Docs

Touch pours material into a grid; on the board, tilt changes which way the
material falls. Water levels, powders pile, and heat and reactions change
what is in a cell. The host-rendered image below is the app's **menu**; the
moving simulation and tilt input need the board.

![Falling Sand title screen](../images/overview/sand-menu.png)

<!-- Regenerate: ./launcher/main/apps/sand/tools/sand_menu_render_host.sh -o <dir>; use title-landscape.png. -->

Start with [The Falling-Sand Simulation](Sand-Simulation.md) for the behavior
and its constraints, or [Architecture](Architecture.md) for a file and data
map. [Adding a Material](Adding-a-Material.md) is the change checklist.

The rest of this folder is a reference by topic:

**How it works today:**

- **[Sand-Simulation.md](Sand-Simulation.md)** — the app in depth:
  materials, the liquid model, gas and fire chemistry, temperature, the
  two-core sweep, the performance budget every design choice answers to.
- **[Architecture.md](Architecture.md)** — a single-page map of
  `main/apps/sand/`'s shape (the grid byte, the material table, the file
  split) rather than the reasoning behind it.
- **[Impulse-Mechanics.md](Impulse-Mechanics.md)** — explosions, thrown
  chunks, and a liquid's own splash: one mechanism, three call sites.
- **[Reaction-Table.md](Reaction-Table.md)** — generated, current
  material-interaction rules. Regenerate with
  `tools/report_reactions.sh`, don't hand-edit the generated region.
- **[Metal.md](Metal.md)** — metal: smelted out of dirt by lava, and the
  only material that moves heat a long way.
- **[Shading-and-Colour.md](Shading-and-Colour.md)** — how a cell's
  material and variant become a pixel, and the traps specific to that.
- **[Testing-Sand.md](Testing-Sand.md)** — the frame-budget capture, its
  free-heap precondition, the scoping rules specific to this app, and the
  chunk layout sweep behind the two-core geometry.

**How to change it:**

- **[Adding-a-Material.md](Adding-a-Material.md)** — the checklist for
  adding a whole new material.

## Related

- [`../plans/`](../plans) — plans that touch this app. Only
  `Reaction-Doc-Generator-Plan.md`'s brush-blurb phase is still unbuilt.
- [`../notes/README.md`](../notes/README.md) — the hardware constraints
  (the PSRAM and memory budget) this app's numbers are shaped by.
- [`../Testing-Guide.md`](../Testing-Guide.md) — how any of this gets
  verified, on host and on device; [`Testing-Sand.md`](Testing-Sand.md)
  is this app's own half of that.
