# image

Strided image views, RGB565 pixel arithmetic, ordered dithering and palette lookup.
This header-only package uses math for rectangles; its tests run with testkit.

`image_t` borrows pixels with a format and byte pitch.
`IMAGE_U16` and `IMAGE_U32` describe raw maps whose meaning belongs to their caller.

`image_format_bits()` gives bit depth; `IMAGE_PITCH()` rounds row bytes up.
`image_row()` addresses a row; `image_sub()` shares pixels and pitch.
`image_mono1_ink()` reads a packed ink bit.

`rgb565_*` operates on native RGB565.
`rgb565be_*` operates on high-byte-first RGB565.

<!-- generated: package-image-headers check: python launcher/tools/gen/package_headers.py --check -->
| Header | Purpose |
|---|---|
| [include/image/image.h](include/image/image.h) | image: strided pixel views without allocation or copying. |
| [include/image/palette/palette.h](include/image/palette/palette.h) | palette: named RGB565 entries and lookup through a caller-owned reverse map. |
| [include/image/pixel/bayer.h](include/image/pixel/bayer.h) | bayer: ordered dithering at absolute pixel coordinates. |
| [include/image/pixel/rgb565.h](include/image/pixel/rgb565.h) | rgb565: native and high-byte-first RGB565 pixel arithmetic. |
| [include/image/pixel/rgb888.h](include/image/pixel/rgb888.h) | rgb888: interpolation and hue generation for 0xRRGGBB colours. |
<!-- /generated: package-image-headers -->
