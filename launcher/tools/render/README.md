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
