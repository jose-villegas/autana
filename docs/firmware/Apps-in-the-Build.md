# Apps in the Build

How to write one is [Building-an-App.md](Building-an-App.md). What matters
architecturally:

- The build globs `apps/**/*.c` and `shell/apps/**/*.c`, and each app registers
  itself from its own file, so adding or deleting an app touches no other file.
- The component is linked `WHOLE_ARCHIVE`, because nothing references an app by
  name.
- A bench-only app declares itself the same way: a `development_only.cmake` in its
  folder leaves it out when `CONFIG_LAUNCHER_DEVELOPMENT` is off. The build names
  no application, and deleting the folder deletes the declaration.
- What each build variant carries is [Build-Variants.md](../tools/Build-Variants.md).

The shell takes one app region from PSRAM at boot, leaving the system
reserve defined in `app/app_arena.h`. Each app entry recreates an empty
TLSF heap over it; exit drops that heap after app and system cleanup.

| Allocation | Release |
|---|---|
| `app_arena_take(size, align)` | Nested marks and rewinds; chunks come from the app heap |
| `memory_alloc(bytes, MEMORY_APP)` | `memory_free(pointer)` within the visit |
| Whole app region | Reused empty on the next entry; never returned to the system heap |

See [App memory](Building-an-App.md#app-memory) for the lifecycle and reserve.
