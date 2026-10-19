# Tile

`bool ui_tile(recti_t r, const char* label, bool selected)` draws one choice
of a row. It returns true on the frame a tap is released on it. `selected`
is the caller's, drawn on the theme's `accent_face`.

## Example

<!-- generated: ui-tile-example check: python launcher/tools/render/ui_examples_doc.py --check -->
```c
/* Two tiles side by side, one choice of two: the caller says which is selected. */
#include "ui/ui.h"

typedef struct {
    bool hard;
} tile_example_t;

bool
tile_example(tile_example_t* s) {
    ui_row(UI_ROW_HEIGHT);
    const recti_t row = ui_next();
    const int w = (row.w - UI_ROW_GAP) / 2;
    if (ui_tile((recti_t){row.x, row.y, w, row.h}, "EASY", !s->hard)) {
        s->hard = false;
        return true;
    }
    if (ui_tile((recti_t){row.x + w + UI_ROW_GAP, row.y, w, row.h}, "HARD", s->hard)) {
        s->hard = true;
        return true;
    }
    return false;
}
```
<!-- /generated: ui-tile-example -->

## Renders

![Two tiles, EASY selected, portrait](../images/ui/tile.png) ![The same, landscape](../images/ui/tile-landscape.png)

## States

Each row is the example above, unchanged, in one of the
[states](../UI-Toolkit.md#states). Fires says when a tap there made it
return true.

<!-- generated: ui-tile-states sha256=38b4cd97c5053c25405387107c26ff4504dc4e7df632c9edb04ce9dcb94f6c89 -->
| State | Render | Fires |
|---|---|---|
| rest | ![tile, rest](../images/ui/tile.png) | no |
| pressed | ![tile, pressed](../images/ui/tile-pressed.png) | no |
| tapped | looks like rest | no |
| disabled | ![tile, disabled](../images/ui/tile-disabled.png) | no |
| no-pack | looks like rest | no |
<!-- /generated: ui-tile-states -->

## Interactions

- A tile never selects itself: the caller stores the pick and passes it back
  next frame.
- Fires when the finger lifts over it; sliding off first cancels
  ([input](input.md#a-tap)).
- A tap just outside lands on the nearest control within reach
  ([input](input.md#a-tap-that-misses)).
- In a `ui_scroll_window()` a vertical drag scrolls the rows and presses
  nothing ([scrolling](scrolling.md)).
- Inside `ui_disabled_begin(true)` it draws in the theme's `muted` ink and
  never fires ([scopes](scopes.md)).
- Its label is its identity: two with one label in a window are told apart
  by call order ([input](input.md#identity)).

## See also

- [Tile with an icon](tile_icon.md)
- [Toggle](toggle.md): on or off without a row of choices
- [UI Toolkit](../UI-Toolkit.md): every control, the theme, the frame
