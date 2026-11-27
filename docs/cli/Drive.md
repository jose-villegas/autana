# Drive

Changes state the way a person holding the board could. `autana help drive`
lists the same commands.

<!-- generated: cli-drive check: python scripts/autana/cli_reference.py --check -->
| Command | What it does |
|---|---|
| `autana play <pack>/<name> [loop] [gain]` | play a pack sound; alias for sound play |
| `autana sound play <pack>/<name> [loop] [gain]` | play a sound from a pack; a bare name means the engine pack; gain is a percent, full gain when omitted; development builds |
| `autana sound stop [voice\|all]` | stop a voice or all voices; all when omitted |
| `autana sound stats` | voice states, dropped commands, amplifier state and RAM sizes |
| `autana sound onset <event> [hz] [gain]` | measure amplifier, start, stop and steal transients |
| `autana tap <x> <y>` | tap, 50 ms, at a point in pixels of the default screenshot |
| `autana press <x> <y> [ms]` | hold a point, as tap reads it; 1000 ms when omitted |
| `autana drag <x0> <y0> <x1> <y1> <ms>` | drag between two points, as tap reads them |
| `autana button <boot\|power> [short]` | press a board button; short when omitted |
| `autana button <boot\|power> long` | hold it |
| `autana reset [--capture [seconds]] [--verbose]` | reboot and wait for USB serial; --capture records the boot (20 s when omitted) |
| `autana apps` | the registered apps, and which is running |
| `autana apps --json` | the same, as JSON |
| `autana open <name>` | enter an app; a case-insensitive, unambiguous prefix |
| `autana home` | back to the launcher |
<!-- /generated: cli-drive -->

Coordinates are pixels of the default `autana screenshot`
([Watch](Watch.md)), whichever way the board is held; read a button's centre
off that image. Pixels from an `--as-shown` or `--framebuffer` capture are in
another frame and may miss. `sound onset` is explained in the
[clicks](../audio/Clicks.md#measuring-an-onset).

## Any console line

A line whose first word is not an autana command goes to the board as typed,
under the device lock, and autana prints the replies:

- A reply line starts with the first word in capitals.
- `<WORD>_END` ends the answer at once; `<WORD>_ERR` ends it and exits 1.
- A line nobody answers prints `sent` once the reply window has passed.
- `SET`, `GET` and `RESET` lines answer as `tune` does ([Live-Tuning.md](Live-Tuning.md)).

An app's own commands are sent this way, its prefix as the first word
([App-Commands.md](App-Commands.md)); each app lists its own.

A reply that does not start with the line's first word needs `device.py send`
with its own `--reply` and `--until`
([Device-Lock.md](Device-Lock.md#calling-devicepy-from-a-script)).

The development console also accepts `haptic <light|firm>` for the
[haptic pulse latency check](../haptics/README.md). It replies with
`HAPTIC <strength> audible_ms=<ms>` or an error.
