# State that every device test starts from

A device suite runs on the real shell, which owns hardware state a test can change. The runner
puts that state back around every test, so a test never inherits what an earlier test, or the
screen before it, left behind.

## What is reset

| State | Reset to | Set by a test through |
|---|---|---|
| Panel clock | the shell's own clock choice | `gfx_set_panel_clock_hz()` |
| Heal queue and settings | their defaults | the heal controls in `gfx_heal.h` |

Both are restored by `display_restore_system_state()`
([`display_device.c:92`](../../launcher/main/display/display_device.c#L92)).

```mermaid
flowchart LR
    S["setUp"] -->|"restore"| T["test body"]
    T --> D["tearDown"]
    D -->|"restore, then judge"| N["next test"]
```

Unity calls `setUp` before and `tearDown` after each test, and `tearDown` runs even when an
assert aborted the test, so a failed test cannot leak its state either
([`selftest.c:41`](../../launcher/main/selftest/selftest.c#L38)).

## Pinning a clock for a timing test

A timed present should measure a known clock, not the system's choice. A suite pins one in its
fixture with [`panel_clock_pin()`](../../launcher/test/panel_clock_pin.h), which sets the clock
and presents once so the link reopen falls outside the timed window. The runner's `tearDown`
puts the system clock back, so the suite does not undo it.

The gfx suite's budgets are pegged with this pinned clock and the default heal state
([`suite_gfx.c:52`](../../launcher/test/suites/suite_gfx.c#L52)); see
[Performance and budgets](Performance-and-Budgets.md) for how a ceiling is set.

## Writing a test

| Do | Do not |
|---|---|
| Pin what you measure in a fixture function | call the restore yourself; the runner does |
| | assume the previous test's clock |

The gfx suite's fixture also clears the clip rect, which its tests share, and no test there
may deinitialise gfx ([`suite_gfx.c:58`](../../launcher/test/suites/suite_gfx.c#L58)).

See [Writing a test](Writing-a-Test.md) for the rest.
