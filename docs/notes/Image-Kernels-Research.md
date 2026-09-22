# Image kernels on the ESP32-S3: research

**Status**: literature and precedent for real-time blur and edge detection
over the framebuffer.
Nothing here has run on the board. Every per-pixel cost is an **estimate**
at roughly 1–1.5 cycles per simple operation at `-O2`, not a measurement.
Sources are marked *(checked)* when they were re-read for this note, and
*(from memory)* when they are standard references cited without re-reading.
A claim with no source says so.

The budget these estimates are held against, from the rendering roadmap: a
368×448 pass is 164,864 pixels, and one core at 240 MHz and 60 fps has about
**24 cycles per pixel** for it — about 48 at 30 fps, and about 97 at half
resolution (184×224) and 60 fps.

---

## 1. Working directly in packed RGB565

**Two-pixel average without unpacking.** `(a & b) + (((a ^ b) & 0xF7DE) >> 1)`.
The mask clears the low bit of each field so a halved difference cannot
borrow into the field below it.

**Many-pixel sums in one word.** Spread a pixel into 32 bits with spare
guard bits above each field (a layout like `0x07E0F81F`, green moved to the
upper half), so red, green and blue can be added or shifted together in one
register without carrying into each other, then pack back.

| Operation | Cost, *estimate* |
|---|---|
| 2-tap average | 6–10 cycles |
| [1 2 1]/4, as `avg(avg(a, c), b)` | 2 averages per pass |
| Separable 3×3 (two passes of the above) | 24–40 cycles |
| Spread to 32 bits, then pack back | ~6 ops |
| One running-sum add and subtract, all three channels | ~2 ops |

**Accuracy.**

- The floor average loses 0.25 LSB per operation on average. On 5-bit
  fields, repeated passes visibly darken the image. Alternating it with the
  rounding-up form `(a | b) - (((a ^ b) & m) >> 1)` on alternate passes or
  rows cancels the bias.
- Averaging in RGB565 is not gamma-correct, so the average of very
  different colours comes out too dark (Riemersma).

**What it cannot do without unpacking.**

- No abs, compare, min/max, clamp, or per-channel division by anything but
  a power of two. That rules out Sobel magnitude, thresholds, and a sharpen
  that clamps.
- Signed differences need a bias offset.

**Sources.**

- Riemersma, "Quick colour averaging", CompuPhase, 2002 —
  https://www.compuphase.com/graphic/scale3.htm *(checked)*
- LVGL issue #5015, 32-bit guard-bit blending —
  https://github.com/lvgl/lvgl/issues/5015 *(checked)*
- Warren, *Hacker's Delight*, §2-5, floor and ceiling average identities
  *(from memory)*

## 2. Separable and recursive blurs

**Running-sum box blur.** Add the pixel entering the window and subtract
the one leaving it. Horizontally that is per row; vertically, one
accumulator per column. The cost is O(1) per pixel for any radius.
*Estimate*: 8–12 cycles per pixel in the spread 32-bit form with a
power-of-two window (division becomes a shift). Any other window needs a
multiply per field.

**Repeated box as a Gaussian.** Three box passes are visually Gaussian
(Wells 1986); Kovesi 2010 gives the box widths for a target sigma.
*Estimate*: three times the box cost, roughly 30–40 cycles per pixel for
full-screen packed RGB565 — a 30 fps candidate, or 60 fps on luminance
only.

**Stack blur.** A triangular kernel built from running sums (Klingemann
2004), reported by its author as about 7× faster than his own Gaussian.
*Estimate*: about twice a box pass.

**Recursive (IIR) Gaussian.** Deriche 1993; Young & van Vliet 1995. Roughly
3–4 multiplies forward and 3–4 backward per pass per channel. *Estimate*:
30–60 cycles per channel, so it fits only half-resolution luminance.

**One-pole smoothing.** `y += (x - y) >> k`, run left→right, right→left,
top→bottom and bottom→top. *Estimate*: about 3 ops per pass. No specific
source was found for using it as an image blur.

**Sources.**

- Kovesi, "Fast Almost-Gaussian Filtering", DICTA 2010,
  doi:10.1109/DICTA.2010.30 *(checked)*
- Getreuer, "A Survey of Gaussian Convolution Algorithms", IPOL 2013 —
  https://www.ipol.im/pub/art/2013/87/ — covers Wells, Deriche and
  Young–van Vliet, boundary handling included *(checked)*
- Kutskir, "Fastest Gaussian Blur" —
  https://blog.ivank.net/fastest-gaussian-blur.html *(checked)*

## 3. Reduced pixel representations

**Cheap luminance.** `Y = 2·r5 + 2·g6 + b5`, range 0–219. Normalised
weights 0.28 / 0.58 / 0.14 against BT.601's 0.299 / 0.587 / 0.114. About
6 ops. Green alone (`g6`, weight 0.587) is cheaper still. Both are derived
for this note; no source was found.

**YCoCg-R.** An exactly reversible transform using only adds and shifts. Y
keeps its bit depth; Co and Cg gain one bit. *Estimate*: about 8 ops
forward and 8 back, so it pays only if the frame stays in YCoCg rather than
converting per effect.

