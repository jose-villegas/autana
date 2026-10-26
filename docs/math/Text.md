# Math text

Include `math/text/math_text.h` to format or parse a math value without allocation.
The scalar functions are linked from the math package; composite functions use
its scalar OPS families through `text_template.h`.

| Value | Format / parse | Buffer capacity |
|---|---|---|
| float | `mathf_format`, `mathf_parse` | `MATHF_TEXT_MAX` |
| int32 | `mathi_format`, `mathi_parse` | `MATHI_TEXT_MAX` |
| int16 | `maths_format`, `maths_parse` | `MATHS_TEXT_MAX` |
| Q16.16 | `mathx_format`, `mathx_parse` | `MATHX_TEXT_MAX` |
| vec2f | `vec2f_format`, `vec2f_parse` | `VEC2F_TEXT_MAX` |
| vec2i | `vec2i_format`, `vec2i_parse` | `VEC2I_TEXT_MAX` |
| vec2s | `vec2s_format`, `vec2s_parse` | `VEC2S_TEXT_MAX` |
| vec2x | `vec2x_format`, `vec2x_parse` | `VEC2X_TEXT_MAX` |
| vec3f | `vec3f_format`, `vec3f_parse` | `VEC3F_TEXT_MAX` |
| vec3i | `vec3i_format`, `vec3i_parse` | `VEC3I_TEXT_MAX` |
| vec3s | `vec3s_format`, `vec3s_parse` | `VEC3S_TEXT_MAX` |
| vec3x | `vec3x_format`, `vec3x_parse` | `VEC3X_TEXT_MAX` |
| quatf | `quatf_format`, `quatf_parse` | `QUATF_TEXT_MAX` |
| quatx | `quatx_format`, `quatx_parse` | `QUATX_TEXT_MAX` |
| mat3f | `mat3f_format`, `mat3f_parse` | `MAT3F_TEXT_MAX` |
| mat4f | `mat4f_format`, `mat4f_parse` | `MAT4F_TEXT_MAX` |
| mat4x | `mat4x_format`, `mat4x_parse` | `MAT4X_TEXT_MAX` |
| rectf | `rectf_format`, `rectf_parse` | `RECTF_TEXT_MAX` |
| recti | `recti_format`, `recti_parse` | `RECTI_TEXT_MAX` |
| rects | `rects_format`, `rects_parse` | `RECTS_TEXT_MAX` |
| rectx | `rectx_format`, `rectx_parse` | `RECTX_TEXT_MAX` |

Each prefix provides the same pair, with its own value type:

```c
int vec2i_format(char* buf, size_t cap, vec2i_t value);
bool vec2i_parse(const char* text, const char** end, vec2i_t* out);
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

The template uses `math_text_copy()` to apply the buffer contract to completed
composite text. Vector components use the iterators in `vec_ops_template.h`.
