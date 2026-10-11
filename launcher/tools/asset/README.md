# asset

The offline half of `main/asset/`.

| Module | What it does |
|---|---|
| [asset_pack.py](asset_pack.py) | The writer of the [asset pack](../../../docs/assets/README.md) container: `build_pack()` lays entries out under a header with a CRC-32 and `parse_pack()` checks it. Standard library only. |
| [asset_directory.py](asset_directory.py) | `build_directory()` writes version-2 directories with every pack starting on an MMU page; padding costs less than one page per pack. `parse_directory()` checks them. Page size and version come from `main/asset/asset_directory.h`. |

What goes in an entry is the business of the tool that owns its type; the lit
mesh's is [`r3d/build_pack.py`](../r3d/build_pack.py), which writes the tree's packs.
