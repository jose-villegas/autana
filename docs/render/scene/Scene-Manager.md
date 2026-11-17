# Scene manager

Scenes are engine machinery the shell drives. An app loads a scene by name and
activates a camera; the shell advances every loaded scene and draws through the
active camera each frame, so an app never calls draw. The code is
`launcher/main/scene/`; [Building-a-Scene.md](../Building-a-Scene.md#6-draw-it)
shows an app using it.

The calls an app makes are [Scene-Components.md](Scene-Components.md);
loading and memory are [Scene-Loading.md](Scene-Loading.md); the shell's
frame is [Scene-Frame.md](Scene-Frame.md).

## The world

The engine has one world. Every loaded scene is a subtree in it under its
**root entity**, which `scene_load()` returns; moving or disabling the root
moves or hides the whole scene. `scene_bounds()` gives the world box around
what a scene draws now, from its root or any entity in it.

An `entity_t` is a [handle](../../../launcher/main/core/handle.h): owner,
slot and generation. The world keeps a generation per slot for the whole run,
so a handle kept after its scene's release is refused, even once the slot
is issued again. A refused handle changes nothing and reads as zero, NULL
or the identity; a development device build also logs and asserts on a
stale or foreign one. A slot whose generation would wrap is retired.

```mermaid
flowchart LR
    Table["entity table<br/>every slot, a generation each"] --> RootA["root slot"]
    Table --> RowsA["one run of row slots"]
    RootA --> RecA["scene record:<br/>request, root transform, state"]
    RowsA --> ColA["scene columns<br/>row = slot - base"]
    RecA --> ColA
```

| Piece | Lives | Holds |
|---|---|---|
| entity table | PSRAM, allocated once for the run | the latest generation of each of `HANDLE_SLOT_MAX + 1` slots |
| scene record | the manager, `SCENE_ROOTS_MAX` of them | its root's slot, transform and flags, its request and state, its failure |
| scene columns | the scene's block ([below](#what-a-scene-holds)) | its entities' components, indexed by row |

A root slot is issued at the request, before the scene's entity count is
known; the rows' run is issued when the scene opens. A call on an entity
finds its record by slot range, so its cost does not depend on the scene.

## What a scene holds

A scene holds packed component arrays indexed by a row: the entity's index
in the SCNE entry. Attaching clips allocates the animator's bindings and pose
buffers in PSRAM. A skin's model matrices, palette and output mesh share one
internal RAM block. Lit colors and the world-space normal palette use a
second when the scene has directional lights or nonzero ambient. A baked-only
scene reserves no runtime-light storage.

A skin holds both blocks only while its entity is active, so memory follows
the copies shown, not the copies loaded:

| When | What the skin does |
|---|---|
| The scene loads | checks the skin; takes no blocks |
| The first step or read that finds it active | takes them, skinned in its animator's current pose, and relights |
| It stops being active | gives them back at once; its renderer reads the bind mesh |
| Its blocks do not fit | it is not drawn, and `scene_failure()` names it, until it stops being active |

`renderer_set_skin()` takes an active entity's blocks at once and keeps its
old skin when they do not fit. The first light allocates every held skin's
lit block transactionally; attachment failure preserves the scene.

| Array | One per | Holds |
|---|---|---|
| `locals[]` | entity | canonical local position, quaternion rotation and scale, in the parent's frame; loaded placements must decompose without shear or reflection |
| parents | entity | the scene entry's `entity_parents`, read in the pack: a lower entity or none |
| `world_slots[]` | entity | its index in `worlds[]`, or none when it has no children |
| `worlds[]` | entity with children | its world matrix, kept for its children; a separate block, absent when no entity has children |
| `animators[]` | entity | optional clip configuration, layer, independent slot clocks and pose buffers |
| `skinned_renderers[]` | entity | optional SKIN and bind-mesh views, output positions, bounds and lit colors; bind-color pointers without runtime lights |
| `lights[]` | entity | optional directional light, linear color and intensity; at most `R3D_SKIN_LIGHT_MAX` attached per scene |
| `flags[]` | entity | enabled, active (enabled with every ancestor), moved since the last draw, world moved in this resolve, and changed since the last skin stage |
| `renderers[]` | entity that draws | a view of its lit mesh in the asset pack, its entity id, and its world matrix |
| `cameras[]` | camera | lens, clear color, its entity id (its pose is the entity's world) |
| `instances[]` | renderer | the list the raster draws this frame, refilled from `renderers[]` |
| `staged[]` | entity | the entities with an animator, a skin or a light, in entity order; the animation stage walks only these |
| `active_renderers[]` | renderer | the renderers whose entity is active, in renderer order; rebuilt at the resolve after an active flag flips |
| names | entity | the scene entry's, read in the pack with each name's hash; looked up only by `entity_find()` |

Array lengths come from the scene's `SCNE` entry
([SCNE.md](../../assets/formats/SCNE.md)). SCNE v5 carries entity name hashes, parents, authored lights, lighting settings, skins, animators and renderer colour-set selections.
Authored lights attach at load, and `light_set_*()` attaches or changes
them at run time; only skinned renderers are lit at run time, baked renderers
keep their pack colors. Names and immutable mesh and track views point into
the asset pack. Animator rows create animators; a camera path is an animator
on the camera's entity whose tracks bind to that entity through
`ANIM_TARGET_SELF`. Binding resolves every clip before playback.

## Hierarchy

Every entity has at most one parent, listed before it in the entry, so one
forward pass meets every parent before its children. An entity with no
parent in the entry sits under the scene's root, whose transform places the
whole scene and whose enabled flag hides it. A local transform is in
the parent's frame; a world is the parent's world times the local, an exact
`transformf_t` (content keeps parents' scale even:
[SCNE.md](../../assets/formats/SCNE.md)).

| Step | When | Does |
|---|---|---|
| `entity_set_enabled()` | the call | recomputes the active flag of the entity and every later one, so a disabled parent hides its descendants at once; each keeps its own enabled flag |
| resolve | each `scene_render()`, after animation, before the draw | rebuilds the world of each parent and active renderer whose own transform moved or whose parent's world did, in entity order; an inactive renderer's world waits until it turns active |
| draw | after the resolve | the raster reads the renderers' worlds ([Each frame](Scene-Frame.md#each-frame)) |

Property clips move an entity's own transform, so a clip on a parent carries
its children in the same frame. A camera, light or skinned renderer sits
directly under the scene root: the entry check refuses one with a parent,
and a light or skin does not attach under one.
