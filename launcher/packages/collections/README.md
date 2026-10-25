# collections

Containers and small algorithms over storage the caller owns: nothing here
allocates. Header-only and inline, so a comparator or a bit index folds into
its caller; it needs only the C library.

Include `"collections/<name>.h"`. Firmware declares `REQUIRES collections`;
plain CMake builds link the `collections` target. Host scripts find the
headers and suites through `launcher/tools/build/packages.sh`.

## Headers

<!-- generated: package-collections-headers check: python launcher/tools/gen/package_headers.py --check -->
| Header | Purpose |
|---|---|
| [include/collections/bits.h](include/collections/bits.h) | bits: one flag per index in caller-owned bytes or 32-bit words, bit 0 first; inline so an index folds into its caller. |
| [include/collections/flat_map.h](include/collections/flat_map.h) | flat_map: rows found by a 32-bit key the rows carry, kept in ascending key order by whoever wrote them, so a find is a binary search where the rows already sit and nothing is built or copied. |
| [include/collections/sort.h](include/collections/sort.h) | sort: stable in-place insertion sort of small arrays. |
| [include/collections/strbuf.h](include/collections/strbuf.h) | strbuf: text built by appending pieces into a caller's fixed buffer. |
<!-- /generated: package-collections-headers -->
