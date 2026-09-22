# The host render harness: real drawing code, real pixels, no board

Rendering a firmware screen into an image on a laptop: how a scene is
declared, what its pixels are pinned to, the same scenes under QEMU, and
diffing any of them against a device capture.
[`../Testing-Guide.md`](../Testing-Guide.md) is the host/device test split
this sits inside.

---

The firmware's drawing code compiles on a host, so a screen can be rendered
into an image without a flash cycle. A **scene** is one thing to render: it
names the translation units it needs, the quarter turn, how many frames to
draw, and what synthetic touch to feed them.

```sh
./launcher/tools/render_all_scenes.sh          # every scene, and the standing check
./launcher/tools/post_ui_render_host.sh        # one scene, into its own results/render/
./launcher/tools/launcher_home_render_host.sh -o /tmp/home
./launcher/main/apps/render_lab/tools/render_lab_render_host.sh
```

Each writes a 24bpp BMP per declared render, plus a PNG beside it when
Pillow happens to be installed. Output lands in `results/render/<scene>/`
under whichever `tools/` folder owns the scene, which is gitignored.

**This is for correctness and code shape, never for cost.** Host wall-clock
is not a perf oracle: an x86 laptop's timings say nothing about what the
work costs on the chip, and even instruction counts only answer whether
work was removed (see
[`../notes/Optimization-Playbook.md`](../notes/Optimization-Playbook.md)).
Time a change on the device, or under QEMU's `--icount` for counts.

## Declaring a scene

Two files, the same declare-then-source shape a report script uses:

- **`<name>_render_host.c`** defines one `render_scene` - see
  `launcher/tools/render_host.h` for the fields: a setup hook that runs
  once after `gfx_init()`, a draw hook that runs once per frame, and an
  options hook taking whatever arguments the harness did not recognise.
- **`<name>_render_host.sh`** declares `scene_name`, `scene_sources` and
  `scene_renders`, then sources `launcher/tools/render_scene.sh` and calls
  `render_scene_run "$@"`. Everything else - finding a compiler, building,
  checking each image, converting to PNG - is that one procedure.

An engine scene lives in `launcher/tools/`; an app's scene lives in that
app's own `tools/`, so nothing in the engine's tooling names an app.
`render_all_scenes.sh` finds both by name, so a new scene is one pair of
files and deleting an app deletes its scenes.

Each line of `scene_renders` is `<label>|<arguments>|<width>x<height>`, and
the declared size is checked against what the binary reports it wrote. That
is what makes the sweep a check rather than a picture nobody looks at
twice.

## Frames, input and orientation

`.frames` is at least two for anything built through microui: a window is
clipped to the rect it had on the previous frame, and `ui_pointer.c`
synthesizes hover frames before a press can land, so a settled screen is
never the first one. Touch is declared as `render_input_step_t` entries in
**panel** coordinates - where a finger lands, not where the rotated canvas
puts it - each holding until the next, so the harness derives the
pressed/released edges rather than the scene restating them.

An image comes out the way the board is READ at that quarter (448x368 for
a landscape one) unless `--panel` asks for the framebuffer the way the
panel holds it, 368x448. That second shape is what a device capture has.

A scene that leaves gfx in band mode is refused rather than rendered: the
band ring retains no frame to read back, the same reason a device capture
refuses one.

## What each scene's pixels are pinned to

An image of the right size can still be the wrong picture, so every render's
content hash is pinned in a `<scene>_render_baseline.txt` beside the scene
that owns it, and `render_all_scenes.sh` fails on a change to the pixels.
Every render is bit-identical from run to run, which is what makes this
worth pinning at all.

Hashes rather than committed images: this repository commits a binary only
as a generator's input, and a rendered frame is neither that nor something
anyone reads a diff of. A render with no pin yet says `(not pinned)` and
passes, so a new scene is not blocked on one.

**A pin only holds where the pixels are integer-exact**, since CI renders on
a different compiler and C library than anyone's desk. The self-test report,
the home screen and the boot animation are pinned: `gfx.c` does no float
maths, and the scroll view's momentum - the one part of the UI that reaches
the maths library - is switched off at a zero time constant, so it is
linked but never called. Render Lab declares `scene_pin=0` and is checked for its
declared size alone: it draws a frame counter that is a `double` printed
with `"%.1f"`, and the only reason that reads zero is a run stopping 20 ms
short of the window that computes it. A scene whose pin can fail for a
reason nobody changed teaches the reader to ignore the pin.

Every scene is linked against the maths library regardless, last on the
line: the Windows toolchains fold those functions into libc, so a scene that
needs one links clean on a laptop and fails only on Linux.

```sh
./launcher/tools/render_all_scenes.sh --update-baseline   # re-pin, deliberately
```

Re-pin only after looking at the images and agreeing the pixels should have
changed. The failure names the file to look at and the command to run.

