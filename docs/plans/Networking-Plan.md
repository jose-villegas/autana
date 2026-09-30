# Plan: networking, and updating the firmware over the air

**Status**: not planned for now; a later milestone. Wi-Fi and Bluetooth are
expected for apps anyway; nothing is built and the sizes below are estimates
to measure.

## What the hardware has

| Radio | What the chip offers | On this board |
|---|---|---|
| Wi-Fi | 2.4 GHz, IEEE 802.11 b/g/n | one antenna feed (J2), matching network unpopulated |
| Bluetooth | Bluetooth LE 5 and Mesh; no Classic Bluetooth | shares the same 2.4 GHz antenna |

## What the firmware does today

No Wi-Fi and no Bluetooth: `launcher/sdkconfig.defaults` enables neither, so
no radio stack is linked and none of its memory is spent.

## The constraint is internal RAM

The Wi-Fi plus TLS stack takes roughly 50-100 KB of internal heap while
running (an estimate to measure). That is the same scarce heap that keeps
LVGL out (`docs/Firmware-Architecture.md`). Some Wi-Fi and lwIP buffers can
move to PSRAM with `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`. The BLE cost is
unmeasured.

## Updating the firmware

Only what ships over USB is set in stone: the bootloader and the partition
table. Rollback lives in the bootloader and the slot layout in the partition
table, so a board flashed without them cannot gain them over the air.

| | Today |
|---|---|
| Partitions (`launcher/partitions.csv`) | `nvs`, `phy_init`, one 8 MB `factory` app slot |
| Flash (`launcher/sdkconfig.defaults`) | 16 MB |
| Image size | about 1.0 MB release, 1.7 MB diagnostics |

- **First step, still over USB:** two app slots (`otadata`, `ota_0`, `ota_1`;
  about 6 MB each fits) and rollback
  (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`: a new image boots pending-verify
  and confirms itself after a check, so a crash or power loss reverts). While
  every board is on the maintainer's desk this costs one USB flash each; it
  should land before any board leaves.
- **Later:** delivery over Wi-Fi with `esp_https_ota`, Wi-Fi credentials from a
  phone (there is no keyboard), and signed images. HTTPS authenticates the
  server, not the image; Secure Boot burns eFuses that cannot be undone, so
  try it on a spare board.
- **An update is its own mode:** the shell stops the app, frees its memory,
  updates and reboots, so Wi-Fi never runs beside an app. It checks the
  battery first.
- **Settings survive a version change:** version the NVS settings format from
  the start (`docs/plans/Settings-App-Plan.md`).
