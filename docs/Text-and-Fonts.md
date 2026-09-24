# Text and Fonts

The firmware ships `gfx_font_8x8`, an 8 x 8 bitmap covering U+0000 through U+007F. Its glyphs live in [`font8x8_basic.h`](../launcher/main/gfx/font8x8_basic.h), with one byte per row and the leftmost pixel in bit 0. There is no font rasterizer on the device.

[`gfx_font_t`](../launcher/main/gfx/gfx_font.h) describes the bitmap, cell size, codepoint range, and optional advance table. The shipped font is monospace, so its `advance` pointer is NULL. Metrics are pure functions in the header and can be tested on a host without a framebuffer.

## Drawing

Text calls reach `gfx_text_font()` in [`gfx.c`](../launcher/main/gfx/gfx.c). Each call takes a pixel scale and a number of quarter turns. Integer scales keep the bitmap crisp. `gfx_text_font_dither()` draws translucent text, including the boot title's shadow and fade. `gfx_text_font_halo()` draws outlined text.

| Call | Font | Scale |
|---|---|---|
| `gfx_text()` | `gfx_font_ui()` | `GFX_GLYPH_SCALE` |
| `gfx_text_scaled()` | `gfx_font_ui()` | given |
| `gfx_text_turned()` | `gfx_font_ui()` | given |
| `gfx_text_font()` | given | given |

Measure with `gfx_text_width()` and `gfx_text_height()` for the UI font, or `gfx_font_width()` for a font and scale. The boot title uses `gfx_font_ui()` at `title_scale` from the timeline.

## Roles

[`gfx_font_roles.h`](../launcher/main/gfx/gfx_font_roles.h) binds `gfx_font_ui()` to the shipped bitmap. UI text and the boot title use that role. Call sites choose scale; labels do not need a separate role.

## Text in a microui screen

`ui_set_font_scaled()` puts the font and scale in microui's command list, so a change causes a repaint. `ui_set_text_style()` affects rendering outside that list and needs `ui_invalidate()` when changed. See [`Building-a-Screen.md`](Building-a-Screen.md) for layout and measurement.
