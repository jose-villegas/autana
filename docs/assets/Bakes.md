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

A pull request that changes what a mesh is made from gets its bake from the
Bakes workflow: its CPU job bakes the meshes the lock lacks and uploads them,
and the lock check fails until the author runs `bake.py lock --from-run N`
with that run and commits the lock. A fit needs the CUDA GPU, so the Bakes GPU
workflow makes it, started by hand on the branch; no pull request reaches that
runner. `--from-run` repeats, so the two runs lock together
(`--from-run CPU --from-run GPU`); two runs that made one key with different
bytes fail, naming both. Only a run's
files can be locked, so every locked file can be published from main; a fit's
reference frames stay on the machine that fitted. The exception is a seeded
row (`seeded = true`), written by `bake.py lock --seed` from the files the tree
held: its bytes were carried over, not made by the code its key names, and
`check` lists such rows until a run makes their keys again. A row a run wrote
names its `host`, the system and machine that made the bytes, since a bake
repeats byte for byte only on one kind of host (a Linux and a Windows Blender
export of the same `.blend` differ in the last bit of some animation keys).
A mesh bake differs by CPU vendor too (an AMD and an Intel host trace a few
rays an ulp apart), so the lock's `mesh_cpu_vendor` line names the vendor its
meshes were made on. A run records its host's vendor; `lock --from-run` refuses
a run that made a mesh on another vendor, naming both, and the CPU job fails
before baking when the hosted runner it got is another vendor's. Either way,
re-run the failed job until it lands on the lock's vendor.

A fit repeats byte for byte only on one GPU model, so a fit's row also names
its `gpu`. The Bakes GPU run's `runner` input picks the self-hosted runner by
its label (default `gpu`). One run fits on one runner, so its fits share one GPU.

A renderer whose import carves shared meshes has a `.bundle` row; an uncarved
one has a `.mesh` row. A carved fit's CPU start is `.start.bundle`. The GPU fit
trains only the rest, copying shapes, LCOL and the
[MCOP manifest](formats/MCOP.md) unchanged; its key names the
locked start bundle's bytes, and its references use the full source.

The lock's `[[poses]]` rows hold the digest and count of each camera-path sampling.

| Who | Does |
|---|---|
| a build whose poses rows are all locked | reads them; never builds or runs `track_host`, so needs no host C compiler |
| a key whose row is missing | samples through `track_host` (a host C compiler), or leaves the bake unlocked, naming `bake.py lock` |
| `bake.py lock` | samples every needed row again and writes it with its `host`, dropping the rest; the only writer |
| `bake.py check` | samples every needed row again; a missing, unneeded or differing row fails, exactly |
