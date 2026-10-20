# power

The AXP2101 owner uses the board I2C bus. Key interrupts are
polled by input; battery snapshots refresh at `POWER_BATTERY_PERIOD_US`.
Battery reads never change gauge, ADC, charging or power-off settings.

| Function | Caller | I2C | Lock |
|---|---|---|---|
| `power_start()` | `buttons_start()`, once at boot | yes | none |
| `power_poll()` | the input polling task only, once per tick | yes, outside the lock | spinlock for the store |
| `power_read_battery()` | any task | no | spinlock for the copy out |
| `power_read_battery_now()` | POST and the console task | yes, outside the lock | spinlock for the store |

POST may create the device handle before input starts. `power_start()`
configures key interrupts and logs the PMU settings once input starts.

The cache keeps the newest `read_us`, including failed reads marked invalid.
A load copies the whole snapshot under its lock. Register decoding and cache
ordering are portable and covered by the `power` suite.

The cache's first reader is planned for a later UI; no UI reads it yet.

## Persistent history

`power_log.h` owns the record format and cadence. The existing settings
service stores signed 64-bit records in the `power_log` NVS namespace,
partition `nvs`; keys run from `r000` through `r239` (`POWER_LOG_SLOTS`).

| Field | Bits | Meaning |
|---|---:|---|
| Sequence | `POWER_LOG_SEQ_BITS` (24) | Wraps modulo the field range |
| Uptime minutes | `POWER_LOG_UPTIME_BITS` (10) | Saturates at `POWER_LOG_UPTIME_MAX` |
| Voltage | `POWER_LOG_MV_BITS` (14) | Battery millivolts |
| Percentage | `POWER_LOG_PERCENT_BITS` (7) | Gauge percentage |
| USB, battery | `POWER_LOG_USB_BITS`, `POWER_LOG_BATTERY_BITS` (1 each) | Presence flags |
| Charge, flow | `POWER_LOG_CHARGE_BITS` (3), `POWER_LOG_FLOW_BITS` (2) | Decoded PMU states |
| Reason | `POWER_LOG_REASON_BITS` (2) | Periodic, transition or boot |

Boot records the first valid snapshot. USB presence, battery presence or
charge changes record a transition. Without USB, `POWER_LOG_PERIOD_US`
(60 seconds) between records triggers a periodic sample.

A transition restarts the periodic interval. Unchanged USB-powered readings
write nothing. Invalid reads write nothing. A failed write warns once and
does not interrupt key polling; its next attempt follows the same cadence.

Startup reads all slots and finds the newest sequence across wrap. The next
write uses the following slot; no head key is stored. Dumps sort present
records oldest first, including gaps. CLEAR erases only this namespace.

The input task uses its existing battery refresh, with no additional I2C.
Each record adds about 1–2 ms of flash writing. A mutex serializes writes,
console snapshots and CLEAR; console output runs after releasing it.

## Flash wear

One packed record occupies one 32-byte NVS entry. A 4 KB NVS page holds 126
entries, and the `nvs` partition rotates six pages. With one spare page,
`POWER_LOG_SLOTS` uses 240 of about 630 usable entries (5 × 126).

At the `POWER_LOG_PERIOD_US` battery-only cadence, filling a page takes
126 × 60 seconds ≈ 2.1 hours. Each of six pages is erased about every
6 × 2.1 = 12.6 hours: 365 × 24 / 12.6 ≈ 700 erases per page per year.

That periodic-only estimate is below the flash's 100,000-cycle endurance.
Transitions, boots and other settings writes add wear; USB-powered idle
periods add none. Retained capacity is `POWER_LOG_SLOTS` records, regardless
of the mix of reasons.
