# Scalar

Per-type arithmetic, fixed point, random draws and units. [Math index](README.md).

`math/scalar/` holds per-type arithmetic, fixed point, random draws, trigonometry
and time units. The tables list the range, lerp and rounding helpers; the
[catalogue](../Shared-Helpers.md) lists the rest.

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

## maths.h

Lerp and range helpers widen to `mathi.h`; lerp weights are int32 and the step truncates toward `a`.

| Function | Purpose | Formula |
|---|---|---|
| `maths_lerp(a, b, t, n)` | interpolation across the int16 range | $`a + \mathrm{trunc}((b-a)t/n)`$ |
| `maths_min(a, b)`, `maths_max(a, b)`, `maths_clamp(v, lo, hi)`, `maths_repeat(v, len)`, `maths_wrap(v, lo, hi)` | range helpers on the value | as in `mathi.h` |

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

## mathf_round.h

| Function | Purpose | Formula |
|---|---|---|
| `mathf_clamp_finite(v, lo, hi)` | finite ordered bounds; NaN maps to `lo`, infinities to the corresponding bound | $`\min(\max(v,lo),hi)`$ for finite inputs |
| `mathf_round_i32(v)` | nearest int32, ties away from zero; saturates | $`\mathrm{round}(v)`$ |
| `mathf_round_i32_in_range(v)` | as `mathf_round_i32` for a finite input whose nearest integer fits int32; no limit tests | $`\mathrm{round}(v)`$ |
| `mathf_round_i16(v)` | nearest int16, ties away from zero; saturates, NaN maps to `INT16_MIN` | $`\mathrm{round}(\mathrm{clamp}(v))`$ |
| `mathf_round_s(v, scale)` | int16, rounded and saturated | $`\mathrm{round}(v/scale)`$ |

`MATH_FLOAT_INT_LIMIT` is the largest float below 2^31 that converts to int32.

## mathf_rsqrt.h

| Function | Purpose | Formula |
|---|---|---|
| `mathf_rsqrt(x)` | reciprocal square root for positive normal-range inputs; on the board the FPU's `rsqrt0.s` seed plus one Newton step, on host and QEMU the exact `1 / sqrt(x)` | $`y_0 = \mathrm{rsqrt0}(x)`$<br>$`y_1 = y_0\,(3 - x\,y_0^2) / 2`$ |

The device result is within `MATHF_RSQRT_RELATIVE_LIMIT` relative error of the exact value, which the device sweep checks.

## fixed.h

Fixed-point multiplication, division and rounding at a caller-selected shift:

| Function | Behaviour |
|---|---|
| `fx_mul_floor` | widens, then floors a negative product |
| `fx_mul_round` | widens, then rounds to nearest, ties away from zero |
| `fx_div_round_wide` | nonzero denominator; keeps the wide result |
| `fx_div_round` | nonzero denominator; wraps a result outside int32 |

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
