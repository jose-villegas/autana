# Console dump framing

How the device sends a block of data over the same serial console that carries its logs, and
how a host script finds it. The screenshot is the model; other verbs follow the same shape.

## The frame

```mermaid
sequenceDiagram
    participant H as host script
    participant D as device
    H->>D: SCREENSHOT\n
    D-->>H: SCREENSHOT_BEGIN size=N
    D-->>H: SCREENSHOT_DATA: base64 (header)
    D-->>H: SCREENSHOT_DATA: base64 (one row) x height
    D-->>H: SCREENSHOT_STATE: {json}
    D-->>H: SCREENSHOT_END
    Note over H,D: or one SCREENSHOT_REFUSED: reason instead of all of these
```

| Line | Carries |
|---|---|
| `SCREENSHOT_BEGIN size=N` | the byte count of the BMP that follows |
| `SCREENSHOT_DATA:` | one base64 chunk: the 54-byte header first, then rows bottom to top |
| `SCREENSHOT_STATE:` | JSON device state from the same frame as the image |
| `SCREENSHOT_END` | the stream is complete |
| `SCREENSHOT_REFUSED:` | the reason; ends the capture at once, not at the timeout |

## Rules the device follows

| Rule | Why | Where |
|---|---|---|
| Marker and data lines go through `console_emit_line()`, never `ESP_LOGx` | a fixed prefix match never has to strip a log prefix | [`console_screenshot.c:142`](../../launcher/main/shell/console/console_screenshot.c#L142) |
| `fflush(stdout)` before the first line | buffered log output must not land inside the stream | same function |
| State is passed in, not read fresh | touch and button fields describe the frame the image shows | [`console_screenshot.h:5`](../../launcher/main/shell/console/console_screenshot.h#L5) |
| Dump after the frame is drawn, before it is presented | the capture matches what is about to appear | same header |

## Rules the host follows

The decoder, [`launcher/tools/device/screenshot.py`](../../launcher/tools/device/screenshot.py), matches
`BEGIN`, `DATA:`, `STATE:`, `REFUSED:` and `END` by exact prefix and ignores every other line,
because the device keeps logging while it streams. It converts the BMP to PNG in memory with
the standard library only; opening the port is the caller's job, under the
[device lock](../cli/Device-Lock.md).

## Binary packets: FRAMEREC

`FRAMEREC START` streams each drawn frame's render input, in the
[frame record format](Frame-Record-Format.md), until `FRAMEREC STOP`. Base64
would add a third to every byte, so the stream goes as raw bytes, each run
behind a line that says how many follow.

```mermaid
sequenceDiagram
    participant H as host script
    participant D as device
    H->>D: FRAMEREC START\n
    D-->>H: FRAMEREC_BEGIN ring=N keyframe_every=K
    D-->>H: FRAMEREC_BIN n crc32\n + n raw bytes, repeated
    Note over H,D: log lines from other tasks land between packets
    H->>D: FRAMEREC STOP\n
    D-->>H: FRAMEREC_BIN ... (what the ring still held)
    D-->>H: FRAMEREC_END frames=F dropped=D bytes=B
    Note over H,D: or one FRAMEREC_REFUSED: reason instead
```

| Line | Carries |
|---|---|
| `FRAMEREC_BEGIN ring=N keyframe_every=K` | the send ring's bytes and the keyframe interval |
| `FRAMEREC_BIN n crc32` | `n` raw stream bytes follow the newline; `crc32` is theirs, in hex |
| `FRAMEREC_END frames=F dropped=D bytes=B` | frames sent, frames dropped, stream bytes sent |
| `FRAMEREC_REFUSED:` | the reason: already recording, not recording, no memory for the stream (with PSRAM's largest block) or the recorder, or neither START nor STOP |

| Rule | Why | Where |
|---|---|---|
| A packet's line and bytes go out in one `console_emit_packet()` write of at most `CONSOLE_PACKET_MAX` | no other task's output lands inside a packet | [`console.c`](../../launcher/main/console/console.c) |
| A stream's task starts with FRAMEREC START, sends what the ring holds after STOP, then `FRAMEREC_END`, and ends itself | a STOP drains the ring before the totals; a START before `FRAMEREC_END` is refused | [`console_stream.c`](../../launcher/main/console/console_stream.c), [`console_framerec.c`](../../launcher/main/shell/console/console_framerec.c) |
| A frame goes into the ring whole or not at all; a full ring drops it and counts it | a dropped frame never leaves half a record; the next frame is a keyframe | [`console_framerec.c`](../../launcher/main/shell/console/console_framerec.c) |
| While FREEZE holds the picture, a frame waits up to `FRAMEREC_HELD_WAIT_MS` for room | a STEP's frame is sent, never dropped | same file |
| Development builds only | the verb and the recorder are not in a release image | [`frame_record.h`](../../launcher/main/render/record/frame_record.h) |

On the host, [`console_packets.py`](../../launcher/tools/device/console_packets.py)
splits lines from packets however the reads cut them. It joins a line that a
packet landed inside, and refuses a packet whose crc32 differs.
[`framerec.py`](../../launcher/tools/device/framerec.py) reads the records.
`autana record` holds the port and writes the joined stream to a file or
stdout ([Watch](../cli/Watch.md)).

## Binary packets: panel video

`PANELCAP START [fps]` records panel windows until `PANELCAP STOP`.
The default admission rate is `PANELCAP_FPS_DEFAULT` in
[`panelcap.h`](../../launcher/main/gfx/present/capture/panelcap.h).
Release images have no console listener or capture code.

`autana video` shares `autana record`'s host capture path. A release image
cannot answer START or STOP; once input ends, the shared END deadline
fails the capture and releases the lock.

```mermaid
sequenceDiagram
    participant H as autana video
    participant D as device
    H->>D: PANELCAP START 15
    D-->>H: PANELCAP_BEGIN fps=N image=B
    loop admitted presents
        D-->>H: PANEL_BIN n crc32 + n bytes
    end
    H->>D: PANELCAP STOP
    D-->>H: remaining PANEL_BIN packets
    D-->>H: PANELCAP_END presents=P dropped=D bytes=B
    Note over H,D: START can answer PANELCAP_REFUSED: reason
```

Each `PANEL_BIN n crc32` line ends with a newline followed by `n` raw
bytes. The CRC covers those bytes. A payload fits
`CONSOLE_PACKET_BYTES_MAX`, including its continuation header.
All integers below are little-endian; pixels remain RGB565 high-byte first.

```mermaid
---
config:
  packet:
    bitsPerRow: 8
    bitWidth: 100
---
packet-beta
title Packet payload, one cell a byte
0-3: "present_seq"
4-5: "part"
6: "last"
7: "slice..."
```

| Part | At | Size |
|---|---|---|
| Present sequence | 0 | u32 |
| Part index | 4 | u16, starts at zero |
| Last flag | 6 | u8, zero or one |
| Stream slice | 7 | remaining packet bytes |

The `last` flag allows streaming without an encoded-present scratch buffer.
A missing, repeated or out-of-order part discards the entire present.
The next sequence starts a fresh assembly at part zero.

```mermaid
---
config:
  packet:
    bitsPerRow: 8
    bitWidth: 100
---
packet-beta
title Present header, 16 bytes, one cell a byte
0-3: "present_seq"
4-11: "device_us"
12-13: "windows"
14-15: "dropped_before"
```

| Part | At | Size |
|---|---|---|
| Present sequence | 0 | u32 |
| Device time at present start | 4 | u64, microseconds |
| Window count | 12 | u16 |
| Dropped presents before this one | 14 | u16, saturates at its maximum |
| Windows | 16 | merged dirty rectangles below |

```mermaid
---
config:
  packet:
    bitsPerRow: 8
    bitWidth: 100
---
packet-beta
title Window header, 12 bytes, one cell a byte
0-1: "x0"
2-3: "y0"
4-5: "x1"
6-7: "y1"
8-11: "packed_len"
12-15: "PackBits pixels..."
```

| Part | At | Size |
|---|---|---|
| Rectangle | 0 | four u16 edges, exclusive x1 and y1 |
| Encoded byte count | 8 | u32 |
| Pixels | 12 | `packed_len` bytes |

| Rule | Detail |
|---|---|
| Pixel codec | Apple PackBits over 16-bit pixels: signed header 0..127 copies n+1 pixels; -127..-1 repeats one pixel 1-n times; -128 is a no-op |
| Admission | Once per present: the fps gate must allow it and the sender must have finished the previous present |
| Storage | One `PANELCAP_IMAGE_BYTES` PSRAM image, derived from `GFX_FRAMEBUFFER_BYTES`, plus a `PANELCAP_WINDOWS_MAX` rectangle list |
| Windows | Each admitted send writes its rectangle into the image, last write wins. Overlapping and rectangular adjacent regions merge |
| List overflow | Collapse to the bounding box, keeping metadata bounded without encoding the panel's outer margins |
| Dropped before | Every present not sent since the previous sent present: fps gate, sender busy, or invalid window. Saturates at u16 maximum |
| Sender | Priority one, encodes only the committed image, never blocks presentation |
| STOP | Stops admission, drains the committed image, frees image and packet scratch, then sends END |
| Totals | END counts committed presents, all admission/refusal drops, and serialized packed-stream bytes excluding continuation headers |
| Refusal | Already on, not recording, bad fps, invalid syntax, unavailable PSRAM, or no memory for the sender task |

START requests a full redraw so the next drawn present initializes the canvas.
The host applies windows in order to a persistent panel canvas, then uses
the screenshot turn. Its MP4 samples the latest device present at each
30 Hz timestamp. Capture drops leave the previous canvas content in place.
See [the command contract](../cli/Watch.md#panel-video).

## Other verbs

Multi-line replies end with their own marker: `PERFMON_END` after the `PERFMON_NAME` and
`PERFMON_EVENT` lines ([`console_perf_parse.h:62`](../../launcher/main/profile/console/console_perf_parse.h#L62)),
`SOUND_END` ([`console_sound.c:48`](../../launcher/main/sound/console/console_sound.c#L48)),
`MELODY_END` ([`console_melody.c:113`](../../launcher/main/sound/console/console_melody.c#L113)).
A new verb with a multi-line reply ends it with a line starting `<VERB>_END` so a script knows when to stop
reading; see [App commands](../cli/App-Commands.md) for adding a verb.
