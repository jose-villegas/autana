# testkit

Suite registration and deferred fixture cleanup shared by package tests and
engine runners. Host builds use the vendored Unity 2.7.1; ESP-IDF uses its
own Unity component.

| Host target | Provides |
|---|---|
| `unity` | Unity assertions and test execution |
| `testkit` | `testkit/cleanup.h`, `testkit/suite.h` and cleanup implementation; links Unity |
| `testkit_runner` | Standalone registry and host main; links testkit |

A package includes `cmake/PackageTests.cmake` and calls
`testkit_add_package_tests(<package>)` after defining its library target.
`<PACKAGE>_BUILD_TESTS` defaults on for a top-level build and off when embedded.
The helper builds `tests/*.c` and registers the executable with CTest.

Suites include `testkit/suite.h` and register with `SUITE_REGISTER` or
`SUITE_REGISTER_ON_REQUEST`. Fixtures may use `testkit/cleanup.h` to release
resources after a failed assertion.

| Standalone runner arguments | Runs |
|---|---|
| None | All suites except those registered on request, sorted by name |
| Suite names | Named suites, including those registered on request |
| `--list` | Prints every registered suite name |

The exit status counts test failures and runner errors. Engine runners define
`suite_register()` themselves and link testkit without `testkit_runner`.

## Headers

<!-- generated: package-testkit-headers check: python launcher/tools/gen/shared_helpers.py --check -->
| Header | Purpose |
|---|---|
| [include/testkit/cleanup.h](include/testkit/cleanup.h) | Deferred cleanups registered by test fixtures. |
| [include/testkit/suite.h](include/testkit/suite.h) | suite: self-registering test entry points for a caller-owned runner. |
<!-- /generated: package-testkit-headers -->
