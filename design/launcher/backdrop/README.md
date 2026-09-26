# Launcher backdrop reference

The look the launcher's backdrop is being built toward: a PSP-XMB-style
field of ridge layers behind the glowing ridge. These are host mocks, not
firmware captures.

| File | Shows |
|---|---|
| `pluck.gif` | The chosen look moving: a tap at 0.5 s, a strum at 2.5 s |
| `idle-waves.gif` | The same with no touch: three layers waving at their own speeds |
| `colour-balance.png` | Background and fill pushed together, 45% (chosen) and 60% |
| `dither-patterns.png` | The dither patterns compared on the same scene |
| `backdrop_mock.py` | The script that renders `pluck.gif` |

## The chosen look

| Part | Value |
|---|---|
| Background above the ridge | flat, `#0B6382` moved 45% toward the fill's mid colour |
| Fill below the front ridge | the sky gradient `#1199C8` to `#91C6D1`, moved 45% toward `#0B6382` |
| Back layers | two, from the sky's hue: HSV (h, 0.95, 0.55) and (h, 0.85, 0.42), where h is the hue of `#1199C8` |
| Layer shape | Cerro Autana's ridge, offset -70 / -30 / 0 px, each with its own wave: 5 / 8 / 6 px high, 260 / 200 / 164 px long, 6.0 / 4.3 / 2.6 s |
| Layer edges | dithered alpha, never a mix: a 12 px lip rising to a mostly solid body (0.8, 0.8, 0.85) |
| Glow | white core, halo `#CFF4F8`, radius 13, core 3, trail 226/256 |
| Dither | fixed to the screen, not the curve; scanlines (4 rows) for the fills |
| Pluck | the spring line as today (tension 64, stiffness 4, damping 2); back layers echo it at 50% and 25% |

Every value above is meant to be a live tunable, and the pattern a
selectable table. Because the dither is fixed to the screen, pixels a
curve did not cross stay identical between frames, so only the strips
the curves cross are sent: in the mock about 60 KiB and 1.5 ms of bus a
frame against 322 KiB and about 10 ms for a full send.
