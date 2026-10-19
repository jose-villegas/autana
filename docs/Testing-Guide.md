# Testing Guide

How this project tests firmware, and why. Read the topic you need; none of
them depends on reading the others first.

**First result, no hardware:** in Git Bash on Windows or a Linux terminal,
run `launcher/test/run_tests.sh`.

- It needs a host C compiler, not ESP-IDF or a board.
- A good run ends with `0 Failures` and `OK`; most of the wait is compiling.
 On Windows you need three installs first: [Git for Windows](https://git-scm.com/download/win)
(provides Git Bash), a compiler (WinLibs, which includes `make`) and
Python 3; [Host tests](testing/Host-Tests.md#install-what-you-need) has the
commands.

| Word | Means |
|---|---|
| suite | one C file of related tests, e.g. `suite_gesture.c` |
| diag build | the self-test firmware: the normal firmware plus every suite (`CONFIG_LAUNCHER_SELFTEST`) |
| scope | which sources a diag build compiles: all suites ("full"), or only the performance ones ("perf scope") |
| autorun | the diag image runs every suite at boot instead of waiting for a command |
| `BUILDID` | a console command that returns the running image's build ID; used as a "still alive" check |

```mermaid
flowchart LR
    Code["your change"] --> Host["Host tests<br/><i>seconds, no board</i>"]
    Host --> Qemu["QEMU<br/><i>device image, no board</i>"]
    Qemu --> Board["Board<br/><i>real chip, real timing</i>"]
```

Each step proves more and costs more. Stop at the first one that answers
your question.

| your question | read |
|---|---|
| How do I run the tests? | [Host tests](testing/Host-Tests.md) |
| Why does the host stop what the board would stop? | [Host tests: device limits](testing/Host-Tests.md#the-host-enforces-the-devices-memory-limits) |
| No board free, or no board at all | [QEMU](testing/QEMU.md) |
| One suite on the board, no rebuild | [Device tests: runsuite](testing/Device-Tests.md#runsuite-the-everyday-device-loop) |
| Console recipes, scene tour, pytest-embedded | [Device tests: pytest-embedded](testing/Device-Tests.md#console-recipes-and-lifecycle-coverage) |
| Is the board itself working? | [Device tests: POST](testing/Device-Tests.md#post-is-a-third-thing) |
| Instruction ratchet, frame watch, perf-test rules | [Performance and budgets](testing/Performance-and-Budgets.md) |
| How do I make this code testable? | [Writing a test](testing/Writing-a-Test.md#making-things-testable) |
| I am adding a suite | [Writing a test: adding a suite](testing/Writing-a-Test.md#adding-a-suite) |
| Which suite covers what I changed? | [Writing a test: finding suites](testing/Writing-a-Test.md#finding-the-suite-for-an-area) |
| What do RELEASE, DEVELOPMENT and SELFTEST gate? | [`Build-Variants.md`](Build-Variants.md) |
| I want to see a screen without flashing | [`tools/Render-Harness.md`](tools/Render-Harness.md) |
| CI says my function is too complex | [`tools/Complexity-Gate.md`](tools/Complexity-Gate.md) |

## Two runners, one set of suites

Portable suites (`launcher/test/suites/`, each package's `tests/`, each app's
`tests/`) compile into every runner that can take them. A device-only suite
guards its body with `#ifdef DEVICE_BUILD`; a host-only one uses the opposite
guard, defines an empty runner for the device, and registers once outside it.

```mermaid
flowchart LR
    subgraph sources["sources"]
        Portable["portable suites"]
        DeviceOnly["device-only suites"]
    end

    subgraph runners["runners"]
        Host["host run_tests.sh"]
        Board["SELFTEST image on the board<br/><i>runsuite, or a full run</i>"]
        Qemu["the same SELFTEST image,<br/>under QEMU"]
    end

    Portable --> Host
    Portable --> Board
    Portable --> Qemu
    DeviceOnly --> Board
    DeviceOnly --> Qemu
```

| Runner | Runs | Proves | Cost |
|---|---|---|---|
| Host | portable suites | the logic is right on x86 | seconds once built |
| QEMU | every suite | the Xtensa build behaves; no timing | minutes, no board |
| Board | every suite | the same source works on this chip | `runsuite` seconds; full run about 18 minutes |

POST is a third thing, a boot-time hardware check rather than a Unity suite:
[Device tests: POST](testing/Device-Tests.md#post-is-a-third-thing).

The host runner is the red-green-refactor loop. The board run is the
guarantee. It runs the portable suites too, because passing on a laptop does
not prove the Xtensa build behaves the same.

Release images contain no test code: the suites are never compiled in. See
[`Build-Variants.md`](Build-Variants.md#release-builds-contain-no-test-code).

## Related

- [`Build-Variants.md`](Build-Variants.md): what RELEASE, DEVELOPMENT and SELFTEST gate, and the scope choice.
- [`Building-an-App.md`](Building-an-App.md): how an app plugs into the shell, and the folder layout app suites assume.
- [`tools/Render-Harness.md`](tools/Render-Harness.md): rendering a real screen on a host and diffing it against a capture.
- [`notes/README.md`](notes/README.md): the hardware constraints behind the device-only performance tests.
