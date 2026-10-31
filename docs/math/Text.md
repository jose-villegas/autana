# Math text

Include `math/text/math_text.h` to format or parse a math value without allocation.
The scalar functions are linked from the math package; composite functions use
its scalar OPS families through `text_template.h`.

## text

`P_format(`, `P_parse(` and `P_text(` provide the operations for each prefix below.
`math_text_copy` copies formatted text into a bounded, NUL-terminated buffer.

| Value | Format / parse | By value | Buffer capacity | Chars, with NUL |
|---|---|---|---|---|
| float | `mathf_format`, `mathf_parse` | `mathf_text` | `MATHF_TEXT_MAX` | 16 |
| int32 | `mathi_format`, `mathi_parse` | `mathi_text` | `MATHI_TEXT_MAX` | 12 |
| int16 | `maths_format`, `maths_parse` | `maths_text` | `MATHS_TEXT_MAX` | 7 |
| uint32 | `mathu_format`, `mathu_parse` | `mathu_text` | `MATHU_TEXT_MAX` | 11 |
| Q16.16 | `mathx_format`, `mathx_parse` | `mathx_text` | `MATHX_TEXT_MAX` | 13 |
| vec2f | `vec2f_format`, `vec2f_parse` | `vec2f_text` | `VEC2F_TEXT_MAX` | 32 |
| vec2i | `vec2i_format`, `vec2i_parse` | `vec2i_text` | `VEC2I_TEXT_MAX` | 24 |
| vec2s | `vec2s_format`, `vec2s_parse` | `vec2s_text` | `VEC2S_TEXT_MAX` | 14 |
| vec2x | `vec2x_format`, `vec2x_parse` | `vec2x_text` | `VEC2X_TEXT_MAX` | 26 |
| vec3f | `vec3f_format`, `vec3f_parse` | `vec3f_text` | `VEC3F_TEXT_MAX` | 48 |
| vec3i | `vec3i_format`, `vec3i_parse` | `vec3i_text` | `VEC3I_TEXT_MAX` | 36 |
| vec3s | `vec3s_format`, `vec3s_parse` | `vec3s_text` | `VEC3S_TEXT_MAX` | 21 |
| vec3x | `vec3x_format`, `vec3x_parse` | `vec3x_text` | `VEC3X_TEXT_MAX` | 39 |
| quatf | `quatf_format`, `quatf_parse` | `quatf_text` | `QUATF_TEXT_MAX` | 64 |
| quatx | `quatx_format`, `quatx_parse` | `quatx_text` | `QUATX_TEXT_MAX` | 52 |
| mat3f | `mat3f_format`, `mat3f_parse` | `mat3f_text` | `MAT3F_TEXT_MAX` | 144 |
| mat4f | `mat4f_format`, `mat4f_parse` | `mat4f_text` | `MAT4F_TEXT_MAX` | 256 |
| mat4x | `mat4x_format`, `mat4x_parse` | `mat4x_text` | `MAT4X_TEXT_MAX` | 208 |
| rectf | `rectf_format`, `rectf_parse` | `rectf_text` | `RECTF_TEXT_MAX` | 66 |
| recti | `recti_format`, `recti_parse` | `recti_text` | `RECTI_TEXT_MAX` | 50 |
| rects | `rects_format`, `rects_parse` | `rects_text` | `RECTS_TEXT_MAX` | 30 |
| rectx | `rectx_format`, `rectx_parse` | `rectx_text` | `RECTX_TEXT_MAX` | 54 |

`mathu` is scalar-only and unsigned: no sign, decimal. `mathu_parse_hex` reads the `0x`-prefixed hexadecimal form
with the same `end` rule.

Each prefix provides the same operations, with its own value type:

```c fragment
int vec2i_format(char* buf, size_t cap, vec2i_t value);
bool vec2i_parse(const char* text, const char** end, vec2i_t* out);
typedef struct { char chars[VEC2I_TEXT_MAX]; } vec2i_text_t;
vec2i_text_t vec2i_text(vec2i_t value);
```

Format returns the required length excluding NUL, even when truncated. A nonzero
capacity always gets a NUL terminator. The named capacity includes NUL and never
truncates; `format(NULL, 0, value)` measures without writing.

Parse rejects leading or component whitespace, malformed text and out-of-range
values. Failure leaves `out` and `end` untouched. With `end == NULL`, the whole
string must match; otherwise `*end` points just after the value.

| Value | Canonical form | Example |
|---|---|---|
| vec2 | `x,y` | `12,-3` |
| vec3 | `x,y,z` | `0.1,1,-2.5` |
| quaternion, struct order | `x,y,z,w` | `0,0,0,1` |
| matrix, `m[row][col]` | Columns separated by `,`, rows by `;` | `1,0,0;0,1,0;0,0,1` |
| rectangle | `WxH+X+Y`, negative offsets use `-` | `64x48-10+20` |

Rectangle formatting always includes both offsets. Parsing accepts dimensions
alone: `640x480` gives `{0,0,640,480}`. No form contains spaces or brackets.
For a JSON vector array, print `[` followed by its text and `]`.

| Scalar | Decimal text |
|---|---|
| float | First `%g` precision from 1 through 9 that parses to identical bits; `0.1f` gives `0.1`, negative zero gives `-0` |
| nonfinite float | `inf`, `-inf`, `nan`; NaN payload and sign are normalized |
| int32 / int16 | Signed decimal integer; parsing checks the destination range |
| Q16.16 | Real value with the shortest fractional precision through five decimal places; raw `1` gives `0.00002` |

Float conversion uses the C numeric locale. Q16.16 conversion uses integer
arithmetic, rounding parsed decimals to the nearest raw step with ties away
from zero. Its parser accepts a decimal point and sign, without an exponent.

Each `_text` function returns a struct with a `chars` array sized to the named
capacity. Its text matches `_format`; the temporary lasts through the full
expression, so it can be passed directly to a print function:

```c fragment
printf("[%s]", vec3f_text(p).chars);
```
