# asset

The offline half of `main/asset/`.

| Module | What it does |
|---|---|
| [asset_pack.py](asset_pack.py) | The one writer of the [asset pack](../../../docs/assets/README.md) container: `build_pack()` lays entries out under a header with a CRC-32, and `parse_pack()` makes the checks the firmware makes. Standard library only. |

What goes in an entry is the business of the tool that owns its type; the lit
mesh's is [`r3d/build_pack.py`](../r3d/build_pack.py), which packs the baked meshes in the tree.
