# Generated Files

Some C headers in this repository are written by a script rather than by
hand: icon tables, palettes, curves, baked layouts. Each one is checked in
beside the code that reads it, and opens with a banner saying so and giving
the command that writes it:

```c
/*
 * GENERATED FILE - do not edit.
 *
 *     python tools/gen/gen_ridge_curve.py ../design/boot/ridge.png main/ui/ridge_curve_generated.h
 */
```

`scripts/gates/check_generated_files.py` reruns every banner's command and
fails if the output is not byte for byte the file in the tree. CI runs it on
every pull request, in the Comment Rules workflow. Run it yourself before you
push a change to a generator or to one of its inputs:

```sh
python scripts/gates/check_generated_files.py
python scripts/gates/check_generated_files.py launcher/main/gfx/draw/icons_system.h
```

It prints `ok` or `FAIL` per file, and for a file that differs, the first
lines of the difference. To fix one, run the command in its banner and
commit the result.

The [generators' README](../../launcher/tools/gen/README.md) holds a table
of every generated file, its generator, the folder it runs in and its
command, written from the banners. `--check-table` fails when that table
is stale, running no banner; the table's marker names it, so the
generated-document gate runs it. Add `--write-table` to rewrite it whenever
a banner is added, changed or removed.

## What the gate reads from a banner

- **Which files are generated.** Every file git tracks with
  `GENERATED FILE - do not edit.` in its first five lines. There is no list
  to keep up to date: add the banner and the file is checked.
- **The command.** The first non-blank line after the marker, written the
  way you would type it. A script ending in `.py` runs under the Python
  that runs the gate, and one ending in `.sh` under `sh`, so the same banner
  works on Windows and Linux.
- **Where it runs.** From the nearest folder above the file that holds the
  command's script. Most banners are written from `launcher/`.
- **Where the output goes.** The command names the file itself, either as
  `> path` at the end (the gate reads standard output) or as an argument
  (the gate substitutes a path in a temporary folder). The tracked file is
  never written. A Windows console's CRLF line endings count as LF, as git
  stores them.

## When a file cannot be regenerated

The gate fails a banner whose command cannot run from the repository: an
input written as a `<placeholder>`, a script that is not there, or a
command that does not name the file it is in as its output. A file built
once from something the repository does not keep, such as a device
screenshot, is a committed fixture instead: its comment says where it came
from, and it carries no banner.

The formatter, the comment-length rule and the style audit skip every file
the banner marks, since the generator, not a person, decides their form.
