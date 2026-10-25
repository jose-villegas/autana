# Math test app

This ESP-IDF app runs the math tests, then its benchmark suites. It needs only
the math and testkit components. `TESTKIT_COMPLETE failures=0` marks success.

| Run | Command from the repository root |
|---|---|
| Generic ESP32 target | `idf.py -C launcher/packages/math/test_apps -D IDF_TARGET=esp32 build` |
| Board capture (ESP32-S3) | `idf.py -C launcher/packages/math/test_apps -B build.board -D SDKCONFIG=sdkconfig.board -D SDKCONFIG_DEFAULTS="../../../sdkconfig.defaults;sdkconfig.defaults;sdkconfig.defaults.standalone" -D IDF_TARGET=esp32s3 build flash` |
| Capture tests and bench | `idf.py -C launcher/packages/math/test_apps -B build.board monitor > docs/math/data/math_bench_capture.txt` |
| Build QEMU image | `idf.py -C launcher/packages/math/test_apps -B build.qemu -D SDKCONFIG=sdkconfig.qemu -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.qemu" -D IDF_TARGET=esp32s3 build` |
| Run QEMU | `idf.py -C launcher/packages/math/test_apps -B build.qemu qemu monitor` |

Stop the monitor after the completion record. QEMU skips hardware timing rows;
its instruction rows support the engine's icount ratchet. Host CTest runs the
bench as a smoke check; documentation uses the hardware capture.
