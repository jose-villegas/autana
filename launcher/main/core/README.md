# core

The platform base every layer above builds on: jobs handed to the second core, memory placed by kind, time, and tunables changed over the console.

`cache.h` scopes hardware data-cache autoload to an address region, with nested register save and restore. It affects both cores; host and QEMU calls are no-ops.
