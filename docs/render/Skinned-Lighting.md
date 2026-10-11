# Skinned-Mesh Lighting

A skinned mesh bends at run time, so its light cannot be baked into its vertex
colours the way a static mesh's is. This page measures two ways to light it
every frame, on the host: direct N.L per vertex, and a per-object lookup table
indexed by the skinned normal.

```mermaid
flowchart LR
    Frame["once per frame, per object"] --> Build["light every table cell<br/>with every light and ambient"]
    Vertex["per vertex"] --> Skin["skin the normal<br/>blended joint rotations"]
    Skin --> Direct["direct: N.L per light"]
    Skin --> Oct["table: octahedral cell"]
    Build --> Oct
    Direct --> Out["light bytes times colour, RGB565"]
    Oct --> Out
```

| Variant | Per vertex |
|---|---|
| Reference | float bind normal, skinned, renormalised, N.L per light |
| Direct | skinned, not renormalised, N.L per light; bind normal as float or int8 per axis |
| Table, nearest | int8 bind normal, skinned, its octahedral point<sup>[[21]](../Citations.md#21)</sup> (`vec3f_octahedral`, any length) picks one cell |
| Table, bilinear | the same point blends the four nearest cell centres with 8-bit weights |

Each table cell holds the light of its centre's direction as three bytes, padded to
four. A cell's direction is fixed, so the directions are one table shared by
every object; the light bytes are rebuilt per object per frame. Every variant ends in
the same integer stage, so only the light bytes differ. Lights are
directional lights plus ambient in object space; each light-count row uses
the first lights of `LIGHTS` in the benchmark.

## Engine and device measurement

`render/skin/r3d_skin_light.h` owns the direct and bilinear kernels. Direct
lighting blends the palette rotations of the packed bind normal, then sums
ambient and positive N.L terms. The measurement flag renormalises that normal.
The table stores rounded, saturated linear light bytes and clamps lookup at
its border. Normal-indexed diffuse tables and bilinear filtering follow
Miller and Hoffman.<sup>[[58]](../Citations.md#58)</sup>

Cell directions are initialized with `vec3f_from_octahedral` into caller-owned
storage once, outside build timing. Each object owns its light bytes; the
shared directions require no engine allocation or mutable global storage.
The host benchmark uses these same rotation, direct-light, table-build and
bilinear kernels; its float reference and eight-light variants remain
benchmark comparisons.

The device suite mounts the demo pack through `asset_store`, opens its model
entries and samples `gallop` at `POSE_SECONDS` in the suite. Each test allocates
and releases internal RAM, reports heap free space and largest block, timing
statistics, cycles per vertex or build, byte-error maximum and mean, and CRC-32.
The skin-only baseline reports a position checksum. The float reference
normalizes the packed bind normal after independent weighted rotation; the
host reference uses the source glTF normal, so their error inputs differ.
Table-build checksums include the zero padding byte in each cell.
The host report writes `build/checksums.txt` beside the benchmark inputs;
its table CRCs can be compared directly at matching resolution and light count.
Vertex CRCs need matching mesh records and pose; the source glTF and packed
mesh have different records.

Run all device measurements on a diagnostics image with its demo assets:

```sh
autana --wait 3600 suite suite_skin_light_perf --flash
```

Select one measurement with `--test test_direct_1`, `test_direct_2`,
`test_direct_4`, `test_renormalised_1`, `test_renormalised_2`,
`test_renormalised_4`, `test_table_lookup`, `test_table_build_1`,
`test_table_build_2`, `test_table_build_4`, or `test_skin_only`.

## Results

The mesh is the hand-rigged capybara's glTF export, the cached bake
`python launcher/tools/bake/bake.py path capybara.glb` prints, passed to
`report_skin_light.sh`:
four influences per vertex, every frame of every clip. `mul`, `add`,
`div` and `sqrt` count float operations per vertex, read off each kernel in
[skin_light_bench.c](../../launcher/tools/r3d/skin_light/skin_light_bench.c),
as the board's proxy: host nanoseconds do not carry over to the S3's FPU, where
a divide or square root costs several instructions.

Per-vertex cost, the table's build excluded:

<!-- generated: skin-light-cost sha256=5bc7e2364e8d21cc79729111b7f40539119ae95c19fa8c96f28ea5cddebec6ca -->
613 vertices x 242 frames, median of 15 runs; host compiler 16.1.0.

| Variant | Lights | mul | add | div | sqrt | other | int mul | ns/vertex |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Skin only (shared) | any | 45 | 33 | 0 | 0 | 0 | 0 | 10.6 |
| Reference: float normal, renormalised | 1 | 57 | 40 | 1 | 1 | 7 | 0 | 23.1 |
| Reference: float normal, renormalised | 2 | 63 | 45 | 1 | 1 | 8 | 0 | 24.4 |
| Reference: float normal, renormalised | 4 | 75 | 55 | 1 | 1 | 10 | 0 | 28.2 |
| Reference: float normal, renormalised | 8 | 99 | 75 | 1 | 1 | 14 | 0 | 38.6 |
| Direct: float normal | 1 | 51 | 38 | 0 | 0 | 7 | 0 | 18.0 |
| Direct: float normal | 2 | 57 | 43 | 0 | 0 | 8 | 0 | 18.9 |
| Direct: float normal | 4 | 69 | 53 | 0 | 0 | 10 | 0 | 21.5 |
| Direct: float normal | 8 | 93 | 73 | 0 | 0 | 14 | 0 | 31.1 |
| Direct: int8 normal | 1 | 54 | 41 | 0 | 0 | 13 | 0 | 31.0 |
| Direct: int8 normal | 2 | 60 | 46 | 0 | 0 | 14 | 0 | 38.0 |
| Direct: int8 normal | 4 | 72 | 56 | 0 | 0 | 16 | 0 | 46.8 |
| Direct: int8 normal | 8 | 93 | 73 | 0 | 0 | 17 | 0 | 33.4 |
| Table 8x8 nearest: int8 normal | any | 49 | 39 | 1 | 0 | 13 | 0 | 23.1 |
| Table 16x16 nearest: int8 normal | any | 49 | 39 | 1 | 0 | 13 | 0 | 23.1 |
| Table 32x32 nearest: int8 normal | any | 49 | 39 | 1 | 0 | 13 | 0 | 23.0 |
| Table 8x8 bilinear: int8 normal | any | 51 | 41 | 1 | 0 | 21 | 18 | 38.9 |
| Table 16x16 bilinear: int8 normal | any | 51 | 41 | 1 | 0 | 21 | 18 | 38.4 |
| Table 32x32 bilinear: int8 normal | any | 51 | 41 | 1 | 0 | 21 | 18 | 38.3 |
<!-- /generated: skin-light-cost -->

The table's build, per object per frame, and what it adds per vertex at the
capybara's vertex count and at a larger mesh's (`LARGE_MESH`):

<!-- generated: skin-light-build sha256=4a27db89032abb2a45845fad4a5f4bba957f6cc66cb999ff31582efb4683063d -->
| Table | Cells | Bytes per object | Shared direction bytes | Lights | mul | add | div | sqrt | other | int mul | Build us/frame | Build ns/vertex, 613 vertices | Build ns/vertex, 1500 vertices |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 8x8 | 64 | 256 | 768 | 1 | 576 | 512 | 0 | 0 | 640 | 0 | 0.78 | 1.3 | 0.5 |
| 8x8 | 64 | 256 | 768 | 2 | 960 | 832 | 0 | 0 | 704 | 0 | 0.88 | 1.4 | 0.6 |
| 8x8 | 64 | 256 | 768 | 4 | 1728 | 1472 | 0 | 0 | 832 | 0 | 1.18 | 1.9 | 0.8 |
| 8x8 | 64 | 256 | 768 | 8 | 3072 | 2560 | 0 | 0 | 896 | 0 | 0.94 | 1.5 | 0.6 |
| 16x16 | 256 | 1024 | 3072 | 1 | 2304 | 2048 | 0 | 0 | 2560 | 0 | 2.98 | 4.9 | 2.0 |
| 16x16 | 256 | 1024 | 3072 | 2 | 3840 | 3328 | 0 | 0 | 2816 | 0 | 3.65 | 5.9 | 2.4 |
| 16x16 | 256 | 1024 | 3072 | 4 | 6912 | 5888 | 0 | 0 | 3328 | 0 | 5.18 | 8.5 | 3.5 |
| 16x16 | 256 | 1024 | 3072 | 8 | 12288 | 10240 | 0 | 0 | 3584 | 0 | 4.26 | 6.9 | 2.8 |
| 32x32 | 1024 | 4096 | 12288 | 1 | 9216 | 8192 | 0 | 0 | 10240 | 0 | 11.23 | 18.3 | 7.5 |
| 32x32 | 1024 | 4096 | 12288 | 2 | 15360 | 13312 | 0 | 0 | 11264 | 0 | 13.68 | 22.3 | 9.1 |
| 32x32 | 1024 | 4096 | 12288 | 4 | 27648 | 23552 | 0 | 0 | 13312 | 0 | 19.10 | 31.2 | 12.7 |
| 32x32 | 1024 | 4096 | 12288 | 8 | 49152 | 40960 | 0 | 0 | 14336 | 0 | 17.30 | 28.2 | 11.5 |
<!-- /generated: skin-light-build -->

Error against the reference over every vertex of every frame. Light byte is the
0-255 light of one channel; RGB565 step is one step of a 5- or 6-bit output
channel:

<!-- generated: skin-light-quality sha256=35b7fdbca2f282d1985104ac1033ec11e65c91a45ffff04af2c0a26ade01abb4 -->
| Variant | Lights | Light byte max | Light byte mean | RGB565 max step | RGB565 mean step | Vertices changed |
|---|---:|---:|---:|---:|---:|---:|
| Direct: float normal | 1 | 65 | 0.14 | 3 | 0.010 | 2.5% |
| Direct: float normal | 2 | 63 | 0.19 | 4 | 0.013 | 3.4% |
| Direct: float normal | 4 | 63 | 0.38 | 4 | 0.030 | 7.0% |
| Direct: float normal | 8 | 60 | 0.46 | 4 | 0.033 | 7.4% |
| Direct: int8 normal | 1 | 65 | 0.26 | 3 | 0.019 | 5.2% |
| Direct: int8 normal | 2 | 63 | 0.34 | 4 | 0.023 | 6.2% |
| Direct: int8 normal | 4 | 63 | 0.52 | 4 | 0.038 | 9.5% |
| Direct: int8 normal | 8 | 60 | 0.60 | 4 | 0.042 | 10.1% |
| Table 8x8 nearest: int8 normal | 1 | 88 | 6.87 | 10 | 0.540 | 41.1% |
| Table 8x8 nearest: int8 normal | 2 | 88 | 8.23 | 10 | 0.632 | 53.0% |
| Table 8x8 nearest: int8 normal | 4 | 68 | 7.50 | 8 | 0.566 | 58.0% |
| Table 8x8 nearest: int8 normal | 8 | 60 | 7.21 | 8 | 0.539 | 61.8% |
| Table 16x16 nearest: int8 normal | 1 | 47 | 3.50 | 6 | 0.271 | 35.8% |
| Table 16x16 nearest: int8 normal | 2 | 47 | 4.24 | 6 | 0.315 | 45.5% |
| Table 16x16 nearest: int8 normal | 4 | 40 | 3.96 | 5 | 0.278 | 44.5% |
| Table 16x16 nearest: int8 normal | 8 | 42 | 3.95 | 5 | 0.285 | 47.9% |
| Table 32x32 nearest: int8 normal | 1 | 23 | 1.72 | 3 | 0.137 | 25.8% |
| Table 32x32 nearest: int8 normal | 2 | 23 | 2.11 | 3 | 0.164 | 31.2% |
| Table 32x32 nearest: int8 normal | 4 | 20 | 2.02 | 3 | 0.148 | 31.6% |
| Table 32x32 nearest: int8 normal | 8 | 21 | 2.03 | 3 | 0.149 | 31.9% |
| Table 8x8 bilinear: int8 normal | 1 | 46 | 3.11 | 5 | 0.238 | 37.0% |
| Table 8x8 bilinear: int8 normal | 2 | 46 | 3.74 | 5 | 0.282 | 45.3% |
| Table 8x8 bilinear: int8 normal | 4 | 42 | 3.31 | 5 | 0.254 | 46.5% |
| Table 8x8 bilinear: int8 normal | 8 | 42 | 3.44 | 5 | 0.261 | 47.3% |
| Table 16x16 bilinear: int8 normal | 1 | 22 | 1.08 | 3 | 0.082 | 18.0% |
| Table 16x16 bilinear: int8 normal | 2 | 22 | 1.35 | 3 | 0.101 | 21.8% |
| Table 16x16 bilinear: int8 normal | 4 | 22 | 1.39 | 3 | 0.105 | 24.4% |
| Table 16x16 bilinear: int8 normal | 8 | 22 | 1.41 | 3 | 0.108 | 25.7% |
| Table 32x32 bilinear: int8 normal | 1 | 12 | 0.34 | 2 | 0.027 | 7.0% |
| Table 32x32 bilinear: int8 normal | 2 | 12 | 0.46 | 2 | 0.035 | 8.6% |
| Table 32x32 bilinear: int8 normal | 4 | 11 | 0.51 | 1 | 0.039 | 10.5% |
| Table 32x32 bilinear: int8 normal | 8 | 11 | 0.52 | 2 | 0.039 | 10.4% |
<!-- /generated: skin-light-quality -->

One gallop frame under the sheet's light count (`SHEET_LIGHTS`); the error
tiles compare the RGB565 output
with the reference's:

![The capybara lit by each variant, with its error against the reference](images/skin-light-sheet.png)

## Recommendation

The host and the float-operation counts disagree, and the board decides
between them:

- **On the host, direct N.L is faster than every bilinear table at every light
  count measured**; only nearest lookup overtakes it as lights are added, at
  the faceted error the sheet shows. Its mean error is lower than any 16x16
  table's. Only its maximum error is worse: linear blending shortens the
  normal where joints of different rotation meet and darkens those vertices
  (the hip in the sheet), and renormalising costs a square root and a divide
  per vertex.
- **In float operations the table wins from four lights up**, because its
  lookup does not grow with the light count and its build is spread over
  the mesh's vertices; what it adds instead, a divide and the bilinear
  blend's integer multiplies, is what only the board can price.

If the board confirms the float counts, build **a 16x16 table, bilinear**:
nearest lookup at 16x16 shows faceted error in the sheet, 32x32 nearest only
comes near 16x16 bilinear's error at four times the build and the memory,
and 32x32 bilinear's build costs more per vertex than the lighting it
replaces at the capybara's vertex count. The table path never renormalises,
because the octahedral map ignores length. Otherwise light directly, and
renormalise if the darkened joints show.

Either way, **int8 bind normals**: they leave the maximum RGB565 error
unchanged and add a fraction of one step on average.

## Reproduce

From the repository root, with the committed glTF export that
`report_skin_light.sh` names:

```sh
launcher/tools/r3d/skin_light/report_skin_light.sh ASSET.glb gallop
```

It builds and runs the benchmark, redraws the sheet and rewrites the three
tables above.
