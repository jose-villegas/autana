# verbs

One file per command the console understands, each registering itself in the console's verb table, with the pure parsing of its arguments in a header beside it.

`BATTERY` performs a fresh PMU read and replies with validity, USB and battery
presence, millivolts, percentage, charge phase, current flow and raw REG00/REG01.
Its pure reply formatter is `console_battery_reply.h`.
