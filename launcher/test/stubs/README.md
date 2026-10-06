Stand-ins for the ESP-IDF and board-support headers, so that code written
for the board can be compiled on a host.

They are declarations only, not a port. `check_app_sources.sh` compile-checks
the hardware-facing `app_*.c` and `scene_*.c` files against them, and
`check_inline_owners.sh` builds a caller of `util/runtime/timing.h` and
`util/runtime/memory.h` as the board would. The host test build and the host renders
also have this folder on their include path. Only one stub has a definition
behind it: `test/heap_arena.c` implements `esp_heap_caps.h` as the board's
two heap pools, for the host tests.

Each stub declares exactly what the real header is used for and no more, so
adding a new IDF call means adding a line here, deliberately.
