# Building a Scene

Start here to put a 3D model on screen: import a mesh, place it in a scene
file, light it, add a camera, and draw what the scene says. The rest of this
folder explains each step in depth; the [index](README.md) lists it. For the
shell's frame ownership see [Firmware Architecture](../Firmware-Architecture.md).

```mermaid
flowchart LR
    Model["Source model<br/><i>zipped OBJ</i>"] --> Import["Import file<br/><i>.import.toml</i>"]
    Import --> Scene["Scene file<br/><i>.scene.toml</i>"]
    Scene --> Bake["mesh_import.py"]
    Bake --> Meshes["Baked meshes<br/><i>_mesh_generated.c</i>"]
    Bake --> Table["Scene table<br/><i>_scene_generated.c</i>"]
    Meshes --> Draw["Your scene<br/><i>raster_draw()</i>"]
    Table --> Draw
```

You need the Python environment in
[`launcher/tools/r3d/README.md`](../../launcher/tools/r3d/README.md) and a host
C compiler for the [render harness](../tools/Render-Harness.md).

## 1. Import a mesh

An import file describes one mesh asset: where its source model is and where the
generated files go.

```toml
[source]
url = "https://example.invalid/hall.zip"
sha256 = "..."                   # the download is checked against it
path = "hall.obj"                # inside the archive
cache = "hall"
credit = "Hall, by A. Modeller, CC BY 4.0."   # written into every banner

[output]
directory = ".."
name = "hall"                    # the mesh's symbol prefix: hall_mesh
```

With only these it imports the mesh as authored: its triangles, with each
vertex coloured by its material's albedo and no light. That is enough for a
mesh that needs no lighting; turn a step on by adding its `[process.*]` table
when it does (`simplify` to a triangle budget, `light` to bake a sun and sky
into the colours, `visibility` to drop what the camera can never see). The
steps, their order and every field are in
[Mesh-Import.md](Mesh-Import.md#import-file).

## 2. Place it in a scene

A scene file lists objects, each with a transform and one component. A mesh
renderer points at the import file:

```toml
[[objects]]
name = "hall"

[objects.mesh_renderer]
mesh = "hall.import.toml"
```

Add more mesh renderers to place more meshes; give one a `position`, a
`rotation` in degrees or a `scale` to move it. A mesh that bakes light must sit at
the identity transform, because its light is baked where it stands
([Scene-Files.md](Scene-Files.md)).

## 3. Light it

Light is baked, so it is part of the scene file and the mesh's `process.light`
step reads it. A directional sun is an object whose rotation points it; sky and
ambient are scene settings; `tonemap_white` sets how bright the result is.

```toml
tonemap_white = 0.35

[sky]
color = [0.55, 0.68, 0.9]
intensity = 0.9
rays = 48

[ambient]
color = [1.0, 1.0, 1.0]
intensity = 0.06

[[objects]]
name = "sun"
rotation = [30.0, -45.0, 0.0]    # pitch, yaw, roll: a sun 30 degrees off overhead

[objects.light]
type = "directional"
color = [1.0, 0.92, 0.78]
intensity = 3.0
disc_degrees = 1.2
rays = 8
```

## 4. Add a camera

The camera object has the lens, the box it moves within (what `visibility`
culls against) and, if it flies a glTF animation, the tracks that
`tools/anim/bake_tracks.py` baked:

```toml
[[objects]]
name = "camera"

[objects.camera]
half_fov_short_tan = 0.62
near_z = 6.0
region = { min = [-1400.0, 20.0, -620.0], max = [1270.0, 1250.0, 550.0] }
```

## 5. Bake

```sh
python launcher/tools/r3d/mesh_import.py path/to/hall.scene.toml
```

writes the lit mesh for each renderer and then the `hall` scene table, the
const data a scene reads. A mesh with no light or visibility step can also be baked on its
own, from its import file. Generated files are committed as written and never
reformatted.

## 6. Draw it

A scene reads the table instead of hard-coding what to draw. It builds a
`raster_instance_t` from each mesh renderer it wants, gives the raster a
scratch block of PSRAM, and each frame asks the camera object where it is:

```c
#include "hall_scene_generated.h"
#include "render/r3d.h"

static raster_instance_t instances[2];
static raster_t raster;

static void
enter(void) {
    for (int i = 0; i < hall_scene.renderer_count; i++) {
        instances[i] = (raster_instance_t){hall_scene.renderers[i].mesh, hall_scene.renderers[i].transform};
    }
    raster = (raster_t){.instances = instances, .instance_count = hall_scene.renderer_count,
                        .width = 184, .height = 224, .clear = sky_colour,
                        .destination = gfx_framebuffer(), .destination_width = GFX_WIDTH,
                        .destination_height = GFX_HEIGHT};
    raster.scratch = heap_caps_malloc(raster_scratch_bytes(&raster), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void
update(uint32_t elapsed_ms) {
    const camera_t camera = r3d_scene_camera_at(hall_scene.camera, elapsed_ms);
    raster_draw(&raster, &camera, display_shell_quarter());
}
```

then `raster_upscale()` in the frame callback. How a frame is cut across the two
cores and what `raster_draw()` does with each instance is in
[Mesh-Rendering.md](Mesh-Rendering.md). To see the scene without a board, declare it
for the [render harness](../tools/Render-Harness.md#declaring-a-scene).

## Checking it

- `python -m unittest discover -s launcher/tools/tests -p "test_r3d*.py"` checks
  the import and scene files, with the environment installed.
- A host suite that draws instances at transforms and checks where they appear
  is `launcher/test/suites/suite_r3d_scene.c`; copy its quad meshes to test your
  own placement.
- Regenerating the committed meshes must change only their banners: diff them.
