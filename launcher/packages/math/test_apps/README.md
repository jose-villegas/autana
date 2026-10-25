# Math test app

This ESP-IDF app runs the math tests, then its benchmark suites. It needs only
the math and testkit components. `TESTKIT_COMPLETE failures=0` marks success.

| Run | Command from the repository root |
|---|---|
| Build and flash ESP32-S3 | `idf.py -C launcher/packages/math/test_apps set-target esp32s3 flash` |
| Capture tests and bench | `idf.py -C launcher/packages/math/test_apps monitor > data/math_bench_capture.txt` |
| Build QEMU image | `idf.py -C launcher/packages/math/test_apps -B build.qemu -D SDKCONFIG=sdkconfig.qemu -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.qemu" -D IDF_TARGET=esp32s3 build` |
| Run QEMU | `idf.py -C launcher/packages/math/test_apps -B build.qemu qemu monitor` |

Stop the monitor after the completion record. QEMU skips hardware timing rows;
its instruction rows support the engine's icount ratchet. Host CTest runs the
bench as a smoke check; documentation uses the hardware capture.
