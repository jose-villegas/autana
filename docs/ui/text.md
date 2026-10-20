# Text

`ui_text(r, s)`, `ui_text_right(r, s)` and `ui_text_centre(r, s)` draw one
line clipped to `r`, centred vertically, in the theme's `text` ink.
`ui_caption(r, s)` and `ui_caption_centre(r, s)` use its `caption` ink.
`ui_text_height()` is one line at the current scale. None takes a tap.

## Example

<!-- generated: ui-text-example check: python launcher/tools/render/ui_examples_doc.py --check -->
```c
/* Text in a row: a caption at the left, a value at the right, rebuilt each frame. */
#include <stdio.h>

#include "ui/ui.h"

typedef struct {
    int unread;
} text_example_t;

bool
text_example(text_example_t* s) {
    char count[12];
    snprintf(count, sizeof count, "%d", s->unread);
    ui_row(ui_text_height() + UI_TEXT_PAD);
    const recti_t r = ui_next();
    ui_caption(r, "UNREAD");
    ui_text_right(r, count);
    return false;
}
```
<!-- /generated: ui-text-example -->

## Renders

<!-- generated: ui-text-renders sha256=5a69689bccaff49c33b48d905740fbd4fda9696f927ca70c37bdf1a6bffda1ed -->
| Portrait | Landscape |
|---|---|
| ![text, portrait](../images/ui/text.png) | ![text, landscape](../images/ui/text-landscape.png) |
<!-- /generated: ui-text-renders -->

## States

Each row is the example above, unchanged, in one of the
[states](../UI-Toolkit.md#states). Fires says when a tap there made it
return true.

<!-- generated: ui-text-states sha256=278801ef384716567db81384875b98980f0813392e42318471a319b1224ef2ea -->
| State | Render | Fires |
|---|---|---|
| rest | ![text, rest](../images/ui/text.png) | no |
| pressed | looks like rest | no |
| tapped | looks like rest | no |
| disabled | looks like rest | no |
| no-pack | looks like rest | no |
<!-- /generated: ui-text-states -->

## Interactions

- Text is built from the caller's values each frame: a changed string is a
  changed frame, and an unchanged one costs nothing.
- `ui_scale_begin()` and `ui_alpha_begin()` change the size and ink coverage
  of the text inside ([scopes](scopes.md)).
- Measure before relying on a fit: `ui_measure_text()` at the current font
  and scale.

## See also

- [Text and Fonts](../Text-and-Fonts.md): fonts, scales, text styles
- [Header](header.md)
- [UI Toolkit](../UI-Toolkit.md): every control, the theme, the frame
