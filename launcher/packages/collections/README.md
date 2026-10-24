# collections

Containers and small algorithms over storage the caller owns: nothing here
allocates. Header-only and inline, so a comparator or a bit index folds into
its caller; it needs only the C library.

Include `"collections/<name>.h"`. Firmware declares `REQUIRES collections`;
plain CMake builds link the `collections` target. Host scripts find the
headers and suites through `launcher/tools/build/packages.sh`.

## Headers

<!-- generated: package-collections-headers check: python launcher/tools/gen/shared_helpers.py --check -->
| Header | Purpose |
|---|---|
| [include/collections/bits.h](include/collections/bits.h) | bits: one flag per index in caller-owned bytes or 32-bit words, bit 0 first; inline so an index folds into its caller. |
| [include/collections/sort.h](include/collections/sort.h) | sort: stable in-place insertion sort of small arrays. |
<!-- /generated: package-collections-headers -->
