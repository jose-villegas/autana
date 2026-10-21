# verbs

One file per command the console understands, each registering itself in the console's verb table, with the pure parsing of its arguments in a header beside it.

`BATTERY` reads the PMU fresh and replies with the decoded snapshot and raw
status bytes; formatting is in `console_battery_reply.h`.

`codec <reg> [value]` reads a codec register, or writes it before reading it
back. Arguments are decimal or `0x` hexadecimal bytes. The development-only
verb requires an open codec and replies `CODEC reg=0xNN value=0xNN` on success.
