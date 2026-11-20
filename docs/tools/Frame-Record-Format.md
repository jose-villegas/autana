# Frame record format

The bytes of a frame record stream: each drawn frame's render input, so a
host redraws the frame with the same renderer. A board streams it over the
console ([Console framing](Console-Framing.md#binary-packets-framerec)); a host
render writes it with `--record` ([Render harness](Render-Harness.md)).

| Format | Version | Writer (C) | Reader (Python) |
|---|---|---|---|
| frame record stream | `FRAME_RECORD_VERSION` 2 | [`frame_record.c`](../../launcher/main/render/record/frame_record.c) | [`framerec.py`](../../launcher/tools/device/framerec.py) |

The layout is [`frame_record_format.h`](../../launcher/main/render/record/frame_record_format.h).
The stream is cut at the renderer's input, not at scene state and not at
pixels: what reached `render_context_draw()` is all a redraw needs.

## Records

Every record is a head, then `length` bytes of body, little-endian. A reader
skips a type it does not know by its length. The type + length framing and
the keyframe as a state snapshot follow GFXReconstruct's capture files and
trimmed captures<sup>[[123]](../Citations.md#lunarg-gfxreconstruct)</sup>.

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 6
    bitWidth: 70
---
packet-beta
title Record head, 6 bytes before every body
0-1: "type"
2-5: "length"
```

| Part | At | Size |
|---|---|---|
| Stream header | 0 | one `HEADER` record |
| Frames | after the header | per frame: `FRAME`, then its `bytes` of records; a final `CHECK` may follow |

| Type | Number | Body |
|---|---|---|
| `HEADER` | 1 | version, panel size, keyframe interval, build id |
| `PACK` | 2 | a pack's slot, crc32 and name, before the first mesh that names it in a keyframe interval |
| `FRAME` | 3 | index, dt, flags, instance and batch counts, following record bytes |
| `VIEW` | 4 | the frame's `render_view_t`, as the engine holds it |
| `CHOICE` | 5 | the resolution step, whether the frame was generated, the scale, the clear colour, drawn width and height |
| `MESH` | 6 | a drawn mesh: kind, where its arrays live, model matrix |
| `POSE` | 7 | skin location, one `float[16]` per joint, then `u8[3]` per lit vertex |
| `DYNMESH` | 8 | a mesh built this frame: then `float[3]` + `u8[3]` per vertex, `u16[3]` per triangle, and a textured mesh's `i16[2]` UV per vertex |
| `TUNE` | 9 | a tunable's value: on a keyframe each off its initial, else each that changed |
| `CHECK` | 10 | the crc32 of a keyframe's composed picture, carried by the next frame the sink takes |
| `SETTINGS` | 11 | the context's `render_settings_t`, on keyframes and changes |
| `LODSTATE` | 12 | pre-draw pick phase, triangle-budget steering and coarse LOD words for every instance and every batch copy |

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(6) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 70
---
packet-beta
title HEADER content, 40 bytes after the head
0-1: "version"
2-3: "panel_width"
4-5: "panel_height"
6-7: "keyframe_every"
8-39: "build_id"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #8fb3ee !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(6) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 8
    bitWidth: 70
---
packet-beta
title PACK content, 37 bytes after the head
0-0: "slot"
1-4: "crc32"
5-36: "name"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #c7aaee !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #93d6b0 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 17
    bitWidth: 50
---
packet-beta
title FRAME content, 17 bytes after the head
0-3: "index"
4-7: "dt_ms"
8-8: "flags"
9-10: "instances"
11-12: "batches"
13-16: "bytes"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 6
    bitWidth: 120
---
packet-beta
title CHOICE content, 10 bytes after the head
0-1: "step"
2-2: "generated"
3-3: "scale_percent"
4-5: "clear"
6-7: "width"
8-9: "height"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(6) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(7) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(8) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(9) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(4) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(5) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(6) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(7) rect:nth-of-type(1) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 16
    bitWidth: 50
---
packet-beta
title MESH content, 96 bytes after the head: 32, then the model matrix
0-1: "index"
2-2: "kind"
3-3: "placed"
4-4: "faces"
5-5: "pack"
6-9: "entry"
10-13: "offset"
14-14: "pack"
15-18: "entry"
19-22: "offset"
23-23: "pack"
24-27: "entry"
28-31: "offset"
32-95: "model"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(2) { fill: #93d6b0 !important; } g:nth-of-type(2) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(4) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(5) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(6) { fill: #e3e3e3 !important; } g:nth-of-type(2) rect:nth-of-type(7) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #93d6b0 !important; } g:nth-of-type(3) rect:nth-of-type(2) { fill: #e3e3e3 !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 16
    bitWidth: 50
---
packet-beta
title POSE content head, 15 bytes after the record head
0-1: "index"
2-3: "joints"
4-5: "lit"
6-6: "pack"
7-10: "entry"
11-14: "offset"
```

```mermaid
---
config:
  packet:
    bitsPerRow: 8
    bitWidth: 120
---
packet-beta
title DYNMESH content head, 17 bytes after the record head
0-1: "index"
2-2: "fitted"
3-3: "double_sided"
4-7: "position_scale"
8-11: "vertices"
12-15: "triangles"
16-16: "textured"
```

```mermaid
---
config:
  themeCSS: "g:nth-of-type(2) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(3) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(4) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(1) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(2) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(3) { fill: #e3e3e3 !important; } g:nth-of-type(5) rect:nth-of-type(4) { fill: #8fb3ee !important; } .packetLabel { fill: #1b1b1b !important; }"
  packet:
    bitsPerRow: 12
    bitWidth: 60
---
packet-beta
title TUNE content, 36 bytes, and CHECK content, 12 bytes, after the head
0-31: "name"
32-35: "value"
36-39: "index"
40-41: "width"
42-43: "height"
44-47: "crc32"
```

A `MESH` names three places, each a pack slot, entry and byte offset: its positions, its
colours, and for a batch the copies' places. `faces` says which colours: 1 for one
RGB565 per triangle, 0 for RGB888 per vertex. A mesh the app made has
`FRAME_RECORD_NO_PACK` and `FRAME_RECORD_NO_ENTRY` there; its data rides in
the same frame's `POSE` or `DYNMESH`.

`POSE.skin` names the skin entry. Its palette follows the head, then `lit`
vertex colours when the posed mesh owns its lighting. `SETTINGS` and `VIEW`
carry the engine structs; writer and reader must share their layout.

| Kind | Number | The mesh |
|---|---|---|
| `STATIC` | 0 | a pack mesh as it is |
| `BATCH` | 1 | a pack mesh's copies, placed by a pack batch |
| `SKINNED` | 2 | a pack mesh posed by the `POSE` of the same index |
| `BUILT` | 3 | built at run time from the `DYNMESH` of the same index |

## LODSTATE

A keyframe is a full restart point. `LODSTATE` captures what picking reads
before that frame draws, because picking updates the coarse words and
advances the context's phase and triangle-budget steering.

```mermaid
flowchart LR
    Phase["Pick phase and steering"] --> Count["Mesh word count"]
    Count --> Words["Coarse words"]
    Words --> Next{"More meshes?"}
    Next -->|yes| Count
    Next -->|no| Draw["Draw keyframe"]
```

| Field | Encoding |
|---|---|
| Pick phase | `u32 raster.pick_round` |
| Steering | raw `r3d_lod_steer_t`, including its padding, as the engine holds it: tolerances, picked coverage, histogram halves, bin, drawn triangles, frame count and settled flag |
| Mesh word count | `u32`, one for each mesh in FRAME order |
| Coarse words | `u32[word_count]`; a batch holds every copy's words in copy order |

The prefix is packed `{uint32_t pick_round; r3d_lod_steer_t steer;}`.
HEADER’s build id pins the steering layout and size. `framerec.py` keeps
LODSTATE opaque; replay uses the matching engine build to read it.

The record follows the keyframe's meshes. Replay resolves those meshes,
then restores the snapshot before drawing. Seeking draws forward from the
keyframe; a generated frame warps the preceding drawn frame through its VIEW.

## Rules

| Rule | Detail |
|---|---|
| Keyframes | a full restart point, including pre-draw `LODSTATE`; every `FRAME_RECORD_KEYFRAME_EVERY` frames, and the frame after a drop: a `MESH` for every drawn mesh and every pack named again; between them only a `MESH` whose mesh, its arrays or matrix changed. A generated frame due one passes it to the next drawn frame |
| A frame is whole or absent | the sink takes a frame's bytes in one write or drops the frame; the next `FRAME` sets `FRAME_RECORD_AFTER_DROP` |
| Pack identity | a `PACK`'s crc32 is the one its pack header states: a reader loads the same pack bytes and checks them against it |
| Resident copies | a scene that copies pack meshes into its own memory tells the asset store (`asset_store_copies_add()`), so the copies are named by the entries they copy |
| Generated frames | a `FRAME` flagged `FRAME_RECORD_GENERATED` was warped from the last drawn frame and has no `MESH` |
| Picture check | `CHECK` hashes the half picture or the framebuffer, whichever the compose wrote, rows top to bottom; a host redraw is compared against it, as apitrace compares replayed images with references<sup>[[124]](../Citations.md#apitrace-usage)</sup> |
| Live drops | a board drops whole frames when its send ring is full and counts them, as target tracers count what their buffer lost<sup>[[125]](../Citations.md#segger-systemview)</sup>; while FREEZE holds the picture it waits up to `FRAMEREC_HELD_WAIT_MS` for room first |
| Reading | a stream that does not open with a `HEADER` of its fixed length is refused from that first head, before any length it claims is waited on; a `HEADER` of another `FRAME_RECORD_VERSION` is refused too |
| Builds | development and host builds only: in a release build every `frame_record_*` call is an empty inline |
| Owners | type numbers, flags and sizes: [`frame_record_format.h`](../../launcher/main/render/record/frame_record_format.h); the interval and stage name: [`frame_record.h`](../../launcher/main/render/record/frame_record.h) |

`framerec.py summary FILE` prints a stream's header and its frame and record
counts.
