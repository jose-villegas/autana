# asset

The offline half of `main/asset/`.

| Module | What it does |
|---|---|
| [asset_pack.py](asset_pack.py) | The one writer of the [asset pack](../../../docs/assets/README.md) container and the bundle directory: `build_pack()` lays entries out under a header with a CRC-32, `build_directory()` lays bundles out on their own sectors, and `parse_pack()` and `parse_directory()` make the checks the firmware makes. Standard library only. |

What goes in an entry is the business of the tool that owns its type; the lit
mesh's is [`r3d/build_pack.py`](../r3d/build_pack.py), which writes the tree's bundles.