- Malvar & Sullivan, JVT-I014r3, 2003 —
  https://www.microsoft.com/en-us/research/wp-content/uploads/2016/06/Malvar_Sullivan_YCoCg-R_JVT-I014r3-2.pdf
  *(checked)*

**Indexed (palette) 8-bit.** The kernel runs on 8-bit indices or heat
values, and one lookup maps the result to RGB565 — the classic fire effect
(section 7).

**What each effect needs.** No source; this is standard practice.

- **Luminance only is enough** for edge detection, difference of Gaussians,
  sharpening detail and bloom masks.
- **Blur needs colour too.** Blurring only luminance leaves colour edges
  sharp and reads as fringing rather than blur, so a colour blur needs all
  channels, or chroma at 2× subsampling.

**Tone mapping.** A full 65,536-entry RGB565 → RGB565 table is 131 KB, far
larger than the 32 KB data cache. Three per-field tables (32, 64 and 32
entries) cost about 8 ops.

## 4. Edge detection

**Sobel, made cheap.**

- Use the separable form: `(a + c) + (b << 1)`, then a difference.
- Take `|gx| + |gy|` instead of a square root.
- Run on 8-bit luminance, and threshold to a 1-bit output.

*Estimate*: 15–20 cycles per pixel in scalar C. Keeping the column sums from
the previous pixel saves about a third.

**Laplacian.** `4c − n − s − e − w`, about 8 ops *(estimate)*.

**Difference of Gaussians.** Two running-box blurs on luminance,
subtracted; about 20 cycles *(estimate)*. Kovesi 2010 covers it.

- Simd library, `SobelDx` / `GaussianBlur3x3`, SSE and NEON implementations
  on u8 input with s16 output — https://github.com/ermig1979/Simd *(checked)*

## 5. SIMD on the S3

**What ESP-DSP provides.**

- `dspi_dotprod_{u8,s8,u16,s16}` and their `_off_` variants, with S3
  assembly versions (`_aes3`).
- `dspi_conv_f32` exists only as plain C.

**Reported speed** (ESP-DSP benchmarks):

| Dot product | S3 assembly | Plain C |
|---|---|---|
| 64×64 u8 | 689 cycles | 37,962 cycles |
| 32×32 s16 | 409 cycles | 15,181 cycles |

The u8 case is about 55× faster, or ~0.17 cycles per tap.

- https://docs.espressif.com/projects/esp-dsp/en/latest/esp32/esp-dsp-benchmarks.html
  *(checked)*

**Why that does not transfer to per-pixel kernels.** A library call per
pixel costs more than the kernel itself. The shape that fits is
hand-written SIMD over whole rows — 16 u8 or 8 s16 values per instruction —
the same shape as Simd's NEON code. *Estimate*: 2–5 cycles per pixel for a
3×3 Sobel or blur on luminance.

**ESP-DL** image tools are resize and colour conversion only; no 2D
convolution for display images was found.

## 6. Memory access

- **A full frame is too big to cache.** One frame is 330 KB, about ten
  times the 32 KB data cache, and larger than most of internal SRAM.
- **Row windows fit the cache.** A 3-row luminance window is 1.1 KB and
  the per-column accumulators are 1.5 KB. Scan sequentially and write the
  output in bands.
- **Half resolution fits internal RAM.** A 184×224 RGB565 buffer is 82 KB.
- Line-buffer scheduling is the core of Halide (Ragan-Kelley et al., PLDI
  2013) *(from memory)*.

## 7. Precedent

- **Jare's fire (1993).** 8-neighbour sum, `>> 3`, a 256-colour palette,
  run at 80×50 for speed — https://www.hanshq.net/fire.html *(checked)*
- **Lode's fire.** Averages only the rows below the current one, so it
  needs a single buffer — https://lodev.org/cgtutor/fire.html *(checked)*
- **No ESP32 or GBA precedent found.** No documented blur or edge trick
  turned up for either. An ESP32 demoscene write-up
  (https://theor.xyz/esp32-love-notes-demoscene/) covers palette cycling
  only.

## First experiments, ranked

All estimates. Measure each on the device with the perfmon method before
building on it.

| # | Experiment | Pixel representation | Cost, *estimate* |
|---|---|---|---|
| 1 | Half-res Sobel, row-wise SIMD, 1-bit threshold | 8-bit luminance | under 10 cycles/pixel |
| 2 | Full-res running-box blur, power-of-two window, one horizontal and one vertical pass | 32-bit spread RGB565 | ~20 cycles/pixel |
| 3 | Scalar separable Sobel with column-sum reuse | full-res 8-bit luminance | 15–20 cycles/pixel |
| 4 | Kovesi 3-pass box blur at half resolution, upscaled with the 2-tap average; also the base for bloom | half-res RGB565 in internal SRAM | within the ~97-cycle half-res budget |
| 5 | [1 2 1]² 3×3 blur with alternating floor/ceil averages | packed 16-bit RGB565 | 24–40 cycles/pixel; cheapest to write |

## Related

- [`../Autana-Rendering-Roadmap.md`](../Autana-Rendering-Roadmap.md) — the
  cycle budget and the frame architecture over the one framebuffer.
