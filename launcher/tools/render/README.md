# Render

| File | Purpose |
|---|---|
| [render_host.c](render_host.c) | Runs a firmware drawing scene on the host and writes BMP frames. |
| [render_host.h](render_host.h) | Scene interface for host rendering. |
| [render_scene.sh](render_scene.sh) | Compiles, runs, and checks one host render scene. |
| [render_all_scenes.sh](render_all_scenes.sh) | Discovers and checks all host render scenes. |
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
| [check_avi.py](check_avi.py) | Validates AVI structure and frame metadata. |
| [scenes/](scenes/) | Engine render scenes and their pinned baselines. |
| [tests/](tests/) | The harness checking its own frame watch against a fixture scene. |
