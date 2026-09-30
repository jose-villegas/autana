# Animation Tracks

`launcher/main/anim/` plays keyed values over time. A track is a list of
keys, each a time and a value, and one call gives the value at any moment.
It does not know what it drives: a caller maps the numbers onto a camera, a
light, a material value, anything a scene exposes. Its layer is in
[Firmware-Architecture.md](Firmware-Architecture.md); it sits beside `util/`
and allocates nothing.

The format is glTF 2.0's own animation model, so a track authored in Blender
or any other exporter plays back as it was made.

```mermaid
flowchart LR
    Author["Blender, or any glTF exporter<br/><i>.glb with an animation</i>"] --> Bake["tools/anim/bake_tracks.py"]
    Bake --> C["*_tracks_generated.c<br/><i>a clip of anim_track_t</i>"]
    C --> Sample["anim_clip_seconds(), anim_track_sample()<br/><i>clip time, then each track</i>"]
    Sample --> Caller["the caller's own object<br/><i>eye, colour, fov, ...</i>"]
```

## What a track stores

| Field | Meaning |
|---|---|
| `times` | Key times in seconds, strictly increasing |
| `values` | `width` floats per key; three runs of them per key for `ANIM_CUBIC` |
| `width` | 1 to 4 components: a scalar, a translation, a quaternion |
| `interp` | `ANIM_STEP`, `ANIM_LINEAR` or `ANIM_CUBIC` (glTF `CUBICSPLINE`) |
| `quaternion` | The value is an xyzw rotation: linear keys slerp, cubic ones are normalised |

A cubic key holds an in-tangent, the value and an out-tangent, in units per
second, exactly as glTF stores them. A track has one interpolation.

A glTF node is animated by up to three tracks, named `node/translation`,
`node/rotation` and `node/scale`. Anything else is reached by
`KHR_animation_pointer`, which names a property by path; a track baked from it
is named by that path with the object's index replaced by its glTF name, for
example `lens/perspective/yfov` for `/cameras/0/perspective/yfov`. Objects are
bound by name, so a re-export that reorders nodes keeps its symbols. A pointer
to a rotation is a quaternion track like a node's. A channel that never
changes is baked as one key.

A **clip** is the tracks of one animation, on one timeline as in glTF: a
track's key times are clip seconds, so tracks that start or end at different
times stay in step, and the clip's duration is the last key of any of them.

## Sampling

```c
float out[ANIM_WIDTH_MAX];
const float seconds = anim_clip_seconds(&prefix_clip, t_ms, ANIM_LOOP);
anim_track_sample(&prefix_node_translation, seconds, out);
```

`anim_clip_seconds()` wraps `t_ms` at the clip's duration with an integer
remainder, so a long run keeps its millisecond resolution, and converts to
seconds once. `ANIM_CLAMP` holds it at the duration instead; a loop authors
its first key again as its last. `anim_track_sample()` holds a track's first
value before its first key and its last after its last, as glTF defines.
`anim_quat_rotate()` turns a vector by a sampled rotation, which is how a
camera track gives an `r3d_camera_t` its look direction.

## Authoring

1. Animate in Blender and export glTF binary (`.glb`) with animation on. Name
   the action: the baker finds it by name. Blender exports keys as
   `LINEAR`, `STEP` or `CUBICSPLINE` per curve; a property outside translation,
   rotation and scale needs the exporter's animation-pointer option.
2. Keep the file beside the code that plays it, as an asset.
3. Bake it from `launcher/`:

   ```sh
   python tools/anim/bake_tracks.py PATH/asset.glb --animation NAME --name PREFIX --out-dir DIR
   ```

   That writes `DIR/PREFIX_tracks_generated.{c,h}`: a `const anim_track_t
   PREFIX_<node>_<path>` per channel, the clip `PREFIX_clip`, and
   `PREFIX_track_names[]`, each track's name in the clip's order. Nothing in
   the firmware refers to the names, so the linker drops them. The command is in the file's banner; the output is
   checked in and never edited.
4. Sample what the scene needs, and convert at its own boundary.

The baker refuses keys out of order, a channel with no node and no pointer,
and a value that is not finite. Skins and morph weights are not tracks.

## Playing a new property

A property needs no change to `anim/` or the baker. Animate it, bake it, and
sample the track where the property is read:

```c
float fov[ANIM_WIDTH_MAX];
anim_track_sample(&prefix_lens_perspective_yfov, seconds, fov);
```

Mapping the value onto the object, including any unit or fixed-point
conversion, belongs to the caller. Tracks are float; a fixed-point caller
converts after sampling, and `render/r3d_trs.h` does it for an object drawn
through small3dlib.

## Looking at a baked animation

`launcher/tools/anim/sample_tracks.sh` builds a small program against a baked
file and prints every track every N milliseconds, through the same
`anim_track_sample()` the firmware runs. With `--poses NODE W H TAN NEAR` it
prints a camera node as the poses file
[`report_triangle_sizes.sh`](../launcher/tools/r3d/README.md#triangle-sizes)
reads, so the poses are always the animation's own.

## How it is tested

- `suite_anim_track.c` holds each interpolation, clamp and loop, tracks on one
  clip timeline, a long run's resolution, the quaternion path and the cubic
  layout to hand-built tracks.
- `tools/tests/test_anim_bake.py` builds a glTF of its own with every
  interpolation, a quaternion, a pointer-targeted scalar and a non-zero first
  key, bakes it, samples it in C, and holds every value to the Python sampler
  in `launcher/tools/gltf/gltf_read.py`, looping and clamped.

## Rules

- **Single precision only**, as in [Mesh-Rendering.md](Mesh-Rendering.md#rules-the-layer-keeps).
- **Portable.** No ESP-IDF and no allocation; a host suite runs all of it.
- **The caller owns the environment.** Time is passed in; nothing reads a
  clock.
