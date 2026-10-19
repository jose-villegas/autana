# power

The AXP2101 owner uses the board I2C bus at 400 kHz. Key interrupts are
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

`BATTERY` reads fresh register bytes on the development console task.
`autana battery --json` exposes the decoded fields and raw status bytes.
