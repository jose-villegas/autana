# Panel video

`PANELCAP START [fps]` streams panel windows until `PANELCAP STOP`.
The default publish rate is `PANELCAP_FPS_DEFAULT` in
[`panelcap.h`](../../launcher/main/gfx/present/capture/panelcap.h).

```mermaid
sequenceDiagram
    participant H as autana video
    participant D as device
    H->>D: PANELCAP START 5
    D-->>H: PANELCAP_BEGIN fps=N image=B
    loop published presents
        D-->>H: PANEL_BIN n crc32 + n bytes
    end
    H->>D: PANELCAP STOP
    D-->>H: remaining PANEL_BIN packets
    D-->>H: PANELCAP_END presents=P dropped=D bytes=B
    Note over H,D: PANELCAP_REFUSED: reason<br/>PANELCAP ERR no memory: need N, largest free M
```

Each `PANEL_BIN n crc32` line ends with a newline followed by `n` raw bytes.
The CRC covers the payload, whose size limit is `CONSOLE_PACKET_BYTES_MAX`.
Header integers are little-endian. RGB565 pixels are high-byte first.

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

| Meaning |
|---|
| Each present attempt advances the device sequence. |
| Parts of a published present start at zero and advance by one. |
| A one marks the final part. Other parts carry zero. |
| The slices join to form one present. |

```mermaid
---
config:
  packet:
    bitsPerRow: 16
    bitWidth: 58
---
packet-beta
title Present header, one cell a byte
0-3: "present_seq"
4-11: "device_us"
12-13: "windows"
14-15: "dropped_before"
```

| Meaning |
|---|
| The sequence matches the continuation headers. |
| Device time in microseconds at the start of the present. |
| The count of dirty rectangles and their pixels that follow. |
| Presents since the previous publication that were unpublished due to the FPS gate, an image still being sent, or a redraw owed. Their windows merge into the next published present. Capped at 65535. |

```mermaid
---
config:
  packet:
    bitsPerRow: 8
    bitWidth: 100
---
packet-beta
title Window header, one cell a byte
0-1: "x0"
2-3: "y0"
4-5: "x1"
6-7: "y1"
```

| Meaning |
|---|
| The first pair is the inclusive top-left edge and the second pair is the exclusive bottom-right edge. |
| PackBits commands follow until the rectangle's pixel count is reached. |

| Rule | Detail |
|---|---|
| PackBits | A signed header from 0 to 127 copies n+1 pixels. A header from -127 to -1 repeats one pixel 1–n times. -128 is a no-op. |
| Storage | One `PANELCAP_IMAGE_BYTES` image, allocated at START from the app heap together with the encoder and packet buffers. |
| Copies | Capture copies successful panel writes into PSRAM while the image is free. |
| Window merge | The latest write wins. Overlapping and rectangular adjacent regions merge. Overflow collapses the list to its bounding box. |
| Publication | The FPS gate controls when a commit hands the image to the sender. Windows from intervening presents accumulate. |
| Redraw | START requests a full redraw. After a lost window, publication waits for the frame owner to mark the lost bounds dirty. Bands, expanded pictures, and indexed mode request a full redraw. |
| Sender | The shared console stream task packs the published image once and releases it after the last packet goes out. |
| App memory | Recording uses `PANELCAP_CAPTURE_BYTES` of app heap. From the launcher, START takes the app heap for the visit. Starting an app from there ends the recording with `PANELCAP STOP app switch`. |
| Short on memory | START refuses with `PANELCAP ERR no memory: need N, largest free M` in bytes. The host exits non-zero. |
| App switch | Stops recording with `PANELCAP STOP app switch`. The usual drain and END follow. |
| STOP | Drains the published image, frees its app memory, and sends END. A stalled USB write ends the drain. |
| Totals | END counts published presents, unpublished presents, and stream bytes excluding continuation headers. |
| Refusal | `PANELCAP_REFUSED: <reason>` for bad syntax or FPS, START while on, STOP while off, or no memory for the sender. A missing image gets `PANELCAP ERR no memory: need N, largest free M`. |
| Release | A release image cannot answer START or STOP. The host releases the lock on its END deadline. |
| Reassembly | A missing, repeated or out-of-order part discards the present. The next sequence starts at part zero. |
| Canvas | The host applies complete presents in window order and uses the screenshot turn when emitting a frame. Missing updates leave previous pixels in place. |
| Output | The MP4 samples the latest present at each 30 Hz device timestamp through the final timestamp. See [the command contract](../cli/Watch.md#panel-video). |

Recording adds redraw work and a PSRAM copy. Both can change frame timing.
