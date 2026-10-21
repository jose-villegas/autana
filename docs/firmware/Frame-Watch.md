# The Frame Watch

## No allocating or logging in steady state

A frame that allocates, frees or logs every time it runs pays for it every
frame. A development build watches for that at runtime
(`profile/frame_watch.h`); release compiles none of it.

| | |
|---|---|
| The frame | From one `gfx_present_begin()` to the next: the shell presents once per pass, so a frame is the shell's pass, the app's `frame()` and `update()`, and the present. A pass that presents nothing (a frozen device, which still answers the console) joins the next frame. Work counts on the loop's task, the panel's sender and the core-1 job worker. |
| Watched | Every heap allocation and free, through ESP-IDF's heap hooks (`CONFIG_HEAP_USE_HOOKS`, dev and diag defaults), keyed by the caller's address. Every `ESP_LOG*` line, through `esp_log_set_vprintf()`, keyed by its format string. A plain `printf()` is not watched on the board. |
| Repeating | The same site in `FRAME_WATCH_REPEATS` of the last `FRAME_WATCH_WINDOW` frames (`profile/frame_watch.h`), a fraction of a second at this board's frame rate. Work done once when something happens, or a report every second or two, never qualifies. The `FRAME_WATCH_WARMUP` frames after an app is entered or left are counted but not judged. |
| Warning | One line per site, `FRAME_WATCH <alloc\|free\|console> in <n> of <window> frames at 0x<address>`, repeated at most once per `FRAME_WATCH_REPORT_INTERVAL_US` while it lasts. `console` is a log line, and its site also shows its format. `scripts/device/device.py` parses this line, so its shape is fixed by a test. |
| Counts | `autana framewatch`, and the `frame_watch` key of `autana screenshot`'s `.json`: the last frame's allocs, frees and log lines, and the sites repeating now. |

A finding shows up in `autana monitor`: live in a terminal, and in a piped or
scripted run as one of the few lines it echoes, with every heap site decoded
to a file and line under "frame watch sites decoded" against the matching
`.elf`. In a saved capture, `grep FRAME_WATCH` finds it. The fix is the
[Building-an-App.md](../Building-an-App.md#rules) rule: allocate in `enter()`,
free in `exit()`, and keep what a frame needs from one frame to the next. A
log line belongs to a change: log when a value changes, not every frame it
holds.

The gates are in [Performance and budgets](../testing/Performance-and-Budgets.md#the-frame-watch-as-a-gate):
every host render scene, and every on-device test that presents more than
`FRAME_WATCH_WARMUP` times.
