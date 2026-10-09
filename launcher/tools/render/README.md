# Render

| File | Purpose |
|---|---|
| [render_host.c](render_host.c) | Runs a firmware drawing scene on the host and writes BMP frames. |
| [render_host.h](render_host.h) | Scene interface for host rendering. |
| [render_scene.sh](render_scene.sh) | Compiles, runs, and checks one host render scene. |
| [render_all_scenes.sh](render_all_scenes.sh) | Discovers and checks all host render scenes. |
| [scene_viewer.sh](scene_viewer.sh) | Builds and renders a scene file, selecting renderers with `--object NAME`; `--build-only -o DIR` returns the host path. |
| [doc_images_demo.sh](doc_images_demo.sh) | Makes the demo's bake, fidelity and import images and measured CPU tables through the scene viewer. |
| [doc_tables.py](doc_tables.py) | Writes the CPU tables from comparison logs, with scene and object names supplied as arguments. |
| [doc_import_examples.py](doc_import_examples.py) | Renders albedo and face-sampling examples from a supplied scene and renderer. |
| [physical_scene.py](physical_scene.py) | Copies a scene without its indirect look and occlusion settings for isolated bake studies. |
| [render_diff.py](render_diff.py) | Compares rendered and captured images. |
| [render_diff.sh](render_diff.sh) | Shell entry point for image comparisons. |
| [render_compare.py](render_compare.py) | Builds the A, B and difference sheet, the side-by-side video, the zoomed crops and the change numbers; every panel is labelled by `--label-a` and `--label-b`, which stills require. |
| [render_compare.sh](render_compare.sh) | Runs a scene at two revisions and compares the images: `--script S [--clear RRGGBB] [--video [--fps N]] [--crops N] A B [--render LABEL "ARGS"]`; `--reference SCENE --poses FILE` scores one render's whole path against the source model. |
| [render_png.py](render_png.py) | Converts host BMP frames to PNG. |
| [render_qemu.sh](render_qemu.sh) | Captures a QEMU screen and compares it with a host render. |
| [render_video.c](render_video.c) | Writes video frames from host render scenes. |
| [render_video.h](render_video.h) | Video writer declarations. |
| [render_watch.c](render_watch.c) | Fails a scene whose frames keep allocating or printing. |
| [render_watch.h](render_watch.h) | Frame watch declarations for the host renderer. |
| [render_masks.json](render_masks.json) | Named masks for image comparisons. |
| [code_layout.py](code_layout.py) | Reports, writes, and checks each `RENDER_ENTRY_OFFSET` function and its machine loops within an instruction-cache line. |
| [check_avi.py](check_avi.py) | Validates AVI structure and frame metadata. |
| [scenes/](scenes/) | Engine render scenes and their pinned baselines. |
| [tests/](tests/) | The harness checking its own frame watch against a fixture scene, and code_layout.py's own tests. |

## Pinned code layout

`code_layout.py` reads the Xtensa tools and instruction-cache line size from a
firmware build. The diagnostics gate checks the address-independent rows in
`launcher/main/render/code_layout.txt`:

```sh
python launcher/tools/render/code_layout.py --check launcher/build.diag
python launcher/tools/render/code_layout.py --write launcher/build.diag
```

A changed layout is measured between revisions with the Sponza performance
suite through `launcher/tools/perf/perf_compare.sh`. A slower layout is retuned
with `RENDER_ENTRY_OFFSET` before the generated file is written.

## Meshlet size captures

`meshlet_capture.py prepare --scene PATH --sizes 16 32 64` copies tracked firmware
sources into one row tree per capture under `launcher/tools/results/meshlet-sizes/tree`
and re-clusters the committed scene triangles through `rebake.py`. It prints one
`autana --wait 3600 --project TREE suite run_sponza_perf_suite --flash --out LOG`
command per row; `--suite NAME` selects the fixed-pose suite. Preparation never
builds firmware or accesses the board. Each command builds a self-test image and holds
one board lock from flash through capture.

The suite measures both culling settings through the tune API and restores the
previous setting. It prints the image build ID, the CRC-32 of the complete mounted
pack, maximum cluster triangle count, culling setting and camera sample interval
inside each FRAME COST block. The cull-off row uses the shipped pack.

`meshlet_sizes.py --scene PATH [--object NAME]` validates captures, regenerates
bakes and their packs, and writes `Render-Pipeline.md#meshlet-sizes`. The selected
renderer defaults to the scene's first renderer and must have an identity
placement. Host counts sample the camera at the suite's fixed five-second poses
and default render size. Stage times average the shipped bake's per-pose reports;
frame time averages its measured draw and upscale wall times, without panel
transfer. Missing suite blocks, mismatched poses, build IDs, pack hashes or render
sizes fail. Monitor logs cannot supply this table.
