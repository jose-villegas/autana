# verbs

One file per command the console understands, each registering itself in the console's verb table, with the pure parsing of its arguments in a header beside it.

`BATTERY` reads the PMU fresh and replies with the decoded snapshot and raw
status bytes; formatting is in `console_battery_reply.h`.
