# Scene components

The calls an app makes on a loaded scene's entities, one module per component.
The scenes themselves are [Scene-Manager.md](Scene-Manager.md).

## What an app calls

Each component has its own module in `scene/`, and every call takes the
entity: `<component>_<verb>_<field>(entity, ...)`. The camera setters return
false for a non-camera entity or an invalid value.

| Module | Call | What it does |
|---|---|---|
| `scene/scene.h` | `scene_load()`, `_load_from()`, `_load_async()`, `_state()`, `_failure()`, `_cancel()`, `_release()` | loads and releases a scene by its root ([Loading](Scene-Loading.md#loading)) |
| | `scene_set_paused()` | stops every scene's animation and draw |
| `scene/entity/entity.h` | `entity_find(within, name_hash(name))` | the first entity below `within` with that name, never a root (`scene_load()` returns it); look it up once, when the scene is READY, and keep it |
| | `entity_next(within, after)` | walks the subtree below `within` in load order; `ENTITY_NONE` starts and ends it |
| | `entity_name()`, `entity_parent()`, `entity_is_within()` | its name; its parent (the root for a scene's top entities, `ENTITY_NONE` for a root); whether it is below another |
| | `entity_enabled()` / `_set_enabled()`, `entity_active()` | hides or shows it with its descendants; active is enabled with every ancestor: drawn and animated |
| `scene/transform/transform.h` | `transform_local()` / `_set_local()` | the stored `transformf_t`, in the parent's frame |
| | `transform_world()` / `_set_world()` | the derived world; setting it stores the local that puts it there |
| | `transform_world_matrix()` | the same world as the `mat4f_t` the renderer draws with |
| `scene/renderer/renderer.h` | `renderer_mesh()` / `_mesh_id()` | the mesh it draws, or NULL |
| | `renderer_set_mesh()` | draws a mesh made at run time in place of its loaded one; call again after each rebuild; false for no renderer or a skinned one |
| | `renderer_set_skin()` | skins it by asset id |
| `scene/skin/skin.h` | `skin_set_light_mode()` | [Per-renderer lighting mode](#per-renderer-lighting-mode) |
| `scene/animator/animator.h` | `animator_set_clips()` | attaches clips and a skeleton |
| | `animator_play()`, `_set_speed()`, `_set_paused()`, `_seek()` | control its playback slots |
| | `animator_clip_count()`, `_clip_name()`, `_clip_duration_ms()`, `_current_clip()` | query them |
| `scene/camera/camera.h` | `camera_first(within)`, `camera_lens()` | the first camera below `within`; a camera's lens |
| | `camera_activate()`, `camera_deactivate()` | the one active camera engine-wide |
| | `camera_set_present()`, `_set_small_fill()`, `_set_skip_hidden()` | per-camera settings; small fill defaults to `SMALL_FILL_SMOOTH` ([Small fill](../frame/Coverage.md#small-fill)) |
| | `camera_set_shift()` | an off-axis view along the camera's right and up, in picture short sides ([Camera](Camera.md#lens-shift)) |
| `scene/light/light.h` | `light_set_color()` / `_set_intensity()` | attach or update one directional light: color finite linear RGB in [0,1], intensity finite and nonnegative; a full light array or a skin without vertex colors fails without mutation |
| | `light_color()` / `_intensity()` | read it; zero when none |
| not the scene's | `render_context_set_scale()`, `_set_dynamic_resolution()`, `_frame()` | the render size, dynamic resolution and what the last frame drew belong to the render context the scene draws through ([Mesh-Rendering.md](../Mesh-Rendering.md#the-render-context)) |

`transformf_*` is the transform value; `transform_*` is the component.
`lens_t` (`scene/camera/lens.h`) is a camera's lens value; `camera_*` is the
component.

A new light starts white with zero intensity. Its world direction is its
entity's rotated +Y axis; moving its rotation changes skin shading next frame,
including while the pose is paused. Scene ambient and tone map are private;
SCNE settings apply ambient and tone map before authored lights attach.
All lit skins share world-space lights. Their normal palettes include entity
and joint scale through an inverse-transpose transform. Skin dirty bits are
consumed by the skin stage even when another scene is active.

Exactly one camera is active engine-wide, and it draws the active renderers of
its own scene. Several scenes may be loaded at once; activating another scene's
camera changes what is seen.

## Per-renderer lighting mode

Each skinned renderer reads its lighting mode from SKIN, direct by default;
`skin_set_light_mode()` changes it. See
[Lighting modes](../animation/Skinned-Lighting.md#lighting-modes).
