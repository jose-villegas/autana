# image

Panel-independent RGB565 pixel arithmetic, ordered dithering and palette lookup.
This header-only package has no package dependencies; its tests run with testkit.

<!-- generated: package-image-headers check: python launcher/tools/gen/package_headers.py --check -->
| Header | Purpose |
|---|---|
| [include/image/palette/palette.h](include/image/palette/palette.h) | palette: named RGB565 entries and lookup through a caller-owned reverse map. |
| [include/image/pixel/bayer.h](include/image/pixel/bayer.h) | bayer: ordered dithering at absolute pixel coordinates. |
| [include/image/pixel/rgb565.h](include/image/pixel/rgb565.h) | rgb565: native and high-byte-first RGB565 pixel arithmetic. |
<!-- /generated: package-image-headers -->
