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

**App memory is lent, not owned.** The shell holds one static block in PSRAM, the
app arena (`APP_ARENA_BYTES`, `app/app_arena.h`), and empties it right after every
app's `exit()`. An app takes bulk buffers from it and never frees them.

- Re-entry cannot fail to heap fragmentation: every visit gets the same block.
- Taking from it is no dynamic-memory call (MISRA 21.3); it bumps an offset.
- Use: [Building-an-App.md](Building-an-App.md#app-memory).
