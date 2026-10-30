# Scalar

Per-type arithmetic, fixed point, random draws and units. [Math index](README.md).

<!-- generated: math-fast-links-scalar check: python launcher/tools/perf/fast_paths.py --check -->
Fast-path evidence: [mathf_recip](Fast-Paths.md#mathf-recip), [mathf_rsqrt](Fast-Paths.md#mathf-rsqrt), [mathf_rsqrt_approx](Fast-Paths.md#mathf-rsqrt-approx), [mathx_rsqrt_q30](Fast-Paths.md#mathx-rsqrt-q30), [mathf_repeat](Fast-Paths.md#mathf-repeat), [trig_sin](Fast-Paths.md#trig-sin), [mathf_recip_approx](Fast-Paths.md#mathf-recip-approx).
<!-- /generated: math-fast-links-scalar -->

`math/scalar/` holds per-type arithmetic, fixed point, random draws, trigonometry
and time units. The tables list the range, lerp and rounding helpers;
the headers under `math/scalar/` declare the rest.

## mathi.h

These take and return plain integers, not structs.

| Function | Purpose | Formula |
|---|---|---|
| `mathi_repeat(v, len)` | floored modulo into `[0, len)`; `len > 0`, any `int` value | $`v - \lfloor v/len\rfloor\,len`$ |
| `mathi_wrap(v, lo, hi)` | into `[lo, hi)`; `lo < hi`, any `int` value | $`lo + \mathrm{repeat}(v-lo,hi-lo)`$ |
| `mathi_repeat_u32(v, len)`, `mathi_repeat_u64(v, len)` | into `[0, len)`; `len > 0`, full unsigned range | $`v \bmod len`$ |
| `mathi_clamp_u32(v, lo, hi)`, `mathi_clamp_u64(v, lo, hi)` | inclusive bounds, full unsigned range | $`\min(\max(v,lo),hi)`$ |
| `mathi_min(a, b)`, `mathi_max(a, b)` | smaller or larger value by comparison | $`\min(a,b)`$, $`\max(a,b)`$ |
| `mathi_clamp(v, lo, hi)` | inclusive bounds | $`\min(\max(v,lo),hi)`$ |
| `mathi_lerp(a, b, t, n)` | `t` of `n` steps from `a` to `b`, the step truncated toward `a`; `n > 0`, `0 <= t <= n` | $`a + \mathrm{trunc}((b-a)t/n)`$ |
| `mathi_lerp_u8(a, b, w)` | `mathi_lerp` with `n = UINT8_MAX` | $`a + \mathrm{trunc}((b-a)w/255)`$ |
| `mathi_isqrt64(v)` | floor square root over the full uint64 range | $`\lfloor\sqrt v\rfloor`$ |
| `mathi_half_chord(r, d)` | floor half chord; zero when outside; `r`, `d` fit int32 | $`\lfloor\sqrt{r^2-d^2}\rfloor`$ for $`\lvert d\rvert<r`$ |
| `mathi_half_chord_round(r, d)` | nearest half chord; same range and outside rule | $`h=r^2-d^2,\ f=\lfloor\sqrt h\rfloor`$<br>$`f+1`$ when $`h-f^2>f`$, else $`f`$ |
| `mathi_in_disc(dx, dy, r)` | closed disc predicate; squared sum must fit int64 | $`dx^2+dy^2\le r^2`$ |
| `mathi_zero()`, `mathi_one()` | integer constants | $`0, 1`$ |
| `mathi_add(a, b)`, `mathi_sub(a, b)`, `mathi_neg(a)`, `mathi_mul(a, b)` | int32 arithmetic that wraps | $`a+b`$, $`a-b`$, $`-a`$, $`ab`$ |
| `mathi_wadd(a, b)`, `mathi_wmul(a, b)` | widened int64 sum and product | $`a+b`$, $`ab`$ |
| `mathi_abs(v)`, `mathi_sign(v)` | magnitude and sign | $`\lvert v\rvert`$, $`-1, 0, 1`$ |
| `mathi_len(x, y)` | length without a square root, within about 4% | $`\max+\lfloor 2\min/5\rfloor`$ of $`\lvert x\rvert,\lvert y\rvert`$ |
| `mathi_even_floor(v)`, `mathi_even_ceil(v)` | round down or up to an even value | $`2\lfloor v/2\rfloor`$, $`2\lceil v/2\rceil`$ |
| `mathi_size_ceil(bytes, alignment)` | byte count rounded up to a power-of-two alignment | $`\lceil bytes/alignment\rceil\,alignment`$ |

## mathf.h

| Function | Purpose | Formula |
|---|---|---|
| `mathf_zero()`, `mathf_half()`, `mathf_one()`, `mathf_two()`, `mathf_four()` | scalar constants | $`0, 1/2, 1, 2, 4`$ |
| `mathf_abs(a)` | absolute value; negative zero becomes positive zero | $`\lvert a\rvert`$ |
| `mathf_lerp(a, b, t)` | single-precision interpolation | $`a + (b - a)t`$ |
| `mathf_min(a, b)` | smaller value by comparison | $`\min(a,b)`$ |
| `mathf_max(a, b)` | larger value by comparison | $`\max(a,b)`$ |
| `mathf_clamp(v, lo, hi)` | inclusive bounds | $`\min(\max(v,lo),hi)`$ |
| `mathf_repeat(v, len)` | into `[0, len)`; `len > 0`; a remainder rounded to `len` maps to zero | $`v - \lfloor v/len\rfloor len`$ |
| `mathf_wrap(v, lo, hi)` | into `[lo, hi)`; `lo < hi` | $`lo + \mathrm{repeat}(v-lo,hi-lo)`$ |
| `mathf_add(a, b)`, `mathf_sub(a, b)`, `mathf_neg(a)`, `mathf_mul(a, b)`, `mathf_div(a, b)` | arithmetic the vector templates call | $`a+b`$, $`a-b`$, $`-a`$, $`ab`$, $`a/b`$ |
| `mathf_wadd(a, b)`, `mathf_wmul(a, b)` | the wide forms are the same in float | $`a+b`$, $`ab`$ |
| `mathf_sqrt(a)` | square root | $`\sqrt a`$ |
| `mathf_half_sin(angle)`, `mathf_half_cos(angle)` | sine and cosine of half a radian angle | $`\sin(a/2)`$, $`\cos(a/2)`$ |
| `mathf_dot2c(a0, b0, a1, b1, c)`, `mathf_dot3c(a0, b0, a1, b1, a2, b2, c)` | products summed left to right plus `c` | $`\sum a_ib_i+c`$ |

## maths.h

Lerp and range helpers widen to `mathi.h`; lerp weights are int32 and the step truncates toward `a`.

| Function | Purpose | Formula |
|---|---|---|
| `maths_lerp(a, b, t, n)` | interpolation across the int16 range | $`a + \mathrm{trunc}((b-a)t/n)`$ |
| `maths_min(a, b)`, `maths_max(a, b)`, `maths_clamp(v, lo, hi)`, `maths_repeat(v, len)`, `maths_wrap(v, lo, hi)` | range helpers on the value | as in `mathi.h` |
| `maths_saturate(v)` | int32 clamped into int16 | $`\min(\max(v,-2^{15}),2^{15}-1)`$ |
| `maths_zero()`, `maths_one()` | int16 constants | $`0, 1`$ |
| `maths_add(a, b)`, `maths_sub(a, b)`, `maths_neg(a)`, `maths_mul(a, b)` | arithmetic that saturates to int16 | $`\mathrm{sat}(a+b)`$, $`\mathrm{sat}(a-b)`$, $`\mathrm{sat}(-a)`$, $`\mathrm{sat}(ab)`$ |
| `maths_wadd(a, b)`, `maths_wmul(a, b)` | widened int64 sum and product | $`a+b`$, $`ab`$ |

## mathx.h

`mathx_lerp` saturates as `mathx_add` and `mathx_mul` do; the range helpers apply `mathi.h` to the raw Q16.16 value.

| Function | Purpose | Formula |
|---|---|---|
| `mathx_zero()`, `mathx_half()`, `mathx_one()`, `mathx_two()`, `mathx_four()` | Q16.16 constants | $`0, 1/2, 1, 2, 4`$ |
| `mathx_abs(a)` | saturated absolute value; `INT32_MIN` maps to `INT32_MAX` | $`\min(\lvert a\rvert,\mathrm{INT32\_MAX})`$ |
| `mathx_lerp(a, b, t)` | interpolation by a Q16.16 weight `t` | $`\mathrm{add}(a,\mathrm{mul}(\mathrm{sub}(b,a),t))`$ |
| `mathx_min(a, b)`, `mathx_max(a, b)`, `mathx_clamp(v, lo, hi)`, `mathx_repeat(v, len)`, `mathx_wrap(v, lo, hi)` | range helpers on the value | as in `mathi.h` |
| `mathf_to_x(v)` | Q16.16, rounded and saturated | $`\mathrm{round}(v\,2^{16})`$ |
| `mathx_to_f(v)` | float | $`v/2^{16}`$ |
| `mathx_saturate(v)` | int64 clamped into int32 | $`\min(\max(v,\mathrm{INT32\_MIN}),\mathrm{INT32\_MAX})`$ |
| `mathx_sub(a, b)`, `mathx_neg(a)` | saturating difference and negation | $`\mathrm{sat}(a-b)`$, $`\mathrm{sat}(-a)`$ |
| `mathx_div(num, den)` | rounded Q16.16 quotient, saturated | $`\mathrm{sat}(\mathrm{round}(num\,2^{16}/den))`$ |
| `mathx_sqrt(v)` | square root; a negative value gives 0 | $`\sqrt v`$ |
| `mathx_sin_turns(turns)`, `mathx_cos_turns(turns)` | sine and cosine of a Q16.16 turn count, from `trig.h` | $`\sin 2\pi t`$, $`\cos 2\pi t`$ |
| `mathx_half_sin(turns)`, `mathx_half_cos(turns)` | sine and cosine of half the turn count | $`\sin \pi t`$, $`\cos \pi t`$ |
| `mathx_wadd(a, b)`, `mathx_wmul(a, b)` | the wide forms saturate like `mathx_add` and `mathx_mul` | $`\mathrm{sat}(a+b)`$, $`\mathrm{sat}(ab)`$ |
| `mathx_dot2c(...)`, `mathx_dot3c(...)` | products floored to Q16.16, the 32-bit sum wraps; inputs inside $`\pm 2^{15}`$ | $`\sum\lfloor a_ib_i/2^{16}\rfloor+c`$ |
| `mathx_dot3_narrow(a0, b0, a1, b1, a2, b2, c, shift)` | 32-bit products and sum, shifted right; the caller proves the sum fits int32 | $`(\sum a_ib_i+c)\gg shift`$ |
| `mathx_normalize_n(in, out, n, t, q)` | scales up to four components to unit length and `t` by the same factor into `q`; zero input gives zero | $`out_i=in_i/L`$, $`q=t/L`$ |

`mathx_rsqrt_q30(m)` accepts $`2^{30} \le m < 2^{32}`$ and returns
$`2^{30}/\sqrt{m/2^{32}}`$ rounded to nearest.
`MATHX_RSQRT_Q30_RELATIVE_LIMIT` bounds relative error by $`2^{-31}`$.
On Xtensa with `CONFIG_MATH_FPU_RECIP`, an FPU seed (`mathf_rsqrt_normal`) and one integer Newton correction replace the seed table and three corrections.
`mathx_rsqrt_q30_table(m)` is the table path; both use `mathx_rsqrt_newton(m, y)` for a correction and `mathx_rsqrt_bracket(m, y)` for the final rounding.
The midpoint bracket makes both paths return the same bits. The FPU path pins its task to a core and is not ISR-safe.

The exhaustive checker runs in host CI, separately from unit tests. It also proves that one correction from any seed within `MATHX_RSQRT_FPU_SEED_RELATIVE_LIMIT` gives the table result:

```sh
cc -O3 -Ilauncher/packages/math/include launcher/test/math_rsqrt_check.c -lm -o /tmp/math_rsqrt_check
/tmp/math_rsqrt_check
```

`mathx_rsqrt(v)` returns Q16.16 reciprocal square root, with error at most
0.5 LSB plus `MATHX_RSQRT_RELATIVE_LIMIT` times the exact result.
Nonpositive input gives `INT32_MAX`; positive input uses `mathx_rsqrt_q30`.

`mathx_rsqrt_product(m, square)` and `mathx_rsqrt_midpoint(m, y)` are the
exact integer steps of `mathx_rsqrt_q30`: the product's upper 64 bits, and the
test $`m(y+\tfrac12)^2 \le 2^{92}`$ for rounding to nearest.

## mathf_round.h

| Function | Purpose | Formula |
|---|---|---|
| `mathf_clamp_finite(v, lo, hi)` | finite ordered bounds; NaN maps to `lo`, infinities to the corresponding bound | $`\min(\max(v,lo),hi)`$ for finite inputs |
| `mathf_round_i32(v)` | nearest int32, ties away from zero; saturates | $`\mathrm{round}(v)`$ |
| `mathf_round_i32_in_range(v)` | as `mathf_round_i32` for a finite input whose nearest integer fits int32; no limit tests | $`\mathrm{round}(v)`$ |
| `mathf_round_i16(v)` | nearest int16, ties away from zero; saturates, NaN maps to `INT16_MIN` | $`\mathrm{round}(\mathrm{clamp}(v))`$ |
| `mathf_round_s(v, scale)` | int16, rounded and saturated | $`\mathrm{round}(v/scale)`$ |

`MATH_FLOAT_INT_LIMIT` is the largest float below 2^31 that converts to int32.

## Reciprocal (`mathf.h`)

| Function | Board formula | Bound source |
|---|---|---|
| `mathf_recip(x)` | `recip0.s` seed, two Newton steps | `MATHF_RECIP_ULP_LIMIT` in `mathf.h` |
| `mathf_recip_approx(x)` | `recip0.s` seed, one Newton step | `MATHF_RECIP_APPROX_RELATIVE_LIMIT` / `MATHF_RECIP_APPROX_BITS` in `mathf.h` |

Both accept normal inputs with normal reciprocals, of either sign; host and QEMU use `1.0F / x`.

## Reciprocal square root (`mathf.h`)

| Function | Board formula | Bound source |
|---|---|---|
| `mathf_rsqrt(x)` | `rsqrt0.s` seed, two Newton steps | `MATHF_RSQRT_ULP_LIMIT` in `mathf.h` |
| `mathf_rsqrt_approx(x)` | `rsqrt0.s` seed, one Newton step | `MATHF_RSQRT_APPROX_RELATIVE_LIMIT` in `mathf.h` |
| `mathf_rsqrt_normal(x)` | `mathf_rsqrt` without the subnormal fallback, for positive normal `x` (Xtensa FPU builds only) | `MATHF_RSQRT_ULP_LIMIT` in `mathf.h` |

Newton-Raphson reciprocal square root uses
$`y_{n+1}=y_n+\tfrac12 y_n\,(1-(xy_n)y_n)`$.<sup>[[85]](../Citations.md#muller-2018)</sup>
The approximate path evaluates its single correction as $`y_0\,(1.5-0.5xy_0^2)`$.

`CONFIG_MATH_FPU_RECIP` enables the board seed. Host and QEMU use
`1.0F / sqrtf(x)`. For `x < FLT_MIN`, including zero and subnormals,
`mathf_rsqrt` uses that same formula and preserves its bits.

The limits in `mathf.h` define the accuracy contract; the
[fast-path evidence](Fast-Paths.md) reports measured maxima and coverage.

The suite's integer product bracket rounds the exact reciprocal root without
using the twice-rounded `1 / sqrtf` as a reference. The suite prints
`MATHERROR` lines and checks both limits.

## ease.h

Timeline ramps and easing curves. Time is milliseconds; the `_u8` functions work
on a byte fraction, 0 to 255, that only ever scales a value.

| Function | Purpose | Formula |
|---|---|---|
| `ease_ramp_u8(now, start, dur)` | 0 until `start`, 255 from `start + dur`, linear between; `dur` of 0 jumps at `start` | $`\lfloor 255\,(now-start)/dur\rfloor`$ |
| `ease_out_u8(x)` | fast off the mark, settling; endpoints exact | $`255-(255-x)^2/255`$ |
| `ease_in_u8(x)` | mirror of `ease_out_u8`: slow off the mark | $`x^2/255`$ |
| `ease_smooth_q8(elapsed, over)` | smoothstep out of 256, slow at both ends; 256 from `over` on and for `over` of 0 | $`t=\lfloor 256\,elapsed/over\rfloor`$<br>$`t^2(768-2t)/2^{16}`$ |

## math_db.h

`math_db_power(ratio)` is $`10\log_{10}`$ and `math_db_amplitude(ratio)` is $`20\log_{10}`$ of a nonnegative ratio, floored at `MATH_DB_FLOOR` (-120 dB).

## fixed.h

Fixed-point multiplication, division and rounding at a caller-selected shift:

| Function | Behaviour |
|---|---|
| `fx_mul_floor` | widens, then floors a negative product |
| `fx_mul_round` | widens, then rounds to nearest, ties away from zero |
| `fx_div_round_wide` | nonzero denominator; keeps the wide result |
| `fx_div_round` | nonzero denominator; wraps a result outside int32 |
| `fx_round_shift` | int64 shifted right, nearest with ties away from zero |
| `fx_round_shift32` | `fx_round_shift` in int32 when `abs(v) + half` fits |

## rng.h

`rng_t` is an explicitly seeded xorshift32 generator.

| Function | Behaviour |
|---|---|
| `rng_seed` | sets the state; a zero seed becomes a nonzero state |
| `rng_next` | advances the state |
| `rng_below` | a draw below a positive bound, by modulo: slightly biased when the bound does not divide the range |
| `rng_chance` | never and always consume no draw |
| `rng_hash` | a counter-based draw with no state, mixed by `rng_avalanche32` |

## trig.h

`trig_sin` and `trig_cos` take a uint16 phase, `TRIG_TURN` to a turn, and return
integers with `TRIG_SIN_SHIFT` fractional bits, at most `TRIG_SIN_MAX`.

`trig_sin_quadrant` interpolates the quarter-wave table; cosine adds `TRIG_QUARTER_TURN`.

## units.h

`MATH_S_PER_MIN`, `MATH_MS_PER_MIN`, `MATH_MS_PER_S`, `MATH_US_PER_MS` and
`MATH_US_PER_S` own integer time conversion factors between minutes, seconds,
milliseconds and microseconds.
