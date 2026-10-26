# testkit

Suite registration and deferred fixture cleanup shared by package tests and
engine runners. Host builds use the vendored Unity 2.7.1; ESP-IDF uses its
own Unity component.

| Build entry | Provides |
|---|---|
| `unity` | Unity assertions and test execution |
| `testkit` | `testkit/bench.h`, `testkit/cleanup.h`, `testkit/suite.h` and harness implementations; links Unity and math |
| `testkit_runner` | Standalone selection and host main; links testkit |
| `runner/idf_main.c` and `runner/lifecycle.c` | Generic ESP-IDF test and bench entry point for a package test app |

A package's `CMakeLists.txt` calls `project()` outside ESP-IDF, sets
`<name>_requires`, includes `cmake/Package.cmake` and calls `package_add()`.
That registers the ESP-IDF component or the plain-CMake library, and adds the tests.
`<PACKAGE>_BUILD_TESTS` defaults on for a top-level build and off when embedded.
The helper builds `tests/*.c` and `bench/*.c`. CTest runs the default suites
and, when `bench/` exists, a separate `<package>_bench` smoke test.

Suites include `testkit/suite.h` and register with `SUITE_REGISTER` or
`SUITE_REGISTER_ON_REQUEST`. Fixtures may use `testkit/cleanup.h` to release
resources after a failed assertion.

| Standalone runner arguments | Runs |
|---|---|
| None | All suites except those registered on request, sorted by name |
| Suite names | Named suites, including those registered on request |
| `--on-request` | All suites registered on request |
| `--list` | Prints every registered suite name |

The exit status counts test failures and runner errors. Engine runners link testkit without `testkit_runner`.

`testkit/bench.h` owns target timing, cache invalidation, instruction counts
and internal fixture allocation. The IDF component uses Unity and IDF services;
the host uses a monotonic clock. Statistics use the math package.

`runner/idf_main.c` runs default suites, then on-request suites, prints
`TESTKIT_COMPLETE failures=<n>`, and idles. `CONFIG_TESTKIT_QEMU` disables
hardware-only rows in package benches.

## Headers

<!-- generated: package-testkit-headers check: python launcher/tools/gen/package_headers.py --check -->
| Header | Purpose |
|---|---|
| [include/testkit/bench.h](include/testkit/bench.h) | bench: portable timing, allocation and measurement identity for benchmark suites. |
| [include/testkit/cleanup.h](include/testkit/cleanup.h) | Deferred cleanups registered by test fixtures. |
| [include/testkit/suite.h](include/testkit/suite.h) | suite: self-registering test entry points for a caller-owned runner. |
<!-- /generated: package-testkit-headers -->
