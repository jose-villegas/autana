# Launcher backdrop reference

The launcher backdrop's reference look: a PSP-XMB-style field of ridge
layers, the front fill's edge being the ridge itself. `pluck.gif` is a host
mock, not a firmware capture, of the look moving: a tap at 0.5 s and a
strum at 2.5 s. `backdrop_mock.py` renders it from the firmware defaults.

| Part | Value |
|---|---|
| Colours | one seed, `#1199C8`; background, sky top and bottom and both back layers are fixed OKLCH lightness roles of it (`ridge_theme_from_rgb()` in `launcher/main/ui/ridge_theme.h`) |
| Background | flat, above every layer |
| Fill below the front ridge | the sky gradient, along gravity |
| Layer shape | Cerro Autana's ridge, offset -70 / -30 / 0 px, each with its own wave: 5 / 8 / 6 px high, 260 / 200 / 164 px long, 6.0 / 4.3 / 2.6 s |
| Layer edges | dithered alpha, never a mix: a 12 px lip rising to a mostly solid body (0.8, 0.8, 0.85) |
| Line | none |
| Dither | scanlines, fixed to the panel's rows rather than to the curve, so in landscape they run across the view |
| Pluck | the launcher's spring line; the far back layer echoes it at 25%, the near one at 50% |

Each value is a live tunable (the `TUNE(ridge, …)` block in
`launcher/main/ui/ui_ridge.c`, which owns the current figures), and the fill
pattern is selectable (`fill_pattern`). Because the dither is fixed to the
screen, pixels a curve did not cross stay identical between frames, so only
the strips the curves cross are sent.
