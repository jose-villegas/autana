"""Look mock of the launcher backdrop: sky, three ridge layers with
screen-fixed dithered fills, the glowing front ridge with its trail, and a
tap and a strum on the spring line. Python, not the firmware - it renders
design/launcher/backdrop/pluck.gif for judging the look.

    python design/launcher/backdrop/backdrop_mock.py <out-dir>

Run from the repository root; needs numpy and Pillow.
"""
import re, sys, numpy as np
from PIL import Image, ImageDraw
OUT = sys.argv[1]
src = open(r'launcher/main/ui/ridge_curve_generated.h').read()
ys = np.array([int(v) for v in re.findall(r'-?\d+', src.split('{',1)[1].split('}')[0])]) / 16.0
W, H = 448, 368
TW, TH = 16, 23
YY, XX = np.mgrid[0:H, 0:W]
X = np.arange(W)
R, CORE, TRAIL = 13.0, 3.0, 226/256

def bayer(n):
    m = np.array([[0]])
    while m.shape[0] < n:
        m = np.block([[4*m, 4*m+2], [4*m+3, 4*m+1]])
    return m / m.size
def rank(m):
    order = np.argsort(m.ravel(), kind="stable")
    r = np.empty(m.size); r[order] = np.arange(m.size)
    return (r / m.size).reshape(m.shape)

def blue_noise(n=32, seed=3):
    rng = np.random.default_rng(seed)
    def blur(a):
        f = np.fft.fft2(a); k = np.fft.fftfreq(n)
        g = np.exp(-(k[:, None] ** 2 + k[None, :] ** 2) * (2 * np.pi * 1.5) ** 2 / 2)
        return np.real(np.fft.ifft2(f * g))
    ranks = np.zeros((n, n)); pts = np.zeros((n, n), bool)
    for i in range(n * n):
        e = blur(pts.astype(float)) + rng.random((n, n)) * 1e-6
        e[pts] = np.inf
        y, x = np.unravel_index(np.argmin(e), e.shape)
        pts[y, x] = True; ranks[y, x] = i
    return ranks / (n * n)

def clustered(n):
    c = (n - 1) / 2
    yy, xx = np.mgrid[0:n, 0:n]
    return rank(-(np.cos(np.pi * (xx - c) / (n / 2)) + np.cos(np.pi * (yy - c) / (n / 2))) + 1e-3 * (yy * n + xx))