## Video

A BMP is one frame. `--video PATH` (`render_video.h`/`.c`) appends every
drawn frame instead, into an uncompressed RIFF AVI - so motion, a
transition, or a scene's settle time can be judged without a flash cycle,
the same reason the rest of this harness exists. The frame rate is
`1000 / --dt`, exact as a rational, not rounded. `-o` keeps working
unchanged alongside `--video`, or on its own.

```sh
./launcher/tools/boot_anim_render_host.sh --video   # every scene's script takes this,
                                                     # writing <label>.avi beside <label>.bmp
python launcher/tools/check_avi.py out.avi ...      # re-reads the header and index and
                                                     # checks frame count, size and rate agree
```

Players stop reading a RIFF AVI 1.0 file well short of its 32-bit size
field's 4 GB, so a run whose video would pass 1 GB is refused before
anything is drawn, with the frame count
that does fit stated in the refusal. `--video` never changes what a scene's
BMP pins: the same bytes are written whether or not it is given, so it adds
no baseline of its own.

A scene animates a `--video` run the way it animates any multi-frame
render: `boot_anim_render_host.c`'s `<now_ms>` is where the first frame
starts, and each later frame adds that frame's own `elapsed_ms` to it, the
harness's usual per-frame schedule. Like every other render this harness
writes, a video's frames are drawn from a scene's own fixture data - never a
reading from any board.

## The second backend: the real image under QEMU

The same scenes, the real Xtensa binary. `test/run_qemu_tests.sh` builds an
image that boots into the shell rather than running its suites, and its
console answers `screenshot` with the frame - the board tool's own protocol,
over a socket instead of USB.

```sh
./launcher/tools/render_qemu.sh -o /tmp/q \
    --row "<first row>" --row "<second row>"     # capture, then diff
./launcher/test/run_qemu_tests.sh --touch down,128,224 --touch up,128,224 \
    --screenshot /tmp/after_tap.png              # tap a row, capture the app
```

A capture takes about a minute and a half after the build, needs no board
and no lock, and any number of runs go at once. `render_qemu.sh` reads the
quarter from the capture's own sidecar, renders the home screen on the host
at that quarter, and diffs the two with the shell's chrome masked. The rows
have to be stated: only the image knows what registered itself, and its
console reports how many, never which.

**A touch goes in as a level, not an event.** `TOUCH <down|up> <x> <y>` on
the console reaches `touch_inject()`, and the polling task samples it at its
own rate - so a sample has to be left in place long enough to be seen, and
there is no acknowledgement to wait for. That is enough to open an app and
photograph it, which is the only way to see a screen whose app cannot be
linked on a host at all.

**Two limits worth knowing before comparing anything.**

*`dt` is real elapsed time.* The image runs its own frame loop against a
clock, so a QEMU run cannot reproduce a scene's declared frame schedule.
Only a screen that has SETTLED - one whose picture does not depend on how
many frames it took to get there - compares pixel-exact with a host render.
The home screen is such a screen. A scene stepped for its animation, a
Render Lab scene at a fixed step count, is not: the same step count does not mean the
same accumulated time.

*Orientation comes from the injected IMU.* Until an `IMU` line
(`qemu_run.py --do "tilt ..."`) says otherwise, the stand-in reads upright
and still; set the pose before comparing at a given quarter.

## Diffing against a capture

```sh
autana screenshot -o shot.png                                # --dev build only
./launcher/tools/post_ui_render_host.sh -o /tmp/post
./launcher/tools/render_diff.sh shot.png /tmp/post/landscape-panel.bmp \
    --mask build_mark --mask home_hint --out /tmp/diff.png
```

It reports the first differing pixel, how many differ, and writes an image
with the differences in red and the masked regions in blue. Exit status is
0 only when nothing differs. Either side may be a host render, a QEMU
capture or a board capture.

**Orientation is declared, never guessed.** A capture is always
panel-native whatever the shell was rotated to; its sidecar's
`orientation_quarter` says which rotation that was and is reported, not
applied. A render in the read orientation must say `--quarter-a` /
`--quarter-b` or it is refused rather than turned on a guess.

**Masks cover what the shell draws and a scene does not** - the development
build's corner mark, the swipe-home strip. They are declared per quarter in
`launcher/tools/render_masks.json` and named on the command line. If that
chrome moves, that file has to move with it.

---

## Related

- [`../Testing-Guide.md`](../Testing-Guide.md) - the host and device test
  runners, runsuite, and the QEMU suite run this harness's second backend
  borrows its image from.
- [`../Building-a-Screen.md`](../Building-a-Screen.md) - building the screen
  a scene renders.
- [`../notes/Optimization-Playbook.md`](../notes/Optimization-Playbook.md) -
  why a host timing is not a cost.
