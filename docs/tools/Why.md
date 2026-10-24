# Why is it like this

`scripts/lookup/why.py` answers "why is this file, these lines or this name the way it is". It lists the pull requests behind it, each with its title, first paragraph and tickets.

It runs in any checkout, needs no board, and prints a short answer cut to a character budget.

```sh
python scripts/lookup/why.py launcher/main/render/r3d_span.c           # the file as it is now
python scripts/lookup/why.py launcher/main/render/r3d_span.c:100-140   # these lines
python scripts/lookup/why.py strbuf_appendf                            # every line that names it
python scripts/lookup/why.py launcher/main/boot                        # a folder
```

```
$ python scripts/lookup/why.py launcher/main/render/r3d_span.c:100-140 --budget 1100
launcher/main/render/r3d_span.c:100-140  7 PRs own these 41 lines
#1038 2026-10-20   3/41  render: opt-in camera small fill - flat small triangles up to 12 px with a colour gate and styles (-7.8% fitted-full)
    The selector (which triangles go flat) is separate from the style kernels (how they draw). That keeps a later distance or fog rule pluggable (autana-1a9r).
    autana-hlvg  closed (merged in #1038)  r3d: make the small-triangle dither pay off (impl suspect), push flat ceilings with dither, polish the ramps
#909 2026-10-15  13/41  render: carry span attributes along the left edge, one row loop per shading mode (ems.18)
    The r3d span rasterizer now carries each attribute (depth, and r, g, b for vertex colours) along the left edge, row to row, instead of rebuilding it per span from the triangle's origin (Hecker 1995).
    autana-ems.18  closed (PR 909 (green). QEMU one-core raster -7.2..-16.9% at all 17)  r3d: merge span setup into the row walk, one loop per shading mode
+5 more PRs and 7 older (--budget, --history or --json)
```

The sample shows the tickets as they stood when it was taken.

## Where each row comes from

| Step | Source |
|---|---|
| Lines now | the file, the range, or `git grep -w` of the name |
| Pull request per line | `git blame --first-parent`, which lands each line on the main-line commit: a squash `subject (#N)` or a `Merge pull request #N` |
| Order | pull requests that own a line now, newest merge first; for a name, the one that first added it joins them |
| Older | pull requests that changed it but own no line now, newest first: the file's history through renames, the range's (`git log -L`), or the name's (`git log -S`) |
| Title, first paragraph | `gh pr view`, cached per repository and pull request in the per-user autana cache, since a merged pull request does not change |
| Tickets | ids in the subject's last parenthesis and the description's `Ticket:` line, with status, close reason and title from `bd` |

A folder counts the lines each pull request changed in it instead, since blaming every file would be slow. Main-line commits without a pull request number show as `<sha> <subject>`. A path is read from the folder you run in, then from the checkout's root.

## Options

`--help` gives each default.

| Option | Does |
|---|---|
| `--rev REV` | answer for another revision |
| `--history N` | how many older pull requests to list on one line |
| `--budget CHARS` | answer size; the last line says how many rows were left out |
| `--json` | every row, uncut |
| `--offline` | no `gh`: descriptions only from the cache |
| `--eval` | ranking score over `scripts/lookup/why_eval.tsv` |
| `--usage [--days N]` | what this machine has asked |

Every lookup is logged locally with the [docs search](../cli/Docs-Search.md)'s log (`scripts/docs/query_log.py`). Nothing leaves the machine except the `gh` request for a pull request not yet cached.

## Evaluation

Each row of `why_eval.tsv` is a question, the pull request or ticket that answers it, and when it was asked. `--eval` answers each from git alone, on the main line as it stood then, and scores where the answer ranked. Each run is kept beside the docs search's evaluations.

| Rows | Question | Answer |
|---|---|---|
| past lookups | a file or name someone ran `git log` or `git blame` on | the pull request they went on to open, merged before they asked |
| closed tickets | a file the ticket names, which its merged pull request changed | the ticket |
