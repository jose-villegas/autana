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
| [include/collections/sort.h](include/collections/sort.h) | sort: stable in-place ordering of a small array by a qsort-shaped comparator, inline so the comparator inlines. |
<!-- /generated: package-collections-headers -->
