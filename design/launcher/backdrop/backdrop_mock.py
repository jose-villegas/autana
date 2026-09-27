"""Look mock of the launcher backdrop in landscape: a flat background, two
back ridge layers and the front fill carrying the sky gradient, each drawn
with screen-fixed scanline dither, and a tap and a strum on the spring line.
The figures are the firmware defaults (launcher/main/ui/ui_ridge.c and
ridge_theme.h), without the breath; it renders pluck.gif to judge the look
without a board.

    python design/launcher/backdrop/backdrop_mock.py <out-dir>

Needs numpy and Pillow.
"""
import math
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
W, H = 448, 368
FPS, SECONDS = 25, 6.0
SEED = 0x1199C8
LIP_PX = 12
CUTOFF = np.array([64, 192, 128, 255])
LAYERS = [  # offset, amplitude, wavelength, period ms, lip alpha, body alpha, echo
    (-70, 5, 260, 6000, 71, 204, 0.25),
    (-30, 8, 200, 4300, 71, 204, 0.5),
    (0, 6, 164, 2600, 76, 217, 1.0),
]
TENSION, STIFFNESS, DAMPING, TICK_MS = 40, 4, 8, 4
PLUCK_TAP, PLUCK_STRUM, PLUCK_WIDTH, STRUM_STEP_PX = 4.5, 3.0, 40, 12


def linear(v):
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def srgb(v):
    return v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055


def to_oklch(rgb):
    r, g, b = (linear(((rgb >> s) & 0xFF) / 255) for s in (16, 8, 0))
    l = (0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b) ** (1 / 3)
    m = (0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b) ** (1 / 3)
    s = (0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b) ** (1 / 3)
    a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s
    bb = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s
    return 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, math.hypot(a, bb), math.atan2(bb, a)


def linear_rgb(l, c, h):
    a, b = c * math.cos(h), c * math.sin(h)
    ll = (l + 0.3963377774 * a + 0.2158037573 * b) ** 3
    mm = (l - 0.1055613458 * a - 0.0638541728 * b) ** 3
    ss = (l - 0.0894841775 * a - 1.2914855480 * b) ** 3
    return (4.0767416621 * ll - 3.3077115913 * mm + 0.2309699292 * ss,
            -1.2684380046 * ll + 2.6097574011 * mm - 0.3413193965 * ss,
            -0.0041960863 * ll - 0.7034186147 * mm + 1.7076147010 * ss)


def role(seed, l, chroma_scale):
    c = seed[1] * chroma_scale
    for _ in range(12):
        if all(0 <= v <= 1 for v in linear_rgb(l, c, seed[2])):
            break
        c *= 0.8
    else:
        c = 0.0
    return tuple(min(255, max(0, int(srgb(v) * 255 + 0.5))) for v in linear_rgb(l, c, seed[2]))


def to565(rgb):
    r, g, b = (np.asarray(rgb)[..., i] for i in range(3))
    return np.stack([(r >> 3) * 255 // 31, (g >> 2) * 255 // 63, (b >> 3) * 255 // 31], -1).astype(np.uint8)


seed = to_oklch(SEED)
BACKGROUND, SKY_TOP, SKY_BOTTOM = role(seed, 0.55, 0.70), role(seed, 0.55, 1.0), role(seed, 0.64, 0.55)
BACKS = [role(seed, 0.49, 0.95), role(seed, 0.42, 0.85)]

half = (W + H) // 4
along = np.clip(half + np.arange(H) - (H - 1) // 2, 0, 2 * half)
amount = along * 100 // (2 * half)
SKY = np.stack([SKY_TOP[i] + (SKY_BOTTOM[i] - SKY_TOP[i]) * amount // 100 for i in range(3)], -1)
SKY = np.broadcast_to(SKY[:, None, :], (H, W, 3))

src = (ROOT / "launcher/main/ui/ridge_curve_generated.h").read_text()
ridge = np.array([int(v) for v in re.findall(r"-?\d+", src.split("{", 1)[1].split("}")[0])]) / 16.0
X = np.arange(W)
YY = np.arange(H)[:, None]
PHASE = CUTOFF[X % 4][None, :]
spring, velocity = np.zeros(W), np.zeros(W)


def poke(x, px_per_tick):
    k = np.arange(W) - x
    window = np.clip(1 - (k / PLUCK_WIDTH) ** 2, 0, 1) ** 2
    velocity[:] -= px_per_tick * window


def advance(ms):
    for _ in range(int(ms // TICK_MS)):
        lap = np.roll(spring, 1) + np.roll(spring, -1) - 2 * spring
        lap[0] = lap[-1] = 0
        velocity[:] += (TENSION * lap - STIFFNESS * spring) / 256
        velocity[:] -= DAMPING / 256 * velocity
        spring[:] += velocity


def frame(t_ms):
    img = np.broadcast_to(np.array(BACKGROUND), (H, W, 3)).copy()
    for index, (offset, amp, wavelength, period, lip, body, echo) in enumerate(LAYERS):
        curve = ridge + offset + amp * np.sin(2 * np.pi * (X / wavelength - t_ms / period)) + spring * echo
        distance = YY - np.round(curve)[None, :]
        alpha = np.where(distance >= LIP_PX, body, lip + (body - lip) * np.clip(distance, 0, LIP_PX) // LIP_PX)
        take = (distance >= 0) & ((alpha == 255) | (alpha >= PHASE))
        colour = SKY if index == 2 else np.array(BACKS[index])
        img = np.where(take[..., None], colour, img)
    return Image.fromarray(to565(img.astype(np.int32)))


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    frames, last_strum = [], None
    for f in range(int(FPS * SECONDS)):
        if f == 12:
            poke(150, PLUCK_TAP)
        if 62 <= f < 72:
            x = 250 + (f - 62) * 15
            if last_strum is None or x - last_strum >= STRUM_STEP_PX:
                poke(x, PLUCK_STRUM)
                last_strum = x
        frames.append(frame(f * 1000 / FPS))
        advance(1000 / FPS)
    frames[0].save(out / "pluck.gif", save_all=True, append_images=frames[1:], duration=1000 // FPS, loop=0)


if __name__ == "__main__":
    main()
