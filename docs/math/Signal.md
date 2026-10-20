# Signal: goertzel.h

Single-bin detection on sample blocks; unlike the other legs it is compiled C, not header-only. [Math index](README.md).

`goertzel_t` accumulates one frequency across sample blocks with a float
recurrence, including non-integer DFT bins.<sup>[[72]](../Citations.md#72)</sup>
The caller supplies frequency and sample rate in Hz, with $`0 < hz < sample\_rate\_hz/2`$ (at DC the float power cancels), and a positive full scale.

| Function | Result |
|---|---|
| `goertzel_init` | Clear the stream and set coefficient $`c = 2\cos(2\pi\,hz/sample\_rate\_hz)`$ |
| `goertzel_reset` | Clear the stream, retaining the coefficient |
| `goertzel_feed_s` | Feed signed 16-bit samples |
| `goertzel_feed_f` | Feed float samples with the same recurrence |
| `goertzel_phase` | DFT phase in radians, sign $`e^{-j\omega n}`$, referenced to the first sample since init/reset; $`(-\pi, \pi]`$, zero for an empty stream |
| `goertzel_power` | Squared unnormalized DFT magnitude, clamped to zero |
| `goertzel_amplitude` | Sine amplitude $`2\sqrt{P}/N`$; zero before any sample |
| `goertzel_level_db` | Sine level relative to full scale; floored at `MATH_DB_FLOOR` |

Phase accuracy is about 0.001 rad on short records and up to 0.03 rad at
65,537 off-bin samples. The error comes from the float recurrence, not the
phase step.

For each sample $`x`$, the recurrence updates the two stored values:

```math
s = x + c s_1 - s_2, \qquad s_2 \gets s_1, \qquad s_1 \gets s
```

With $`N`$ samples and full scale $`F`$, power and level are:

```math
P = \max(0, s_1^2 + s_2^2 - c s_1 s_2), \qquad S = NF/2
```

```math
L = \max(\mathrm{MATH\_DB\_FLOOR}, 10\log_{10}(P/S^2))
```

An empty stream or zero power returns the level floor. A sine of amplitude
$`F`$ reads 0 dB; doubling the reference full scale lowers its level by
$`20\log_{10}(2)`$ dB.
