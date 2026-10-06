# Render/Tests

| File | Purpose |
|---|---|
| [asset_fixture.c](asset_fixture.c) | Scene that mounts the bundle it is given, or refuses to render. |
| [check_frame_watch.sh](check_frame_watch.sh) | Checks that the frame watch fails repeated work and passes one-off work. |
| [check_scene_assets.sh](check_scene_assets.sh) | Checks that a scene's `scene_assets` bundles are found, overridable, and required. |
| [frame_watch_fixture.c](frame_watch_fixture.c) | Scene that allocates or prints every frame or once. |
| [test_code_layout.py](test_code_layout.py) | Checks pin discovery, cache-line spans, and generated-layout differences. |
