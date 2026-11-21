# image

Strided image views, RGB565 pixel arithmetic, ordered dithering, palette lookup,
the sRGB transfer function and OKLab ([colour reference](../../../docs/image/Colour.md)).
This header-only package uses math for rectangles and `vec3f_t`; its tests run with testkit.

`image_t` borrows pixels with a format and byte pitch.
`IMAGE_U16` and `IMAGE_U32` describe raw maps whose meaning belongs to their caller.
`IMAGE_A8` is one coverage byte a pixel, 0 clear to 255 solid.

`image_format_bits()` gives bit depth; `IMAGE_PITCH()` rounds row bytes up.
`image_row()` addresses a row; `image_sub()` shares pixels and pitch.
`image_mono1_ink()` reads a packed ink bit.

`rgb565_*` operates on native RGB565.
`rgb565be_*` operates on high-byte-first RGB565.

<!-- generated: package-image-headers check: python launcher/tools/gen/package_headers.py --check -->
| Header | Purpose |
|---|---|
| [include/image/colour/oklab.h](include/image/colour/oklab.h) | oklab: the OKLab perceptual colour space (Ottosson 2020) from and to linear-light sRGB. |
| [include/image/colour/srgb.h](include/image/colour/srgb.h) | srgb: the sRGB transfer function of IEC 61966-2-1, between linear light and encoded [0, 1]. |
| [include/image/image.h](include/image/image.h) | image: strided pixel views without allocation or copying. |
| [include/image/palette/palette.h](include/image/palette/palette.h) | palette: named RGB565 entries and lookup through a caller-owned reverse map. |
| [include/image/pixel/bayer.h](include/image/pixel/bayer.h) | bayer: ordered dithering at absolute pixel coordinates. |
| [include/image/pixel/rgb565.h](include/image/pixel/rgb565.h) | rgb565: native and high-byte-first RGB565 pixel arithmetic. |
| [include/image/pixel/rgb565_layout.h](include/image/pixel/rgb565_layout.h) | rgb565_layout: where each channel lies in a native RGB565 pixel. |
| [include/image/pixel/rgb888.h](include/image/pixel/rgb888.h) | rgb888: interpolation and hue generation for 0xRRGGBB colours. |
<!-- /generated: package-image-headers -->
