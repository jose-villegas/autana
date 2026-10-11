# gfx

The offline half of `main/gfx/image/`: bakes a PNG, or an icon cut from an
atlas or an SVG, into a pack entry the firmware draws in place. The entry's layout is
[the image entry](../../../docs/assets/README.md#the-image-entry). Nothing
here runs on the board.

| File | Purpose |
|---|---|
| [ridge_curve.py](ridge_curve.py) | Bakes a continuous drawn curve from `NAME.curve.toml` and its PNG into a named int16 table row; uses the shared PNG decoder and panel size. |
| [image_asset.py](image_asset.py) | Writes and reads the image entry, baked from a `NAME.image.toml` and the PNG it names. |
| [icons_asset.py](icons_asset.py) | Cuts each icon of a `NAME.icons.toml` out of its atlas or SVG into a one-bit image entry ([icons](../../../docs/tools/Icon-Baker.md)). |
