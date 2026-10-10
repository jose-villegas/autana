# gfx

The offline half of `main/gfx/image/`: bakes a PNG into a pack entry the
firmware draws in place. The entry's layout is
[the image entry](../../../docs/assets/README.md#the-image-entry). Nothing
here runs on the board.

| File | Purpose |
|---|---|
| [image_asset.py](image_asset.py) | Writes and reads the image entry, baked from a `NAME.image.toml` and the PNG it names. |
