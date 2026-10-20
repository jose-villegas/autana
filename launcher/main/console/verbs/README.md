# verbs

One file per command the console understands, each registering itself in the console's verb table, with the pure parsing of its arguments in a header beside it.

`BATTERY` reads the PMU fresh and replies with the decoded snapshot and raw
status bytes; formatting is in `console_battery_reply.h`.

| Command | Reply |
|---|---|
| `BATTERY` | Fresh snapshot and raw status bytes |
| `BATTERY LOG` | Oldest-first `BATLOG` records, then `BATLOG end count=N` |
| `BATTERY LOG CLEAR` | Erases the history namespace; `BATLOG cleared` on success |

Log rows carry `seq`, `up_min`, `mv`, `percent`, `usb`, `battery`, `charge`,
`flow` and `reason`. Parsing and formatting share `console_battery_reply.h`.
Storage failures reply `BATLOG error read` or `BATLOG error clear`.