BN = blue_noise()
DITHERS = {
    "Bayer 2x2": bayer(2)[YY % 2, XX % 2],
    "Bayer 4x4": bayer(4)[YY % 4, XX % 4],
    "Bayer 8x8": bayer(8)[YY % 8, XX % 8],
    "Bayer 4x4 at 2x2 px": bayer(4)[(YY // 2) % 4, (XX // 2) % 4],
    "halftone 4x4": clustered(4)[YY % 4, XX % 4],
    "halftone 8x8": clustered(8)[YY % 8, XX % 8],
    "blue noise 32x32": BN[YY % 32, XX % 32],
    "scanlines 4": rank(np.arange(4)[:, None] + 0 * np.arange(1)[None, :] + 1e-3 * np.array([[0], [2], [1], [3]]))[YY % 4, 0],
    "diagonal hatch 4": ((XX + YY) % 4) / 4 + ((XX - YY) % 2) / 8,
}
SKY_TOP, SKY_BOT = np.array((0x11,0x99,0xC8),float), np.array((0x91,0xC6,0xD1),float)
LAYERS = [
    dict(off=-70, amp=5, wl=260, per=6.0, rgb=(0x04,0x63,0x79), alpha=0.8),
    dict(off=-30, amp=8, wl=200, per=4.3, rgb=(0x99,0x92,0x94), alpha=0.8),
    dict(off=0,   amp=6, wl=164, per=2.6, rgb=(0x2D,0x57,0x3B), alpha=0.85),
]
FADE = 12.0  # px below a curve over which its fill comes in

SPRING = np.zeros(W); SPRING_V = np.zeros(W)
ECHO = [0.25, 0.5, 1.0]
def spring_poke(x, width, px_per_tick):
    xs = np.arange(W); w = np.clip(1 - np.abs(xs - x) / (width / 2), 0, 1)
    SPRING_V[:] += -px_per_tick * (0.5 - 0.5 * np.cos(np.pi * w))
def spring_step(ms):
    for _ in range(int(ms // 4)):
        lap = np.roll(SPRING, 1) + np.roll(SPRING, -1) - 2 * SPRING; lap[0] = lap[-1] = 0
        SPRING_V[:] += 64 / 256 * lap - 4 / 256 * SPRING
        SPRING_V[:] -= 2 / 256 * SPRING_V
        SPRING[:] += SPRING_V
def curve(L, t):
    base = ys + L['off'] + L['amp'] * np.sin(2 * np.pi * (X / L['wl'] - t / L['per']))
    return base + SPRING * ECHO[LAYERS.index(L)]

def quant(c, th, levels=6):
    step = 255.0 / (levels * 4)
    return np.floor(c / step + th[..., None]) * step

def frame(t, glow_fb, th):
    ty = (YY / (H - 1))[..., None]
    k = SWAP[0]
    bg = np.array((0x0B, 0x63, 0x82), float)
    top, bot = SKY0[0], SKY0[1]
    fill_mid = (top + bot) / 2
    bg_b = bg + (fill_mid - bg) * k
    sky = (top + (bg - top) * k) * (1 - ty) + (bot + (bg - bot) * k) * ty
    img = np.broadcast_to(bg_b, sky.shape).copy()
    for L in LAYERS:
        below = np.clip((YY - curve(L, t)[None, :]) / FADE, 0, 1)[..., None]
        a = L['alpha'] * np.minimum(1, 0.35 + below)   # a soft lip, then the body
        a = np.where((YY >= curve(L, t)[None, :])[..., None], a, 0)
        take = a[..., 0] > th                       # dithered alpha: pick, never mix
        colour = sky if L.get('sky') else np.array(L['rgb'], float)
        img = np.where(take[..., None], colour, img)
    img = quant(img, th)
    cy = curve(LAYERS[-1], t)
    d = np.abs(YY - cy[None, :]); tt = np.clip((d / R) ** 2, 0, 1)
    light = np.floor((1 - tt) ** 2 * 6 + th) / 6
    halo = np.clip(np.sqrt(tt) / (CORE / R), 0, 1)[..., None]
    g = (np.array([255, 255, 255.]) * (1 - halo) + HALO * halo) * light[..., None]
    glow_fb = np.maximum(glow_fb * TRAIL, g)
    return np.maximum(img, glow_fb), glow_fb

def to565(rgb):
    return np.floor(np.clip(rgb, 0, 255) / 255 * [31, 63, 31] + 0.5).astype(np.int32)



import colorsys
HALO = np.array([0xCF, 0xF4, 0xF8], float)
def hsv(h, s_, v): return tuple(255 * c for c in colorsys.hsv_to_rgb(h, s_, v))
sky_h = colorsys.rgb_to_hsv(0x11/255, 0x99/255, 0xC8/255)[0]
VARIANTS = [("balanced %d%%" % int(k * 100), k, [hsv(sky_h, 0.95, 0.55), hsv(sky_h, 0.85, 0.42), None]) for k in (0.45, 0.60)]

SKY0 = (SKY_TOP.copy(), SKY_BOT.copy())
SWAP = [0.45]
for L, c in zip(LAYERS, [hsv(sky_h, 0.95, 0.55), hsv(sky_h, 0.85, 0.42), None]): L['rgb'] = c
LAYERS[-1]['alpha'] = 0.85; LAYERS[-1]['sky'] = True; LAYERS[-1]['rgb'] = (0, 0, 0)
HALO[:] = (0xCF, 0xF4, 0xF8)
g = np.zeros((H, W, 3)); frames = []
LAST = [-1000]
fps = 25
for f in range(int(fps * 6.0)):
    t = f / fps
    if f == 12: spring_poke(150, 32, 8.0)
    if 62 <= f < 72:
        x = 250 + (f - 62) * 15
        if x - LAST[0] >= 20: spring_poke(x, 32, 8.0); LAST[0] = x
    img, g = frame(t, g, DITHERS["scanlines 4"])
    q = to565(img); frames.append(Image.fromarray((q / [31, 63, 31] * 255).astype(np.uint8)))
    spring_step(1000 / fps)
frames[0].save(f'{OUT}/pluck.gif', save_all=True, append_images=frames[1:], duration=int(1000 / fps), loop=0)
