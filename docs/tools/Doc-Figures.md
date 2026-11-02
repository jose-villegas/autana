# Doc figures: producers and `autana figures`

The plots, GIFs and renders under `docs/images/` are made by producers:
scripts kept beside the code they explain. A producer states in its own
source which images it makes and how it is run. `autana figures` reads
those statements, so it can tell you who makes an image and rebuild just that
image, without a list kept anywhere else.

The maths figures are producers today. The other areas still run through
`launcher/tools/render/render_doc_images.sh`, which calls the runner for the
producers and makes the rest itself.

## Declaring a producer

One module-level literal, read from the source without running it:

```python
from docs.producer import Producer

PRODUCER = Producer(area="math", needs="host", run=("--cc", "{cc}", "--out", "{out}/math"),
                    outputs=("math/stats-percentile-ranks.png",))
```

| Field | Meaning |
|---|---|
| `area` | The docs area it serves, the unit of `--area` (`math`, ...). |
| `needs` | What must exist before it runs. `host`: a C compiler and Python with Pillow, numpy and matplotlib. |
| `outputs` | Paths under `docs/images/`. A glob (`math/fast-path-*.png`) where the code itself decides the names. |
| `run` | The script's arguments. `{cc}` becomes the host C compiler, `{out}` a folder laid out like `docs/images/`. |
| `reads` | Data files the script reads, as globs. Each must match a file before anything runs. |

The value must be a literal: no names, calls or computed strings. Two
producers declaring one image is an error.

A figure that plots a library's results gets its numbers from a C probe: a
small program that calls the library and prints JSON.
`launcher/tools/docs/figures.py` builds and runs one (`probe_into_folder`
covers the usual `--cc CC --out DIR` producer).

## Running them

| Command | What it does |
|---|---|
| `autana figures list [--area AREA]` | each producer: script, area, needs, image count |
| `autana figures which docs/images/math/frustum.png` | the producer that makes it, its arguments and the data it reads |
| `autana figures build --area math` | make that area's images and copy them into `docs/images/` |
| `autana figures check --area math` | make them and compare with the committed images |

`build` and `check` also take script paths in place of `--area`, or nothing for
every producer. A run goes in three steps:

| Step | What happens | On failure |
|---|---|---|
| Check | The C compiler, the Python modules and each `reads` file, for all chosen producers | every problem printed, nothing runs, exit 2 |
| Run | The producers in parallel (`--jobs N`, the core count by default), each into its own folder and log under `launcher/tools/results/doc_images/work/figures/` | the failed producer's log end, exit 2 |
| Compare | Each image against `docs/images/` by decoded pixels | `changed IMAGE: how` or `orphan IMAGE`, exit 1 |

A producer that leaves out a declared image, or writes one it does not
declare, fails the run. `check` reports a committed image that matches a
producer's glob but that the producer no longer makes as `orphan`.
