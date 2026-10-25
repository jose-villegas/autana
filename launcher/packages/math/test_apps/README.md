# Math test app

This ESP-IDF app runs the math tests, then its benchmark suites. It needs only
the math and testkit components plus IDF VFS console support. `TESTKIT_COMPLETE failures=0` marks success.

Use the pinned IDF from a POSIX shell at the repository root:

```sh
. launcher/tools/build/idf.sh
idf_init "$PWD/launcher" "$PWD/launcher/tools/build"
```

`idf` runs relative to `launcher/`; `SDKCONFIG` and defaults paths are relative to the app.
The board layer reuses `launcher/sdkconfig.defaults` for CPU, flash and caches.

PSRAM stays disabled because this headless benchmark uses internal memory only.
Use a fresh resolved sdkconfig when changing defaults layers.

| Run | Command after initialization |
|---|---|
| Generic ESP32 target | `idf -C packages/math/test_apps -B packages/math/test_apps/build.esp32 -D SDKCONFIG=build.esp32/sdkconfig -D SDKCONFIG_DEFAULTS=sdkconfig.defaults -D IDF_TARGET=esp32 build` |
| Generic ESP32-S3 target | `idf -C packages/math/test_apps -B packages/math/test_apps/build.esp32s3 -D SDKCONFIG=build.esp32s3/sdkconfig -D SDKCONFIG_DEFAULTS=sdkconfig.defaults -D IDF_TARGET=esp32s3 build` |
| Flash board (ESP32-S3) | `idf -C packages/math/test_apps -B packages/math/test_apps/build.board -D SDKCONFIG=sdkconfig.board -D SDKCONFIG_DEFAULTS="../../../sdkconfig.defaults;sdkconfig.defaults;sdkconfig.defaults.standalone" -D IDF_TARGET=esp32s3 build flash` |
| Capture tests and bench | `autana monitor 300 --stream > docs/math/data/math_bench_capture.txt` |
| Build QEMU image | `idf -C packages/math/test_apps -B packages/math/test_apps/build.qemu -D SDKCONFIG=sdkconfig.qemu -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.qemu" -D IDF_TARGET=esp32s3 build` |
| Run QEMU | `idf -C packages/math/test_apps -B packages/math/test_apps/build.qemu qemu monitor` |

After flashing, open the monitor immediately. On USB-Serial-JTAG consoles,
the runner waits up to 30 seconds for a USB host, then delays 5 seconds before
printing `TESTKIT_START` and starting Unity. USB host detection does not mean
the serial port is open; the start delay gives the monitor time to attach.

The run starts even if host detection times out. UART consoles and QEMU skip
the wait and delay. `TESTKIT_COMPLETE failures=0` confirms the capture is complete.

QEMU skips hardware timing rows; its instruction rows support the engine's
icount ratchet. Host CTest runs the bench as a smoke check; documentation uses
the hardware capture.
