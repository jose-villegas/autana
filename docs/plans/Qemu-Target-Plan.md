# A QEMU target for autana

Not built yet.

Every `autana` verb is a line of text over the firmware console, and QEMU already
carries that console on a TCP socket. So an emulated target is a transport swap,
not a second protocol: the same `tap`, `drag`, `open`, `tune`, `screenshot` and
`suite` reach an image running under emulation, with no flash cycle, no device
lock, and as many instances at once as a machine will hold.

What it does not give is a screen. QEMU models no AMOLED, which is why the QEMU
build already uses the null panel and why injected touch existed there first.

## What it is for

| | Board | QEMU |
|---|---|---|
| Flash cycle | esptool write, reset, re-enumerate | none |
| Concurrency | one board, everything queues | one instance per run |
| Panel | the real one | none: frames come back as stills |
| Timing | real | meaningless |

Logic, layout, navigation, a suite that asserts behaviour: emulated. Anything
that measures - frame budget, present cost, PSRAM bandwidth, touch latency -
stays on the board, and the CLI should say so rather than quietly reporting a
number nobody should trust.

## Shape

- `autana qemu start [variant]` builds the QEMU image if needed, starts one
  instance, prints its handle. `autana qemu stop [handle]`, `autana qemu list`.
  Whatever starts an instance owns killing it: an orphan held 1.1 GB for hours
  after the run that spawned it had finished, so every instance carries the pid
  of its owner and `stop` reaps the ones whose owner is gone.
- `AUTANA_TARGET=qemu[:handle]`, or `--qemu`, points the ordinary verbs at an
  instance. Everything else about them is unchanged.
- `scripts/device/device.py` grows one seam: the console is either a serial port
  or a socket. `launcher/test/qemu_run.py` already opens that socket and matches
  lines on it, so the reader belongs there, shared rather than copied.
- The device lock arbitrates the board. An instance is private to its starter,
  so it takes no lock - but two instances must not share a port, and a stale
  instance must not be adopted by accident.

## What has to be decided when it is built

- **Whether a QEMU run may report a suite as passed.** A portable suite proves
  the same thing under emulation that it proves on a host, and less than it
  proves on the board: the point of the device run is that Xtensa behaves like
  x86. Emulated Xtensa is a third answer, and the report must name which one it
  is.
- **What a screenshot means with no panel.** The framebuffer is real; the panel
  is not. A capture is what the app drew, not what a screen would show - no
  panel timing, no orientation quirk, none of the faults a screenshot is usually
  reached for.
- **Whether the image is a variant or a build.** It is `sdkconfig.defaults.qemu`
  today, so it is its own build directory: a first build costs what any variant
  costs, and nothing after that. Worth stating in the CLI's own help, because
  "no flash" reads as "no wait" and the first run is not free.

## What it does not replace

The render harness draws a real firmware screen on the host at native speed,
with no emulation at all - the better answer when the question is what a screen
looks like. QEMU's answer is what the firmware does.
