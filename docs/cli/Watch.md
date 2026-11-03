# Watch the board

Only reads the board, so running a command twice changes nothing. `autana help
watch` lists the same commands.

<!-- generated: cli-watch check: python scripts/autana/cli_reference.py --check -->
| Command | What it does |
|---|---|
| `autana monitor [seconds] [--follow] [--stream] [--elf PATH]` | in a terminal the console live, until Ctrl+C or for seconds; piped it needs seconds or --follow and prints only errors and FRAME_WATCH warnings unless --stream; exits 1 on a crash or reboot |
| `autana coredump [--elf PATH]` | the last panic's core dump, saved to the records and decoded against the build that crashed; resets the board |
| `autana coredump --core FILE [--elf PATH]` | decode a saved dump; no board |
| `autana coredump --erase` | clear the dump so the next crash's is told apart; resets the board |
| `autana mic record <ms> [--settle ms] [--at offset reg value \| --at offset tone hz duration_ms gain]... -o take.wav [--json]` | record microphone PCM and codec state; --at offsets are ms from take start; negative offsets fall in settling |
| `autana codec <reg> [value]` | development builds: read, or write then read back, one codec register (hex or decimal) |
| `autana screenshot [--as-shown\|--framebuffer] [-o PATH]` | PATH.png plus a PATH.json state snapshot; landscape by default |
| `autana screenshot --frames N -o PATH` | N frames as PATH-00 on: the loop is frozen and stepped a frame between captures, then resumed |
| `autana battery [--json]` | fresh voltage, percentage, USB, charge phase and flow, plus raw PMU status bytes; development builds |
| `autana battery --log [--json]` | persistent battery records oldest first (JSON is an array); reasons: periodic, transition, boot, shutdown-warning; development builds |
| `autana battery --log --clear` | erase the battery history namespace |
| `autana heap [--json]` | free bytes and largest block in the internal DMA pool (MEMORY_DMA); development builds |
| `autana perf` | the frame_cost names seen and the counter events |
| `autana perf <name> [event] [seconds]` | count one event (insn when omitted) on one frame_cost bracket for seconds (10 when omitted); prints cycles avg/min/max, n and cycles per event |
| `autana perf off` | disarm the counters |
| `autana framewatch` | the frame watch as JSON: allocations, frees and log lines repeating frame after frame |
<!-- /generated: cli-watch -->

Where a coredump lands and how to read it:
[Debugging](../notes/Debugging.md#after-a-crash-the-reset-reason-and-the-core-dump).
`framewatch` is explained in
[the frame watch](../firmware/Frame-Watch.md#no-allocating-or-logging-in-steady-state)
and `perf` in [Frame-Cost.md](../tools/Frame-Cost.md#hardware-counters).
