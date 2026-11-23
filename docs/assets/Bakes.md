# Bakes

Part of the [asset docs](README.md). Meshes and sounds are bake products: keyed, locked and fetched, never committed.

How the meshes are cached
([Cached-Bakes-Design-Sketch.md](../plans/Cached-Bakes-Design-Sketch.md)):
`launcher/tools/bake/bake.py` keys each one on its recipe, its sources and
the code that bakes it; `launcher/bakes.lock`, written only by that tool,
records the bytes made for each key, and main publishes those files to the
repository's `bakes` release. `build_pack.py`, and so the firmware build, the
host tests and the render scripts, takes every mesh from the bake cache by
its lock row: the user cache, one per user and shared by every clone and
worktree, or the folder `AUTANA_BAKE_CACHE` names for one process. What the
cache lacks is downloaded from the release, or before main publishes it from
the CI run that made it (that needs a GitHub token: `GH_TOKEN`, or `gh`
signed in). **A build with a cold cache needs the network once**; offline, it
fails naming every mesh it cannot get. Files land in the cache by rename, so
builds running side by side never read half a file.

A mesh, a fit's reference set, its fit and its levels count only the functions, classes and constants their
entry functions reach (`bake/code_units.py`), so an edit to a helper a stage never runs keeps its bake. The
other stages count everything their entry scripts import.

| Change to the tools | Re-keys |
|---|---|
| A helper the stage never reaches | nothing |
| A reached line edited, renamed or moved | that stage, and the fit when it is the reference set (the fit names its key) |
| A start's or fit's code, with the same bytes out | only that bake: the fit and levels keys name the bytes they read |

A branch that changes what a mesh is made from gets its bake from the
Bakes workflow, which every push to a branch of this repository starts: its CPU job bakes the meshes the lock lacks and uploads them,
and the lock check fails until the author runs `bake.py lock --from-run N`
with that run and commits the lock. A branch keeps one Bakes run: a push cancels
the run of the head before it, so wait for the run to finish before `lock --from-run`,
which refuses one still going.
A fit needs the CUDA GPU, so the Bakes GPU workflow makes it, started by hand on the branch; no pull request reaches that
runner. `--from-run` repeats, so the two runs lock together
(`--from-run CPU --from-run GPU`); two runs that made one key with different
bytes fail, naming both. Only a run's
files can be locked, so every locked file can be published from main. The exception is a seeded
row (`seeded = true`), written by `bake.py lock --seed` from the files the tree
held: its bytes were carried over, not made by the code its key names, and
`check` lists such rows until a run makes their keys again. A seed skips a
levels bake whose fit it has not locked yet; the CPU run makes it afterwards. A row a run wrote
names its `host`, the system and machine that made the bytes, since a bake
repeats byte for byte only on one kind of host (a Linux and a Windows Blender
export of the same `.blend` differ in the last bit of some animation keys).
A bake that traces rays differs between CPU models too (an AMD and an Intel host
trace a few rays an ulp apart), so every Bakes job that makes a bake runs on the
self-hosted runner that makes the fits (see [The bakes runner](#the-bakes-runner)).
A row records the CPU vendor it was made on, and a rerun adopts an earlier attempt's bake only from a matching one.

A fit's reference set is traced on the CPU, as a mesh is, since the GPU's ray
tracing may build a different tree each time
([Reference on the CPU](../../launcher/tools/r3d/README.md#reference-on-the-cpu)).
It is a bake of its own, locked like the start; its bytes differ between CPU models, even two generations of one vendor.

The Bakes plan job selects each missing set's poses once and uploads their files for that attempt.
Every reference shard renders that plan and copies its pose files byte for byte; the join job packs their union.

A fit reads only locked bytes, so it repeats exactly on any GPU model. Its row
names the `gpu` that made it. The Bakes GPU run's `runner` input picks the
self-hosted runner by its label (default `gpu-legion`); with `again`, the run fails
when a fit makes other bytes than its lock row, which checks a fit on a GPU
other than the one that made it.

A run keeps each bake it finishes: the bake goes to the cache and the upload as
it is made, and the upload runs even when the job fails, so a failed run locks too.

| A later run on | Makes only what is left, taking the rest from |
|---|---|
| the GPU runner | its cache, which persists between runs |
| a rerun attempt | the earlier attempts' uploads made on a host like its own (`bake --from-run`) |

Each attempt uploads its own artifact; a key two attempts made locks with the later attempt's bytes.

Every mesh bake writes a `.bundle`: its LMSH entries, LCOL/LLOD siblings
and copies manifest, including when no shapes are carved.

| Fitted renderer stage | Output | Input |
|---|---|---|
| CPU start | `<entry>.start.bundle` | Source and import recipe |
| CPU reference set | `<entry>.reference.tar.xz` | Source, scene light and camera, pose size and spacing |
| GPU fit | `<entry>.fit.bundle` | Locked start and reference set bytes; rest without levels |
| CPU levels | `<entry>.bundle` | Locked fit bytes, `lod` and meshlet size |

The fit trains only the rest. Both later stages pass copy meshes, colour
sets and the [MCOP manifest](formats/MCOP.md) through unchanged. Only the final
bundle enters the scene pack.

The lock's `[[poses]]` rows hold the digest and count of each camera-path sampling.

| Who | Does |
|---|---|
| a build whose poses rows are all locked | reads them; never builds or runs `track_host`, so needs no host C compiler |
| a key whose row is missing | samples through `track_host` (a host C compiler), or leaves the bake unlocked, naming `bake.py lock` |
| `bake.py lock` | samples every needed row again and writes it with its `host`, dropping the rest; the only writer |
| `bake.py check` | samples every needed row again; a missing, unneeded or differing row fails, exactly |

SH incident-radiance grids are a CPU bake kind, `sh_probes`, requested by a
scene's sibling `<scene>.probes.toml`. They produce one `.shpr` payload per scene,
keyed independently of mesh and reference bakes. See
[SH probe baking](../render/offline/SH-Probes.md) for sampling and commands.

## The bakes runner

One self-hosted runner, labelled `gpu-legion`, makes every bake: the Bakes jobs
that make one (meshes, exports, sounds, levels, the reference plan and shards)
and the Bakes GPU fits. It is one instance, so a CPU bake and a fit queue behind
each other. A fit runs in its fit environment, `~/gpu/env`
([r3d README](../../launcher/tools/r3d/README.md)); a CPU job makes a venv of
its own over it, so what it installs stays in the venv; only a Bakes GPU run
installs into the fit environment. Both run on that environment's Python, the
release the r3d README creates it with, not the `.python-version` one.
Neither needs root: the machine holds LLVM, Git LFS, a C compiler and the X
libraries Blender loads.
Tracing runs as a stage of [`local_schedule.py`](../../scripts/lib/local_schedule.py),
on the half of the CPUs board work leaves, under the stage's hang guard.

No pull request starts the Bakes workflow, since a self-hosted runner never
takes a pull request's code ([`check_workflows.py`](../../scripts/gates/check_workflows.py)).
A pull request's bake checks are those of the push run on its head commit. A
fork's pull request has none until a maintainer pushes its branch here; until
then the hosted [`bakes-fork.yml`](../../.github/workflows/bakes-fork.yml)
fails it when it needs a bake. While the machine is off, its jobs wait in GitHub's queue,
which fails a job after a day; nothing falls back to a hosted runner, whose bytes would differ.
