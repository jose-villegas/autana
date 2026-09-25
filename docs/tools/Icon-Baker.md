# Icon baker

`launcher/tools/gen/gen_icons.py` bakes icons from an atlas; the rules below
are the generator's. Reads a PNG atlas cell or an integer-grid SVG path per
icon (pixelarticons' shape: `M`/`H`/`V`/`h`/`v`/`Z` only) and emits one
`icons_<name>.h` per manifest - `gfx/icons_system.h` for the shared set,
`apps/<name>/icons_<name>.h` for an app's own. The shared type both
instantiate is `icon_t` (`gfx/icon.h`); drawing is `icon_walk_blocks()`
(streamed runs, not a collected buffer) and `ui_draw_icon()` (`ui/ui.h`).

## Ownership: one generator, two homes

Split by ownership, not by mechanism:

| | |
|---|---|
| `design/icons/system.png` → `gfx/icons_system.h` | vocabulary any app means the same way: check, close, chevrons, info, back |
| `apps/<name>/icons/<name>.png` → `apps/<name>/icons_<name>.h` | that app's own artwork; deleting the folder takes the art, the manifest and the baked header with it |

Same generator, same format, same tests.

## What the generator rejects before emitting

- any PNG pixel that is not strictly on or off (no antialiasing threshold);
- any SVG icon whose path is not an axis-aligned integer rectangle grid -
  a curve, an arc or a non-integer coordinate is a rejection, not an
  approximation;
- a named cell that is empty, or a non-empty cell with no name;
- duplicate names, or names that are not valid C identifiers;
- an SVG entry missing its `upstream`/`commit` provenance - **provenance is
  a first-class requirement**, not paperwork, so a re-bake years from now
  stays reproducible and the licence stays checkable;
- a pack/unpack round trip that does not reproduce the source pixels
  exactly.

## Testing the artifact, not the generator

The shipped header is tested on its own terms: every icon non-empty, its
content bounding box inside its declared `w x h`, its baked `blocks` count
matching what `icon_walk_blocks()` actually produces, and declared
symmetries holding (`suite_sand_icons.c`, `suite_icons_system.c`). **Do not
assert the baked bytes against a Python re-implementation of the packer** -
that tests the generator twice and the artifact never.
