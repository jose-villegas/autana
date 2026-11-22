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
| `autana video -o PATH [--fps N] [--seconds S]` | live panel windows to an H.264 MP4 plus frames.csv; stdin lines go to the board; development builds |
| `autana record [-o PATH] [--seconds S]` | each frame's render input as records (FRAMEREC), to PATH or stdout; console lines from stdin go to the board, so FREEZE, STEP and RESUME pause and step it, and it stops when stdin ends; with --seconds it reads no stdin and stops after S seconds; development builds |
| `autana battery [--json]` | fresh voltage, percentage, USB, charge phase and flow, plus raw PMU status bytes; development builds |
| `autana battery --log [--json]` | persistent battery records oldest first (JSON is an array); reasons: periodic, transition, boot, shutdown-warning; development builds |
| `autana battery --log --clear` | erase the battery history namespace |
| `autana heap [psram\|app] [--json]` | free bytes, largest block, total and low-water mark of the internal DMA pool (MEMORY_DMA), of PSRAM, or of the running app's heap; development builds |
| `autana heap reset` | restart the low-water marks, so the next reading's min covers only what ran since |
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

## Panel video

```sh
autana video -o run.mp4 --fps 5 --seconds 10
autana video -o run.mp4 --fps 5
printf 'DRAG 20 40 200 240 500\n' | autana video -o drag.mp4 --fps 5
```

The command holds the device lock and port through `PANELCAP_END`. Board
text goes to stderr. EOF or Ctrl+C stops capture. With `--seconds` the
command stops on its timer and leaves stdin unread.

| Recording rule | Effect |
|---|---|
| App memory | Recording uses `PANELCAP_CAPTURE_BYTES` (about 334 KB) of free app memory. A scene that needs those bytes cannot be recorded. |
| Short on memory | START prints `PANELCAP ERR no memory: need N, largest free M` in bytes and exits non-zero. |
| App switch | Capture reports `PANELCAP STOP app switch`, drains its last published present, and ends. |

`--fps` limits device publication. The landscape H.264 MP4 samples the
latest present at each 30 Hz device timestamp. Gaps repeat the last
picture, and bursts contribute the latest eligible picture.

`run.frames.csv` records each complete present beside the MP4.

| Column | Meaning |
|---|---|
| `present_seq` | Device sequence, including presents that merged or were lost. |
| `device_us` | Device time at present start, in microseconds. |
| `host_ns` | Host wall-clock time at reassembly, in nanoseconds. |
| `windows` | Dirty rectangles in the present. |
| `packed_bytes` | Bytes of PackBits pixel data. |
| `dropped_before` | Presents merged or lost while busy since the previous publication, saturating at 65535. |

The summary reports

- `presents` and `fps` for complete presents received and their attained rate.
- `device_dropped` for the whole run's count of merged or busy presents.
- `host_dropped` for presents rejected after missing, repeated, out-of-order or damaged parts or invalid data.
- `MB/s` for packed pixel throughput.

See [panel video framing](../tools/Panel-Video.md) and the [FFmpeg pin](../tools/Framework-Versions.md#ffmpeg).
